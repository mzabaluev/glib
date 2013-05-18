/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * gmain-poll.c: Main loop backend using poll()
 * Copyright 1998 Owen Taylor
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

#include "gmain-internal.h"

#include <errno.h>

#include "gmessages.h"
#include "gslice.h"
#include "gstrfuncs.h"

#ifdef  G_MAIN_POLL_DEBUG
#include "gtimer.h"
#endif

typedef struct _GPollLoopBackend GPollLoopBackend;
typedef struct _GPollRec GPollRec;

struct _GPollRec
{
  GPollRec *prev;
  GPollRec *next;
  gint fd;
  gint priority;
  gushort events;
};

struct _GPollLoopBackend
{
  GMainContext *context;

  GMutex mutex;

  GPollRec *poll_records, *poll_records_tail;
  guint n_poll_records;
  GPollFD *cached_poll_array;
  guint cached_poll_array_size;

  /* Flag indicating whether the set of fd's changed during a poll */
  gboolean poll_changed;

  GPollFunc poll_func;
};

#define LOCK_BACKEND(backend) g_mutex_lock (&backend->mutex)
#define UNLOCK_BACKEND(backend) g_mutex_unlock (&backend->mutex)

static gpointer g_poll_context_create      (gpointer user_data);
static void     g_poll_context_set_context (gpointer backend_data,
                                            GMainContext *context);
static void     g_poll_context_free        (gpointer backend_data);
static gboolean g_poll_context_acquire     (gpointer backend_data);
static gboolean g_poll_context_iterate     (gpointer backend_data,
                                            gboolean block);
static gboolean g_poll_context_add_fd      (gpointer backend_data,
                                            gint     fd,
                                            gushort  events,
                                            gint     priority);
static gboolean g_poll_context_modify_fd   (gpointer backend_data,
                                            gint     fd,
                                            gushort  events,
                                            gint     priority);
static gboolean g_poll_context_remove_fd   (gpointer backend_data,
                                            gint     fd);

static gint     g_poll_context_query     (GPollLoopBackend *backend,
                                          gint     max_priority,
                                          GPollFD *fds,
                                          gint     n_fds);
static void     g_poll_context_poll      (GPollLoopBackend *backend,
                                          gint              timeout,
                                          gint              priority,
                                          GPollFD          *fds,
                                          gint              n_fds);

GMainContextFuncs _g_main_poll_context_funcs =
{
  g_poll_context_create,
  g_poll_context_set_context,
  g_poll_context_free,
  g_poll_context_acquire,
  NULL, /* release */
  g_poll_context_iterate,
  g_poll_context_add_fd,
  g_poll_context_modify_fd,
  g_poll_context_remove_fd,
};

static gpointer
g_poll_context_create (G_GNUC_UNUSED gpointer user_data)
{
  GPollLoopBackend *backend;

  backend = g_slice_new0 (GPollLoopBackend);

  g_mutex_init (&backend->mutex);

  backend->poll_func = g_poll;

  backend->cached_poll_array = NULL;
  backend->cached_poll_array_size = 0;

  return backend;
}

static void g_poll_context_set_context (gpointer backend_data,
                                        GMainContext *context)
{
  GPollLoopBackend *backend = backend_data;
  backend->context = context;
}

static void
g_poll_context_free (gpointer backend_data)
{
  GPollLoopBackend *backend = backend_data;

  g_mutex_clear (&backend->mutex);

  g_free (backend->cached_poll_array);

  g_slice_free_chain (GPollRec, backend->poll_records, next);

  g_slice_free (GPollLoopBackend, backend);
}

static gboolean
g_poll_context_acquire (gpointer backend_data)
{
  /* Built-in main loops can always be acquired */
  return TRUE;
}

