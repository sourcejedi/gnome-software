/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __GS_UBUNTU_SNAPD_H__
#define __GS_UBUNTU_SNAPD_H__

#include <gio/gio.h>

gboolean send_snapd_request (const gchar  *method,
			     const gchar  *path,
			     const gchar  *content,
			     gboolean      authenticate,
			     GVariant     *macaroon,
			     gboolean      retry_after_login,
			     guint        *status_code,
			     gchar       **reason_phrase,
			     gchar       **response_type,
			     gchar       **response,
			     gsize        *response_length,
			     GError      **error);

#endif /* __GS_UBUNTU_SNAPD_H__ */
