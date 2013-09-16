/* GLIB - Library of useful routines for C programming
 *
 * gmain-epoll.c: Main loop backend using epoll()
 * Copyright (C) 2013  Mikhail Zabaluev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#ifdef HAVE_SYS_EPOLL_H

#include "gmain-internal.h"

#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "ghash.h"
#include "gmessages.h"
#include "gpoll.h"
#include "gslice.h"
#include "gstrfuncs.h"
#include "gtestutils.h"

typedef struct _GEpollLoopBackend GEpollLoopBackend;

struct _GEpollLoopBackend
{
  GMainContext *context;

  int epoll_fd;

  guint n_poll_records;

  struct epoll_event *epoll_output;
  gsize               epoll_output_size;

  GPollFD *fds_ready;
  gsize    fds_ready_size;

  GHashTable *compat_fds;

  GMutex mutex;
};

#define LOCK_BACKEND(backend) g_mutex_lock (&backend->mutex)
#define UNLOCK_BACKEND(backend) g_mutex_unlock (&backend->mutex)

static gpointer g_epoll_context_create      (gpointer user_data);
static void     g_epoll_context_set_context (gpointer backend_data,
                                             GMainContext *context);
static void     g_epoll_context_free        (gpointer backend_data);
static gboolean g_epoll_context_acquire     (gpointer backend_data);
static gboolean g_epoll_context_iterate     (gpointer backend_data,
                                             gboolean block,
                                             gboolean dispatch);
static gboolean g_epoll_context_add_fd      (gpointer backend_data,
                                             gint     fd,
                                             gushort  events,
                                             gint     priority);
static gboolean g_epoll_context_modify_fd   (gpointer backend_data,
                                             gint     fd,
                                             gushort  events,
                                             gint     priority);
static gboolean g_epoll_context_remove_fd   (gpointer backend_data,
                                             gint     fd);

static int g_epoll_context_poll (GEpollLoopBackend *backend, gint timeout);

GMainContextFuncs _g_main_epoll_context_funcs =
{
  g_epoll_context_create,
  g_epoll_context_set_context,
  g_epoll_context_free,
  g_epoll_context_acquire,
  NULL, /* release */
  g_epoll_context_iterate,
  g_epoll_context_add_fd,
  g_epoll_context_modify_fd,
  g_epoll_context_remove_fd,
};

static inline gushort
g_io_condition_from_epoll_events (uint32_t epoll_events)
{
  gushort io_cond = epoll_events & (0
#if G_IO_IN == EPOLLIN
        |G_IO_IN
#endif
#if G_IO_OUT == EPOLLOUT
        |G_IO_OUT
#endif
#if G_IO_PRI == EPOLLPRI
        |G_IO_PRI
#endif
#if G_IO_ERR == EPOLLERR
        |G_IO_ERR
#endif
#if G_IO_HUP == EPOLLHUP
        |G_IO_HUP
#endif
      );
#if G_IO_IN != EPOLLIN
  if ((epoll_events & EPOLLIN) != 0)
    io_cond |= G_IO_IN;
#endif
#if G_IO_OUT != EPOLLOUT
  if ((epoll_events & EPOLLOUT) != 0)
    io_cond |= G_IO_OUT;
#endif
#if G_IO_PRI != EPOLLPRI
  if ((epoll_events & EPOLLPRI) != 0)
    io_cond |= G_IO_PRI;
#endif
#if G_IO_ERR != EPOLLERR
  if ((epoll_events & EPOLLERR) != 0)
    io_cond |= G_IO_ERR;
#endif
#if G_IO_HUP != EPOLLHUP
  if ((epoll_events & EPOLLHUP) != 0)
    io_cond |= G_IO_HUP;
#endif
  return io_cond;
}

static inline uint32_t
g_io_condition_to_epoll_events (gushort io_cond)
{
  uint32_t epoll_mask = io_cond & (0
#if G_IO_IN == EPOLLIN
        |EPOLLIN
#endif
#if G_IO_OUT == EPOLLOUT
        |EPOLLOUT
#endif
#if G_IO_PRI == EPOLLPRI
        |EPOLLPRI
#endif
      );
#if G_IO_IN != EPOLLIN
  if ((io_cond & G_IO_IN) != 0)
    epoll_mask |= EPOLLIN;
#endif
#if G_IO_OUT != EPOLLOUT
  if ((io_cond & G_IO_OUT) != 0)
    epoll_mask |= EPOLLOUT;
#endif
#if G_IO_PRI != EPOLLPRI
  if ((io_cond & G_IO_PRI) != 0)
    epoll_mask |= EPOLLPRI;
#endif
  return epoll_mask;
}

