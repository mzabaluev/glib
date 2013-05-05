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

typedef struct _GPollRec GPollRec;

struct _GPollRec
{
  GPollRec *prev;
  GPollRec *next;
  gint fd;
  gint priority;
  gushort events;
};

struct _GBaselinePollerData
{
  GPollRec *poll_records, *poll_records_tail;
  guint n_poll_records;
  GPollFD *cached_poll_array;
  guint cached_poll_array_size;
  GPollFunc poll_func;
};

static void     g_baseline_poller_destroy   (gpointer   backend_data);
static void     g_baseline_poller_add_fd    (gpointer   backend_data,
                                             gint       fd,
                                             gushort    events,
                                             gint       priority);
static void     g_baseline_poller_modify_fd (gpointer   backend_data,
                                             gint       fd,
                                             gushort    events,
                                             gint       priority);
static void     g_baseline_poller_remove_fd (gpointer   backend_data,
                                             gint       fd);
static void     g_baseline_poller_reset     (gpointer   backend_data);
static void     g_baseline_poller_iterate   (gpointer   backend_data,
                                             GMainLoop *loop);

static gint     g_baseline_poller_query (GBaselinePollerData *backend,
                                         gint     max_priority,
                                         GPollFD *fds,
                                         gint     n_fds);
static void     g_baseline_poller_poll  (GBaselinePollerData *backend,
                                         gint              timeout,
                                         gint              priority,
                                         GPollFD          *fds,
                                         gint              n_fds);

GPollerFuncs _g_baseline_poller_funcs =
{
  NULL,  /* start */
  g_baseline_poller_destroy,
  g_baseline_poller_add_fd,
  g_baseline_poller_modify_fd,
  g_baseline_poller_remove_fd,
  g_baseline_poller_reset,
  g_baseline_poller_iterate,
};

GBaselinePollerData *
_g_baseline_poller_new (GPollFunc func)
{
  GBaselinePollerData *backend;

  backend = g_slice_new0 (GBaselinePollerData);
  backend->poll_func = func;
  return backend;
}

static void
g_baseline_poller_destroy (gpointer backend_data)
{
  GBaselinePollerData *backend = backend_data;

  g_slice_free_chain (GPollRec, backend->poll_records, next);

  g_free (backend->cached_poll_array);

  g_slice_free (GBaselinePollerData, backend);
}

static void
g_baseline_poller_reset (gpointer backend_data)
{
  GBaselinePollerData *backend = backend_data;

  g_slice_free_chain (GPollRec, backend->poll_records, next);
  backend->poll_records = NULL;
  backend->poll_records_tail = NULL;
  backend->n_poll_records = 0;
}

static void
g_baseline_poller_iterate (gpointer   backend_data,
                           GMainLoop *loop)
{
  GBaselinePollerData *backend = backend_data;
  gint max_priority;
  gint timeout;
  gint nfds, allocated_nfds;
  GPollFD *fds = NULL;

  timeout = g_main_loop_prepare_poll (loop, &max_priority);

  if (!backend->cached_poll_array)
    {
      backend->cached_poll_array_size = backend->n_poll_records;
      backend->cached_poll_array = g_new (GPollFD, backend->n_poll_records);
    }

  allocated_nfds = backend->cached_poll_array_size;
  fds = backend->cached_poll_array;

  while ((nfds = g_baseline_poller_query (backend, max_priority, fds,
                                          allocated_nfds)) > allocated_nfds)
    {
      backend->cached_poll_array_size = allocated_nfds = nfds;
      backend->cached_poll_array = fds = g_renew (GPollFD, fds, nfds);
    }

  g_baseline_poller_poll (backend, timeout, max_priority, fds, nfds);

  g_main_loop_process_poll (loop, max_priority, fds, nfds);
}

static void
g_baseline_poller_poll (GBaselinePollerData *backend,
                        gint                 timeout,
                        gint                 priority,
                        GPollFD             *fds,
                        gint                 n_fds)
{
#ifdef  G_MAIN_POLL_DEBUG
  GTimer *poll_timer = NULL;
#endif

  if (n_fds || timeout != 0)
    {
#ifdef  G_MAIN_POLL_DEBUG
      if (_g_main_poll_debug)
        {
          g_print ("polling context=%p n=%d timeout=%d\n",
                   backend->context, n_fds, timeout);
          poll_timer = g_timer_new ();
        }
#endif

      if (backend->poll_func (fds, n_fds, timeout) < 0 && errno != EINTR)
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

static void
g_baseline_poller_add_fd (gpointer backend_data,
                          gint     fd,
                          gushort  events,
                          gint     priority)
{
  GBaselinePollerData *backend = backend_data;
  GPollRec *prevrec, *nextrec;
  GPollRec *newrec = g_slice_new (GPollRec);

  newrec->fd = fd;
  newrec->events = events;
  newrec->priority = priority;

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
}

static void
g_baseline_poller_remove_fd (gpointer backend_data, gint fd)
{
  GBaselinePollerData *backend = backend_data;
  GPollRec *pollrec, *prevrec, *nextrec;

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

  g_return_if_fail (pollrec != NULL);
}

static void
g_baseline_poller_modify_fd (gpointer backend_data,
                             gint     fd,
                             gushort  events,
                             gint     priority)
{
  GBaselinePollerData *backend = backend_data;
  GPollRec *pollrec;

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

  g_return_if_fail (pollrec != NULL);
}

static gint
g_baseline_poller_query (GBaselinePollerData *backend,
                         gint                 max_priority,
                         GPollFD             *fds,
                         gint                 n_fds)
{
  GPollRec *pollrec = backend->poll_records;
  gint n_poll = 0;

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

  return n_poll;
}

void
_g_baseline_poller_set_poll_func (GBaselinePollerData *backend, GPollFunc func)
{
  backend->poll_func = func;
}

GPollFunc
_g_baseline_poller_get_poll_func (const GBaselinePollerData *backend)
{
  return backend->poll_func;
}
