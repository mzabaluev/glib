/* gevent.h - the GLib event contexts
 * Copyright (C) 1998-2000 Red Hat, Inc.
 * Copyright (C) 2013 Mikhail Zabaluev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __G_EVENT_H__
#define __G_EVENT_H__

#if !defined (__GLIB_H_INSIDE__) && !defined (GLIB_COMPILATION)
#error "Only <glib.h> can be included directly."
#endif

#include <glib/gpoll.h>

G_BEGIN_DECLS

/**
 * GEventContext:
 *
 * The <structname>GEventContext</structname> structure is an opaque
 * data type representing an implementation of an event processing context,
 * with a set of sources to be handled in an event loop.
 */
typedef struct _GEventContext GEventContext;
typedef struct _GEventContextPrivate GEventContextPrivate;

/**
 * GEventContextFuncs:
 * @add_poll: Called to add a file descriptor to the event context.
 * @remove_poll: Called when a file descriptor is removed from the event context.
 * @finalize: Called when the event context is finalized.
 *
 * The <structname>GEventContextFuncs</structname> structure contains a table
 * of functions implementing an event context backend.
 */
typedef struct _GEventContextFuncs GEventContextFuncs;

struct _GEventContextFuncs
{
  void (*add_poll)    (GEventContext *context, GPollFD *fd);
  void (*remove_poll) (GEventContext *context, GPollFD *fd);
  void (*finalize)    (GEventContext *context);
};

struct _GEventContext
{
  /*< private >*/
  GEventContextFuncs   *funcs;
  GEventContextPrivate *priv;
};

GLIB_AVAILABLE_IN_2_38
GEventContext *g_event_context_default (void);
GLIB_AVAILABLE_IN_2_38
GEventContext *g_event_context_new (void);
GLIB_AVAILABLE_IN_2_38
GEventContext *g_event_context_new_custom (GEventContextFuncs *funcs,
                                           gsize struct_size);
GLIB_AVAILABLE_IN_2_38
GEventContext *g_event_context_ref   (GEventContext *context);
GLIB_AVAILABLE_IN_2_38
void           g_event_context_unref (GEventContext *context);

G_END_DECLS

#endif /* __G_EVENT_H__ */