static gpointer
g_epoll_context_create (G_GNUC_UNUSED gpointer user_data)
{
  GEpollLoopBackend *backend;
  int epoll_fd;

#ifdef HAVE_EPOLL_CREATE1
  epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
#else
  epoll_fd = epoll_create (1);
#endif

  if (G_UNLIKELY (epoll_fd == -1))
    {
      g_warning ("epoll_create failed: %s", g_strerror (errno));
      return NULL;
    }

#ifndef HAVE_EPOLL_CREATE1
  {
    int flags;

    flags = fcntl (epoll_fd, F_GETFD);
    if (flags >= 0)
      {
        flags |= FD_CLOEXEC;
        fcntl (epoll_fd, F_SETFD, flags);
      }
  }
#endif

#ifdef G_MAIN_POLL_DEBUG
  if (_g_main_poll_debug)
    g_print ("epoll %d created (thread %p)\n",
             epoll_fd, g_thread_self ());
#endif

  backend = g_slice_new0 (GEpollLoopBackend);

  backend->epoll_fd = epoll_fd;

  g_atomic_int_set (&backend->n_poll_records, 0);

  backend->compat_fds = g_hash_table_new (g_direct_hash, g_direct_equal);

  g_mutex_init (&backend->mutex);

  return backend;
}

static void
g_epoll_context_set_context (gpointer backend_data,
                             GMainContext *context)
{
  GEpollLoopBackend *backend = backend_data;

  backend->context = context;
}

static void
g_epoll_context_free (gpointer backend_data)
{
  GEpollLoopBackend *backend = backend_data;

  close (backend->epoll_fd);

#ifdef G_MAIN_POLL_DEBUG
  if (_g_main_poll_debug)
    g_print ("epoll %d closed (thread %p)\n",
             backend->epoll_fd, g_thread_self ());
#endif

  g_mutex_clear (&backend->mutex);

  g_free (backend->fds_ready);
  g_free (backend->epoll_output);
  g_hash_table_destroy (backend->compat_fds);

  g_slice_free (GEpollLoopBackend, backend);
}

static gboolean
g_epoll_context_acquire (gpointer backend_data)
{
  /* Built-in main loops can always be acquired */
  return TRUE;
}

static void
g_epoll_context_ensure_ready_size (GEpollLoopBackend *backend, gsize needed)
{
  if (needed > backend->fds_ready_size)
    {
      backend->fds_ready_size = needed;
      backend->fds_ready = g_renew (GPollFD, backend->fds_ready, needed);
    }
}

static guint
g_epoll_context_query_compat_fds (GEpollLoopBackend *backend)
{
  guint n_compat_fds;
  GHashTableIter compat_iter;
  gpointer key, value;
  GPollFD *pollfd;

  LOCK_BACKEND (backend);

  n_compat_fds = g_hash_table_size (backend->compat_fds);

  if (n_compat_fds != 0)
    {
      g_epoll_context_ensure_ready_size (backend, n_compat_fds);

      pollfd = backend->fds_ready;
      g_hash_table_iter_init (&compat_iter, backend->compat_fds);
      while (g_hash_table_iter_next (&compat_iter, &key, &value))
        {
          pollfd->fd = GPOINTER_TO_INT (key);
          pollfd->events = GPOINTER_TO_UINT (value);
          pollfd->revents = 0;
          ++pollfd;
        }
    }

  UNLOCK_BACKEND (backend);

  return n_compat_fds;
}

static gboolean
g_epoll_context_iterate (gpointer backend_data,
                         gboolean block,
                         gboolean dispatch)
{
  GEpollLoopBackend *backend = backend_data;
  gint max_priority;
  gint timeout;
  gint n_ready;
  gint i;
  GPollFD *pollfd;
  guint n_compat_fds;
  gboolean sources_ready;

  g_main_context_prepare (backend->context, &max_priority);

  /* If the application has added descriptors that are not meaningfully
   * pollable, we should still serve them as per poll(2) semantics. */

  n_compat_fds = g_epoll_context_query_compat_fds (backend);

  if (n_compat_fds != 0)
    {
      if (g_poll (backend->fds_ready, n_compat_fds, 0) > 0)
        block = FALSE;
    }

  /* Could do some optimizations here, like noticing that none of our records
   * have priority equal or higher than max_priority and skipping the poll,
   * or ignoring fds that are falling behind max_priority.
   * But that would require extra bookkeeping on the backend,
   * more if we want to accurately update it when poll records are removed.
   * Neither do we keep track in the GMainContext to lower priority
   * when redundant GPollFDs are removed. So instead g_main_context_check()
   * is to ignore out-of-priority GPollFDs apart from updating their
   * revents fields.
   */

  timeout = block? g_main_context_get_poll_timeout (backend->context) : 0;

  n_ready = g_epoll_context_poll (backend, timeout);
  if (n_ready < 0)
    {
      if (G_UNLIKELY (errno != EINTR))
        g_warning ("epoll_wait failed: %s", g_strerror (errno));
      n_ready = 0;
    }

  g_epoll_context_ensure_ready_size (backend, n_compat_fds + (guint) n_ready);

  pollfd = backend->fds_ready + n_compat_fds;
  for (i = 0; i < n_ready; i++)
    {
      const struct epoll_event *ev = backend->epoll_output + i;
      pollfd->fd = ev->data.fd;
      pollfd->events = G_IO_IN|G_IO_OUT|G_IO_PRI;
      pollfd->revents = g_io_condition_from_epoll_events (ev->events);
      ++pollfd;
    }

  sources_ready = g_main_context_check (backend->context, max_priority,
      backend->fds_ready, n_compat_fds + n_ready);

  if (dispatch && sources_ready)
    g_main_context_dispatch (backend->context);

  return sources_ready;
}

