/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2016 Richard Hughes <richard@hughsie.com>
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

/**
 * SECTION:gs-payment_method-list
 * @title: GsPaymentMethodList
 * @include: gnome-software.h
 * @stability: Unstable
 * @short_description: An payment method list
 *
 * These functions provide a refcounted list of #GsPaymentMethod objects.
 */

#include "config.h"

#include <glib.h>

#include "gs-payment-method-list.h"

struct _GsPaymentMethodList
{
	GObject			 parent_instance;
	GPtrArray		*array;
};

G_DEFINE_TYPE (GsPaymentMethodList, gs_payment_method_list, G_TYPE_OBJECT)

/**
 * gs_payment_method_list_add:
 * @list: A #GsPaymentMethodList
 * @app: A #GsPaymentMethod
 *
 * Add a payment method.
 **/
void
gs_payment_method_list_add (GsPaymentMethodList *list, GsPaymentMethod *payment_method)
{
	g_return_if_fail (GS_IS_PAYMENT_METHOD_LIST (list));
	g_return_if_fail (GS_IS_PAYMENT_METHOD (payment_method));

	g_ptr_array_add (list->array, g_object_ref (payment_method));
}

/**
 * gs_payment_method_list_index:
 * @list: A #GsPaymentMethodList
 * @idx: An index into the list
 *
 * Gets a payment method at a specific position in the list.
 *
 * Returns: (transfer none): a #GsPaymentMethod, or %NULL if invalid
 **/
GsPaymentMethod *
gs_payment_method_list_index (GsPaymentMethodList *list, guint idx)
{
	return GS_PAYMENT_METHOD (g_ptr_array_index (list->array, idx));
}

/**
 * gs_payment_method_list_length:
 * @list: A #GsPaymentMethodList
 *
 * Gets the length of the payment method list.
 *
 * Returns: Integer
 **/
guint
gs_payment_method_list_length (GsPaymentMethodList *list)
{
	g_return_val_if_fail (GS_IS_PAYMENT_METHOD_LIST (list), 0);
	return list->array->len;
}

static void
gs_payment_method_list_finalize (GObject *object)
{
	GsPaymentMethodList *list = GS_PAYMENT_METHOD_LIST (object);
	g_ptr_array_unref (list->array);
	G_OBJECT_CLASS (gs_payment_method_list_parent_class)->finalize (object);
}

static void
gs_payment_method_list_class_init (GsPaymentMethodListClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gs_payment_method_list_finalize;
}

static void
gs_payment_method_list_init (GsPaymentMethodList *list)
{
	list->array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
}

/**
 * gs_payment_method_list_new:
 *
 * Creates a new list.
 *
 * Returns: A newly allocated #GsPaymentMethodList
 **/
GsPaymentMethodList *
gs_payment_method_list_new (void)
{
	GsPaymentMethodList *list;
	list = g_object_new (GS_TYPE_PAYMENT_METHOD_LIST, NULL);
	return GS_PAYMENT_METHOD_LIST (list);
}

/* vim: set noexpandtab: */