static gboolean
g_poll_context_iterate (gpointer backend_data,
                        gboolean block)
{
  GPollLoopBackend *backend = backend_data;
  gint max_priority;
  gint timeout;
  gint nfds, allocated_nfds;
  GPollFD *fds = NULL;
  gboolean poll_changed;

  LOCK_BACKEND (backend);

  if (!backend->cached_poll_array)
    {
      backend->cached_poll_array_size = backend->n_poll_records;
      backend->cached_poll_array = g_new (GPollFD, backend->n_poll_records);
    }

  UNLOCK_BACKEND (backend);

  allocated_nfds = backend->cached_poll_array_size;
  fds = backend->cached_poll_array;

  g_main_context_prepare (backend->context, &max_priority);

  while ((nfds = g_poll_context_query (backend, max_priority, fds,
                                       allocated_nfds)) > allocated_nfds)
    {
      g_free (fds);
      backend->cached_poll_array_size = allocated_nfds = nfds;
      backend->cached_poll_array = fds = g_new (GPollFD, nfds);
    }

  timeout = block? g_main_context_get_poll_timeout (backend->context) : 0;

  g_poll_context_poll (backend, timeout, max_priority, fds, nfds);

  /* If the set of poll file descriptors changed, bail out
   * and let the main loop rerun
   */
  LOCK_BACKEND (backend);
  poll_changed = backend->poll_changed;
  UNLOCK_BACKEND (backend);
  if (poll_changed)
    return FALSE;

  return g_main_context_check (backend->context, max_priority, fds, nfds);
}

static void
g_poll_context_poll (GPollLoopBackend *backend,
                     gint              timeout,
                     gint              priority,
                     GPollFD          *fds,
                     gint              n_fds)
{
#ifdef  G_MAIN_POLL_DEBUG
  GTimer *poll_timer = NULL;
#endif

  GPollFunc poll_func;

  if (n_fds || timeout != 0)
    {
      LOCK_BACKEND (backend);

      poll_func = backend->poll_func;

      UNLOCK_BACKEND (backend);

#ifdef  G_MAIN_POLL_DEBUG
      if (_g_main_poll_debug)
        {
          g_print ("polling context=%p n=%d timeout=%d\n",
                   backend->context, n_fds, timeout);
          poll_timer = g_timer_new ();
        }
#endif

      if ((*poll_func) (fds, n_fds, timeout) < 0 && errno != EINTR)
        {
#ifndef G_OS_WIN32
          g_warning ("poll(2) failed due to: %s.",
                     g_strerror (errno));
#else
          /* If g_poll () returns -1, it has already called g_warning() */
#endif
        }

#ifdef  G_MAIN_POLL_DEBUG
      if (_g_main_poll_debug)
        {
          GPollRec *pollrec;

          g_print ("g_main_poll(%d) timeout: %d - elapsed %12.10f seconds",
                   n_fds,
                   timeout,
                   g_timer_elapsed (poll_timer, NULL));
          g_timer_destroy (poll_timer);
          pollrec = backend->poll_records;
          while (pollrec != NULL)
            {
              gint i = 0;
              while (i < n_fds)
                {
                  if (fds[i].fd == pollrec->fd &&
                      pollrec->events &&
                      fds[i].revents)
                    {
                      g_print (" [" G_POLLFD_FORMAT " :", fds[i].fd);
                      if (fds[i].revents & G_IO_IN)
                        g_print ("i");
                      if (fds[i].revents & G_IO_OUT)
                        g_print ("o");
                      if (fds[i].revents & G_IO_PRI)
                        g_print ("p");
                      if (fds[i].revents & G_IO_ERR)
                        g_print ("e");
                      if (fds[i].revents & G_IO_HUP)
                        g_print ("h");
                      if (fds[i].revents & G_IO_NVAL)
                        g_print ("n");
                      g_print ("]");
                    }
                  i++;
                }
              pollrec = pollrec->next;
            }
          g_print ("\n");
        }
#endif
    } /* if (n_fds || timeout != 0) */
}

