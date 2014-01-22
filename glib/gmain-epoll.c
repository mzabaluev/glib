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
#include <string.h>

#include "ghash.h"
#include "gmessages.h"
#include "gpoll.h"
#include "gslice.h"
#include "gstrfuncs.h"
#include "gtestutils.h"

struct _GEpollerData
{
  int epoll_fd;

  guint n_poll_records;

  struct epoll_event *epoll_output;
  gsize               epoll_output_size;

  GPollFD *fds_ready;
  gsize    fds_ready_size;

  guint n_compat_fds;
};

static void     g_epoller_destroy   (gpointer   backend_data);
static void     g_epoller_add_fd    (gpointer   backend_data,
                                     gint       fd,
                                     gushort    events,
                                     gint       priority);
static void     g_epoller_modify_fd (gpointer   backend_data,
                                     gint       fd,
                                     gushort    events,
                                     gint       priority);
static void     g_epoller_remove_fd (gpointer   backend_data,
                                     gint       fd);
static void     g_epoller_reset     (gpointer   backend_data);
static void     g_epoller_iterate   (gpointer   backend_data,
                                     GMainLoop *loop);

static int g_epoller_poll (GEpollerData *backend, gint timeout);

GPollerFuncs _g_epoller_funcs =
{
  NULL,  /* start */
  g_epoller_destroy,
  g_epoller_add_fd,
  g_epoller_modify_fd,
  g_epoller_remove_fd,
  g_epoller_reset,
  g_epoller_iterate,
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

static gint
g_epoll_create (void)
{
  int epoll_fd;

#ifdef HAVE_EPOLL_CREATE1
  epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
#else
  epoll_fd = epoll_create (1);
#endif

  if (G_UNLIKELY (epoll_fd < 0))
    {
      g_warning ("epoll_create failed: %s", g_strerror (errno));
      return -1;
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

  return epoll_fd;
}

GEpollerData *
_g_epoller_new (void)
{
  GEpollerData *backend;
  int epoll_fd;

  epoll_fd = g_epoll_create ();

  if (G_UNLIKELY (epoll_fd < 0))
    return NULL;

  backend = g_slice_new0 (GEpollerData);

  backend->epoll_fd = epoll_fd;

  return backend;
}

static void
g_epoller_destroy (gpointer backend_data)
{
  GEpollerData *backend = backend_data;

  close (backend->epoll_fd);

  g_free (backend->fds_ready);
  g_free (backend->epoll_output);

  g_slice_free (GEpollerData, backend);
}

static void
g_epoller_reset (gpointer backend_data)
{
  GEpollerData *backend = backend_data;

  if (backend->n_poll_records != 0)
    {
      close (backend->epoll_fd);
      backend->epoll_fd = g_epoll_create ();
      backend->n_poll_records = 0;
    }

  backend->n_compat_fds = 0;
}

static void
g_epoller_ensure_ready_size (GEpollerData *backend, gsize needed)
{
  if (needed > backend->fds_ready_size)
    {
      backend->fds_ready_size = needed;
      backend->fds_ready = g_renew (GPollFD, backend->fds_ready, needed);
    }
}

static void
g_epoller_iterate (gpointer backend_data, GMainLoop *loop)
{
  GEpollerData *backend = backend_data;
  gint max_priority;
  gint timeout;
  gint n_ready;
  gint n_fds_total;
  gint i;
  GPollFD *pollfd;

  timeout = g_main_loop_prepare_poll (loop, &max_priority);

  /* If the application has added descriptors that are not meaningfully
   * pollable, we should still serve them as per poll(2) semantics. */

  if (backend->n_compat_fds != 0)
    {
      if (g_poll (backend->fds_ready, backend->n_compat_fds, 0) > 0)
        timeout = 0;
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

  n_ready = g_epoller_poll (backend, timeout);
  if (n_ready < 0)
    {
      if (G_UNLIKELY (errno != EINTR))
        g_warning ("epoll_wait failed: %s", g_strerror (errno));
      n_ready = 0;
    }

  n_fds_total = backend->n_compat_fds + n_ready;
  g_epoller_ensure_ready_size (backend, n_fds_total);

  pollfd = backend->fds_ready + backend->n_compat_fds;
  for (i = 0; i < n_ready; i++)
    {
      const struct epoll_event *ev = &backend->epoll_output[i];
      pollfd->fd = ev->data.fd;
      pollfd->events = G_IO_IN|G_IO_OUT|G_IO_PRI;
      pollfd->revents = g_io_condition_from_epoll_events (ev->events);
      ++pollfd;
    }

  g_main_loop_process_poll (loop, max_priority,
                            backend->fds_ready, n_fds_total);
}

static int
g_epoller_poll (GEpollerData *backend, gint timeout)
{
  guint nfds = backend->n_poll_records;

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

static void
g_epoller_add_fd (gpointer backend_data,
                  gint     fd,
                  gushort  events,
                  G_GNUC_UNUSED gint priority)
{
  GEpollerData *backend = backend_data;
  struct epoll_event ev = { 0, };
  int retval;

  ev.events = g_io_condition_to_epoll_events (events);
  ev.data.fd = fd;
  retval = epoll_ctl (backend->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

  if (G_UNLIKELY (retval != 0))
    {
      if (errno == EPERM)
        {
          /* epoll does not think this descriptor is pollable */

          guint i_next = backend->n_compat_fds++;
          GPollFD *pollfd;

          g_epoller_ensure_ready_size (backend, i_next + 1);

          pollfd = &backend->fds_ready[i_next];
          pollfd->fd = fd;
          pollfd->events = events;
          pollfd->revents = 0;
        }
      else
        {
          g_warning ("EPOLL_CTL_ADD failed: %s", g_strerror(errno));
        }
      return;
    }

  ++backend->n_poll_records;
}

static void
g_epoller_modify_fd (gpointer backend_data,
                     gint     fd,
                     gushort  events,
                     G_GNUC_UNUSED gint priority)
{
  GEpollerData *backend = backend_data;
  struct epoll_event ev = { 0, };
  guint i;
  int retval;

  for (i = 0; i < backend->n_compat_fds; i++)
    {
      if (backend->fds_ready[i].fd == fd)
        {
          backend->fds_ready[i].events = events;
          return;
        }
    }

  ev.events = g_io_condition_to_epoll_events (events);
  ev.data.fd = fd;
  retval = epoll_ctl (backend->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
  if (G_UNLIKELY (retval != 0))
    g_warning ("EPOLL_CTL_MOD failed: %s", g_strerror(errno));
}

static void
g_epoller_remove_fd (gpointer backend_data, gint fd)
{
  GEpollerData *backend = backend_data;
  struct epoll_event dummy_ev = { 0, };
  guint i;
  int retval;

  for (i = 0; i < backend->n_compat_fds; i++)
    {
      if (backend->fds_ready[i].fd == fd)
        {
          memmove (backend->fds_ready + i,
                   backend->fds_ready + i + 1,
                   backend->n_compat_fds - 1 - i);
          --backend->n_compat_fds;
          return;
        }
    }

  --backend->n_poll_records;

  retval = epoll_ctl (backend->epoll_fd, EPOLL_CTL_DEL, fd, &dummy_ev);

  if (retval != 0)
    {
      /* Removing or blocking a source after its fd has been closed
       * is normal usage. ENOENT or EPERM can apparently occur if the
       * descriptor has been reclaimed by another kernel object.
       * Other errors should be logged.
       */
      if (G_UNLIKELY (errno != EBADF && errno != ENOENT && errno != EPERM))
        g_warning ("EPOLL_CTL_DEL failed: %s", g_strerror(errno));
    }
}

#endif /* HAVE_SYS_EPOLL_H */
