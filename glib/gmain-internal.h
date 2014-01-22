/* gmain-internal.h - GLib-internal mainloop API
 * Copyright (C) 2011 Red Hat, Inc.
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __G_MAIN_INTERNAL_H__
#define __G_MAIN_INTERNAL_H__

#if !defined (GLIB_COMPILATION)
#error "This is a private header"
#endif

#include "gmain.h"

/* Uncomment the next line (and the corresponding line in gpoll.c) to
 * enable debugging printouts if the environment variable
 * G_MAIN_POLL_DEBUG is set to some value.
 */
/* #define G_MAIN_POLL_DEBUG */

#ifdef _WIN32
/* Always enable debugging printout on Windows, as it is more often
 * needed there...
 */
#define G_MAIN_POLL_DEBUG
#endif

G_BEGIN_DECLS

GSource *_g_main_create_unix_signal_watch (int signum);

#ifdef G_MAIN_POLL_DEBUG
extern gboolean _g_main_poll_debug;
#endif

typedef struct _GBaselinePollerData GBaselinePollerData;
extern GPollerFuncs _g_baseline_poller_funcs;
GBaselinePollerData *_g_baseline_poller_new (GPollFunc func);

#ifdef HAVE_SYS_EPOLL_H
typedef struct _GEpollerData GEpollerData;
extern GPollerFuncs _g_epoller_funcs;
GEpollerData *_g_epoller_new (void);
#endif

/* Functions supporting poll-centric API on GMainContext */
void      _g_baseline_poller_set_poll_func (GBaselinePollerData *backend,
                                            GPollFunc func);
GPollFunc _g_baseline_poller_get_poll_func (const GBaselinePollerData *backend);

G_END_DECLS

#endif /* __G_MAIN_H__ */