static gboolean
g_poll_context_add_fd (gpointer backend_data,
                       gint     fd,
                       gushort  events,
                       gint     priority)
{
  GPollLoopBackend *backend = backend_data;
  GPollRec *prevrec, *nextrec;
  GPollRec *newrec = g_slice_new (GPollRec);

  newrec->fd = fd;
  newrec->events = events;
  newrec->priority = priority;

  LOCK_BACKEND (backend);

  prevrec = backend->poll_records_tail;
  nextrec = NULL;
  while (prevrec && priority < prevrec->priority)
    {
      nextrec = prevrec;
      prevrec = prevrec->prev;
    }

  if (prevrec)
    prevrec->next = newrec;
  else
    backend->poll_records = newrec;

  newrec->prev = prevrec;
  newrec->next = nextrec;

  if (nextrec)
    nextrec->prev = newrec;
  else
    backend->poll_records_tail = newrec;

  backend->n_poll_records++;

  backend->poll_changed = TRUE;

  UNLOCK_BACKEND (backend);

  /* Now wake up the main loop if it is waiting in the poll() */
  g_main_context_wakeup (backend->context);

  return TRUE;
}

static gboolean
g_poll_context_remove_fd (gpointer backend_data, gint fd)
{
  GPollLoopBackend *backend = backend_data;
  GPollRec *pollrec, *prevrec, *nextrec;

  LOCK_BACKEND (backend);

  prevrec = NULL;
  pollrec = backend->poll_records;

  while (pollrec)
    {
      nextrec = pollrec->next;
      if (pollrec->fd == fd)
        {
          if (prevrec != NULL)
            prevrec->next = nextrec;
          else
            backend->poll_records = nextrec;

          if (nextrec != NULL)
            nextrec->prev = prevrec;
          else
            backend->poll_records_tail = prevrec;

          g_slice_free (GPollRec, pollrec);

          backend->n_poll_records--;
          break;
        }
      prevrec = pollrec;
      pollrec = nextrec;
    }

  backend->poll_changed = TRUE;

  UNLOCK_BACKEND (backend);

  /* Now wake up the main loop if it is waiting in the poll() */
  g_main_context_wakeup (backend->context);

  return TRUE;
}

static gboolean
g_poll_context_modify_fd (gpointer backend_data,
                          gint     fd,
                          gushort  events,
                          gint     priority)
{
  GPollLoopBackend *backend = backend_data;
  GPollRec *pollrec;

  LOCK_BACKEND (backend);

  pollrec = backend->poll_records;

  while (pollrec != NULL)
    {
      if (pollrec->fd == fd)
        {
          pollrec->events = events;
          pollrec->priority = priority;
          break;
        }
      pollrec = pollrec->next;
    }

  backend->poll_changed = TRUE;

  UNLOCK_BACKEND (backend);

  /* Now wake up the main loop if it is waiting in the poll() */
  g_main_context_wakeup (backend->context);

  return TRUE;
}

static gint
g_poll_context_query (GPollLoopBackend *backend,
                      gint     max_priority,
                      GPollFD *fds,
                      gint     n_fds)
{
  gint n_poll;
  GPollRec *pollrec;

  pollrec = backend->poll_records;
  n_poll = 0;
  while (pollrec && max_priority >= pollrec->priority)
    {
      if (n_poll < n_fds && pollrec->events != 0)
        {
          fds[n_poll].fd = pollrec->fd;
          fds[n_poll].events = pollrec->events;
          fds[n_poll].revents = 0;
        }

      pollrec = pollrec->next;
      n_poll++;
    }

  backend->poll_changed = FALSE;

  return n_poll;
}

void
_g_main_compat_set_poll_func (gpointer backend_data, GPollFunc func)
{
  GPollLoopBackend *backend = backend_data;

  LOCK_BACKEND (backend);
  backend->poll_func = (func != NULL)? func : g_poll;
  UNLOCK_BACKEND (backend);
}
