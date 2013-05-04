/* GLIB - Library of useful routines for C programming
 *
 * gevent-epoll.h: The epoll() backend for GEventContext
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

#ifndef __G_EVENT_EPOLL_H__
#define __G_EVENT_EPOLL_H__

#if !defined(GLIB_COMPILATION)
#error "This is a private header"
#endif

#include "gevent.h"

G_BEGIN_DECLS

#ifdef HAVE_SYS_EPOLL_H
GEventContext *_g_epoll_event_context_new (void);
#endif

G_END_DECLS

#endif /* __G_EVENT_EPOLL_H__ */
