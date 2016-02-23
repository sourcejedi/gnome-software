 /* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd.
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

#ifndef __GS_PAYMENT_METHOD_H
#define __GS_PAYMENT_METHOD_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GS_TYPE_PAYMENT_METHOD (gs_payment_method_get_type ())

G_DECLARE_FINAL_TYPE (GsPaymentMethod, gs_payment_method, GS, PAYMENT_METHOD, GObject)

GsPaymentMethod	*gs_payment_method_new			(void);

const gchar	*gs_payment_method_get_description	(GsPaymentMethod	*method);
void		 gs_payment_method_set_description	(GsPaymentMethod	*method,
							 const gchar		*description);

const gchar	*gs_payment_method_get_metadata_item	(GsPaymentMethod	*method,
							 const gchar		*key);
void		 gs_payment_method_add_metadata		(GsPaymentMethod	*method,
							 const gchar		*key,
							 const gchar		*value);

G_END_DECLS

#endif /* __GS_PAYMENT_METHOD_H */

/* vim: set noexpandtab: */