static int
g_epoll_context_poll (GEpollLoopBackend *backend, gint timeout)
{
  gsize nfds;

  nfds = g_atomic_int_get (&backend->n_poll_records);

  /* Must pass a nonzero value to epoll_wait */
  if (nfds == 0)
    ++nfds;

  if (backend->epoll_output_size < nfds)
    {
      backend->epoll_output_size = nfds;
      backend->epoll_output = g_renew (struct epoll_event,
          backend->epoll_output, nfds);
    }

  return epoll_wait (backend->epoll_fd, backend->epoll_output, (int) nfds,
                     timeout);
}

static gboolean
g_epoll_context_add_fd (gpointer backend_data,
                        gint     fd,
                        gushort  events,
                        G_GNUC_UNUSED gint priority)
{
  GEpollLoopBackend *backend = backend_data;
  struct epoll_event ev = { 0, };
  int retval;

  ev.events = g_io_condition_to_epoll_events (events);
  ev.data.fd = fd;
  retval = epoll_ctl (backend->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

#ifdef G_MAIN_POLL_DEBUG
  if (_g_main_poll_debug)
    g_print ("epoll %d ADD fd=%d returned %d (thread %p)\n",
             backend->epoll_fd, fd, retval, g_thread_self ());
#endif

  if (retval != 0)
    {
      if (errno == EPERM)
        {
          /* epoll does not think this descriptor is pollable */

          LOCK_BACKEND (backend);

          g_hash_table_replace (backend->compat_fds,
              GINT_TO_POINTER (fd), GUINT_TO_POINTER (events));

          UNLOCK_BACKEND (backend);

          return TRUE;
        }
      else
        {
          g_warning ("EPOLL_CTL_ADD failed: %s", g_strerror(errno));
          return FALSE;
        }
    }

  g_atomic_int_inc (&backend->n_poll_records);

  return TRUE;
}

static gboolean
g_epoll_context_modify_fd (gpointer backend_data,
                           gint     fd,
                           gushort  events,
                           G_GNUC_UNUSED gint priority)
{
  GEpollLoopBackend *backend = backend_data;
  struct epoll_event ev = { 0, };
  gboolean is_compat;
  int retval;

  LOCK_BACKEND (backend);

  is_compat = g_hash_table_contains (backend->compat_fds,
      GINT_TO_POINTER (fd));

  if (is_compat)
    g_hash_table_replace (backend->compat_fds,
        GINT_TO_POINTER (fd), GUINT_TO_POINTER (events));

  UNLOCK_BACKEND (backend);

  if (is_compat)
    return TRUE;

  ev.events = g_io_condition_to_epoll_events (events);
  ev.data.fd = fd;
  retval = epoll_ctl (backend->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
  if (retval != 0)
    {
      g_warning ("EPOLL_CTL_MOD failed: %s", g_strerror(errno));
      return FALSE;
    }

  return TRUE;
}

static gboolean
g_epoll_context_remove_fd (gpointer backend_data, gint fd)
{
  GEpollLoopBackend *backend = backend_data;
  struct epoll_event dummy_ev = { 0, };
  gboolean was_compat_fd;
  int retval;

  LOCK_BACKEND (backend);

  was_compat_fd = g_hash_table_remove (backend->compat_fds,
      GINT_TO_POINTER (fd));

  UNLOCK_BACKEND (backend);

  if (was_compat_fd)
    return TRUE;

  g_atomic_int_dec_and_test (&backend->n_poll_records);

  retval = epoll_ctl (backend->epoll_fd, EPOLL_CTL_DEL, fd, &dummy_ev);

#ifdef G_MAIN_POLL_DEBUG
  if (_g_main_poll_debug)
    g_print ("epoll %d DEL fd=%d returned %d (thread %p)\n",
             backend->epoll_fd, fd, retval, g_thread_self ());
#endif

  if (retval != 0)
    {
      /* Removing or blocking a source after its fd has been closed
       * is normal usage. ENOENT or EPERM can apparently occur if the
       * descriptor has been reclaimed by another kernel object.
       * Other errors should be logged.
       */
      if (G_UNLIKELY (errno != EBADF && errno != ENOENT && errno != EPERM))
        g_warning ("EPOLL_CTL_DEL failed: %s", g_strerror(errno));
      return FALSE;
    }

  return TRUE;
}

#endif /* HAVE_SYS_EPOLL_H */
