/* GLIB - Library of useful routines for C programming
 *
 * gevent-epoll.c: The epoll() backend for GEventContext
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

#include "gevent.h"

#include <sys/epoll.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#include "ghash.h"
#include "gmessages.h"
#include "gslice.h"
#include "gslist.h"
#include "gstrfuncs.h"

typedef struct _GEpollContext GEpollContext;

struct _GEpollContext {
  GEventContext base_context;
  int epoll_fd;
  GHashTable *poll_records;
};

static void g_epoll_context_add_poll    (GEventContext *context, GPollFD *fd);
static void g_epoll_context_remove_poll (GEventContext *context, GPollFD *fd);
static void g_epoll_context_finalize    (GEventContext *context);

static GEventContextFuncs g_epoll_context_funcs =
    {
        g_epoll_context_add_poll,
        g_epoll_context_remove_poll,
        g_epoll_context_finalize,
    };

GEventContext *
g_epoll_event_context_new ()
{
  GEpollContext *context;

  context = (GEpollContext *)
      g_event_context_new (&g_epoll_context_funcs, sizeof(GEpollContext));

  context->epoll_fd = epoll_create (1);

  if (G_UNLIKELY (context->epoll_fd < 0))
    {
      g_critical ("epoll_create failed: %s", g_strerror(errno));
      g_slice_free (GEpollContext, context);
      return NULL;
    }

  context->poll_records = g_hash_table_new (g_direct_hash, g_direct_equal);

  return (GEventContext *) context;
}

static void
free_poll_record (gpointer key, gpointer value, gpointer user_data)
{
  g_slist_free ((GSList *) value);
}

static void
g_epoll_context_finalize (GEventContext *base_context)
{
  GEpollContext *context = (GEpollContext *) base_context;

  close (context->epoll_fd);

  g_hash_table_foreach (context->poll_records, free_poll_record, NULL);
  g_hash_table_destroy (context->poll_records);
}

static uint32_t
get_epoll_event_mask (const GSList *fds)
{
  uint32_t events = 0;
  const GSList *elem;
  for (elem = fds; elem != NULL; elem = elem->next)
    {
      gushort g_events = ((GPollFD *) elem->data)->events;

#if G_IO_IN == EPOLLIN && G_IO_OUT == EPOLLOUT && G_IO_PRI == EPOLLPRI
      events |= g_events & (G_IO_IN | G_IO_OUT | G_IO_PRI);
#else
      if ((g_events & G_IO_IN) != 0)
        events |= EPOLLIN;
      if ((g_events & G_IO_OUT) != 0)
        events |= EPOLLOUT;
      if ((g_events & G_IO_PRI) != 0)
        events |= EPOLLPRI;
#endif
    }
  return events;
}

static void
g_epoll_context_add_poll (GEventContext *base_context, GPollFD *fd)
{
  GEpollContext *context = (GEpollContext *) base_context;
  int epoll_op;
  struct epoll_event ev;
  GSList *poll_list;
  gpointer poll_key;
  int retval;

  g_return_if_fail (fd->fd >= 0);

  poll_key = GINT_TO_POINTER(fd->fd);
  poll_list = (GSList *) g_hash_table_lookup (context->poll_records, poll_key);
  epoll_op = (poll_list == NULL)? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  poll_list = g_slist_prepend (poll_list, fd);
  g_hash_table_insert (context->poll_records, poll_key, poll_list);

  ev.events = get_epoll_event_mask (poll_list);
  ev.data.ptr = poll_list;
  retval = epoll_ctl (context->epoll_fd, epoll_op, fd->fd, &ev);

#ifdef G_ENABLE_DEBUG
  if (G_UNLIKELY (retval != 0))
    {
      g_critical ("epoll_ctl failed: %s", g_strerror(errno));
      return;
    }
#endif
}

static void
g_epoll_context_remove_poll (GEventContext *base_context, GPollFD *fd)
{
  GEpollContext *context = (GEpollContext *) base_context;
  int epoll_op;
  struct epoll_event ev;
  GSList *poll_list;
  GSList *modified_poll_list;
  gpointer poll_key;
  int retval;

  poll_key = GINT_TO_POINTER(fd->fd);
  poll_list = (GSList *) g_hash_table_lookup (context->poll_records, poll_key);
  modified_poll_list = g_slist_remove (poll_list, fd);
  if (modified_poll_list == NULL)
    {
      epoll_op = EPOLL_CTL_DEL;
      g_hash_table_remove (context->poll_records, poll_key);
    }
  else
    {
      epoll_op = EPOLL_CTL_MOD;
      ev.events = get_epoll_event_mask (modified_poll_list);
      ev.data.ptr = modified_poll_list;
      g_hash_table_insert (context->poll_records, poll_key, modified_poll_list);
    }

  retval = epoll_ctl (context->epoll_fd, epoll_op, fd->fd, &ev);

#ifdef G_ENABLE_DEBUG
  if (G_UNLIKELY (retval != 0))
    {
      g_critical ("epoll_ctl failed: %s", g_strerror(errno));
      return;
    }
#endif
}

#endif /* HAVE_SYS_EPOLL_H */
