/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * gevent.c: Event loop basic implementation
 * Copyright (C) 2013  Mikhail Zabaluev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GLib Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/.
 */

/*
 * MT safe
 */

#include "config.h"

#include "geventprivate.h"

#include "gmessages.h"
#include "gslice.h"
#include "gthread.h"

struct _GEventContextPrivate
{
  gsize struct_size;
  gint  ref_count;
  GMutex mutex;
};

#define LOCK_CONTEXT(context) g_mutex_lock (&context->priv->mutex)
#define UNLOCK_CONTEXT(context) g_mutex_unlock (&context->priv->mutex)

static void g_event_context_add_poll_unlocked    (GEventContext *context,
                                                  GPollFD       *fd);
static void g_event_context_remove_poll_unlocked (GEventContext *context,
                                                  GPollFD       *fd);

GEventContext *
g_event_context_new (GEventContextFuncs *funcs,
                     gsize struct_size)
{
  GEventContext *context;
  GEventContextPrivate *priv;

  context = g_slice_alloc0 (struct_size);
  priv = g_slice_new0 (GEventContextPrivate);
  context->funcs = funcs;
  context->priv = priv;
  priv->struct_size = struct_size;
  priv->ref_count = 1;
  g_mutex_init (&priv->mutex);

  return context;
}

GEventContext *
g_event_context_ref (GEventContext *context)
{
  g_return_val_if_fail (context != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&context->priv->ref_count) > 0, NULL);

  g_atomic_int_inc (&context->priv->ref_count);

  return context;
}

void
g_event_context_unref (GEventContext *context)
{
  GEventContextPrivate *priv;

  g_return_if_fail (context != NULL);
  g_return_if_fail (g_atomic_int_get (&context->priv->ref_count) > 0);

  priv = context->priv;

  if (!g_atomic_int_dec_and_test (&priv->ref_count))
    return;

  context->funcs->finalize (context);

  g_mutex_clear (&priv->mutex);

  g_slice_free1 (priv->struct_size, context);
  g_slice_free (GEventContextPrivate, priv);
}

void
_g_event_context_add_poll (GEventContext *context,
                           GPollFD       *fd)
{
  g_return_if_fail (g_atomic_int_get (&context->priv->ref_count) > 0);
  g_return_if_fail (fd != NULL);

  LOCK_CONTEXT (context);
  g_event_context_add_poll_unlocked (context, fd);
  UNLOCK_CONTEXT (context);
}

void
_g_event_context_remove_poll (GEventContext *context,
                              GPollFD       *fd)
{
  g_return_if_fail (g_atomic_int_get (&context->priv->ref_count) > 0);
  g_return_if_fail (fd != NULL);

  LOCK_CONTEXT (context);
  g_event_context_remove_poll_unlocked (context, fd);
  UNLOCK_CONTEXT (context);
}

static void
g_event_context_add_poll_unlocked (GEventContext *context,
                                   GPollFD       *fd)
{
  context->funcs->add_poll (context, fd);
}

static void
g_event_context_remove_poll_unlocked (GEventContext *context,
                                      GPollFD       *fd)
{
  context->funcs->remove_poll (context, fd);
}
