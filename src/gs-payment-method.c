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

#include "config.h"

#include <glib/gi18n.h>

#include "gs-payment-method.h"

struct _GsPaymentMethod
{
	GObject			 parent_instance;

	gchar			*description;
	GHashTable		*metadata;
};

G_DEFINE_TYPE (GsPaymentMethod, gs_payment_method, G_TYPE_OBJECT)

/**
 * gs_payment_method_get_description:
 * @method: a #GsPaymentMethod
 *
 * Gets the payment method description.
 *
 * Returns: a one line description for this payment method, e.g. "**** **** **** 1111 (exp 23/2020)"
 */
const gchar *
gs_payment_method_get_description (GsPaymentMethod *method)
{
	g_return_val_if_fail (GS_IS_PAYMENT_METHOD (method), NULL);
	return method->description;
}

/**
 * gs_payment_method_set_description:
 * @method: a #GsPaymentMethod
 * @description: Human readable description for this payment method, e.g. "**** **** **** 1111 (exp 23/2020)"
 *
 * Sets the one line description that may be displayed for this payment method.
 */
void
gs_payment_method_set_description (GsPaymentMethod *method, const gchar *description)
{
	g_return_if_fail (GS_IS_PAYMENT_METHOD (method));
	g_free (method->description);
	method->description = g_strdup (description);
}

/**
 * gs_payment_method_get_metadata_item:
 * @method: a #GsPaymentMethod
 * @key: a string
 *
 * Gets some metadata from a payment method object.
 * It is left for the the plugin to use this method as required, but a
 * typical use would be to store an ID for this payment, or payment information.
 *
 * Returns: A string value, or %NULL for not found
 */
const gchar *
gs_payment_method_get_metadata_item (GsPaymentMethod *method, const gchar *key)
{
	g_return_val_if_fail (GS_IS_PAYMENT_METHOD (method), NULL);
	g_return_val_if_fail (key != NULL, NULL);
	return g_hash_table_lookup (method->metadata, key);
}

/**
 * gs_payment_method_add_metadata:
 * @method: a #GsPaymentMethod
 * @key: a string
 * @value: a string
 *
 * Adds metadata to the review object.
 * It is left for the the plugin to use this method as required, but a
 * typical use would be to store an ID for this payment, or payment information.
 */
void
gs_payment_method_add_metadata (GsPaymentMethod *method, const gchar *key, const gchar *value)
{
	g_return_if_fail (GS_IS_PAYMENT_METHOD (method));
	g_hash_table_insert (method->metadata, g_strdup (key), g_strdup (value));
}

static void
gs_payment_method_finalize (GObject *object)
{
	GsPaymentMethod *method = GS_PAYMENT_METHOD (object);

	g_free (method->description);
	g_hash_table_unref (method->metadata);

	G_OBJECT_CLASS (gs_payment_method_parent_class)->finalize (object);
}

static void
gs_payment_method_class_init (GsPaymentMethodClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gs_payment_method_finalize;
}

static void
gs_payment_method_init (GsPaymentMethod *method)
{
	method->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
						  g_free, g_free);
}

/**
 * gs_payment_method_new:
 *
 * Creates a new payment method object.
 *
 * Return value: a new #GsPaymentMethod object.
 **/
GsPaymentMethod *
gs_payment_method_new (void)
{
	GsPaymentMethod *method;
	method = g_object_new (GS_TYPE_PAYMENT_METHOD, NULL);
	return GS_PAYMENT_METHOD (method);
}

/* vim: set noexpandtab: */
