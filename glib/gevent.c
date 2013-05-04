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
#include "gevent-epoll.h"

#include "gmessages.h"
#include "gslice.h"
#include "gthread.h"

/* Uncomment the next line (and the corresponding line in gpoll.c) to
 * enable debugging printouts if the environment variable
 * G_MAIN_POLL_DEBUG is set to some value.
 */
/* #define G_MAIN_POLL_DEBUG */

struct _GEventContextPrivate
{
  gsize struct_size;
  gint ref_count;
  GMutex mutex;
};

#define LOCK_CONTEXT(context) g_mutex_lock (&context->priv->mutex)
#define UNLOCK_CONTEXT(context) g_mutex_unlock (&context->priv->mutex)

static void g_event_context_add_poll_unlocked    (GEventContext *context,
                                                  GPollFD       *fd);
static void g_event_context_remove_poll_unlocked (GEventContext *context,
                                                  GPollFD       *fd);

#ifdef G_MAIN_POLL_DEBUG
extern gboolean _g_main_poll_debug;
#endif

static GEventContext *default_event_context;

/**
 * g_event_context_default:
 *
 * Returns the global default event context. This is the context
 * used for main loop functions when a main loop is not explicitly
 * specified, and corresponds to the "main" main loop. See also
 * g_event_context_get_thread_default().
 *
 * Return value: (transfer none): the global default event context.
 **/
GEventContext *
g_event_context_default (void)
{
  static gsize initialised;

  if (g_once_init_enter (&initialised))
    {
      default_event_context = g_event_context_new ();
#ifdef G_MAIN_POLL_DEBUG
      if (_g_main_poll_debug)
        g_print ("default event context=%p\n", default_event_context);
#endif
      g_once_init_leave (&initialised, TRUE);
    }

  return default_event_context;
}

/**
 * g_event_context_new:
 *
 * Creates a #GEventContext instance with the built-in implementation.
 *
 * Return value: the new #GEventContext
 **/
GEventContext *
g_event_context_new (void)
{
#ifdef HAVE_SYS_EPOLL_H
  return _g_epoll_event_context_new ();
#else
  /* TODO: implement the fallback poll() implementation */
#error no event context implementation available
#endif
}

GEventContext *
g_event_context_new_custom (GEventContextFuncs *funcs,
                            gsize struct_size)
{
  GEventContext *context;
  GEventContextPrivate *priv;

#ifdef G_MAIN_POLL_DEBUG
  static gsize debug_initialised;

  if (g_once_init_enter (&debug_initialised))
    {
      if (getenv ("G_MAIN_POLL_DEBUG") != NULL)
        _g_main_poll_debug = TRUE;

      g_once_init_leave (&debug_initialised, TRUE);
    }
#endif

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
  if (!context)
    context = g_event_context_default ();

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
  if (!context)
    context = g_event_context_default ();

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
