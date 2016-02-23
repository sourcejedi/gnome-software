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

#ifndef __GS_PAYMENT_METHOD_LIST_H
#define __GS_PAYMENT_METHOD_LIST_H

#include <glib-object.h>

#include "gs-payment-method.h"

G_BEGIN_DECLS

#define GS_TYPE_PAYMENT_METHOD_LIST (gs_payment_method_list_get_type ())

G_DECLARE_FINAL_TYPE (GsPaymentMethodList, gs_payment_method_list, GS, PAYMENT_METHOD_LIST, GObject)

GsPaymentMethodList	*gs_payment_method_list_new		(void);
void			 gs_payment_method_list_add		(GsPaymentMethodList	*list,
								 GsPaymentMethod	*app);
GsPaymentMethod		*gs_payment_method_list_index		(GsPaymentMethodList	*list,
								 guint			 idx);
guint			 gs_payment_method_list_length		(GsPaymentMethodList	*list);

G_END_DECLS

#endif /* __GS_PAYMENT_METHOD_LIST_H */

/* vim: set noexpandtab: */
