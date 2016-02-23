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
#include <gtk/gtk.h>

#include "gs-purchase-dialog.h"

struct _GsPurchaseDialog
{
	GtkDialog	 parent_instance;
};

G_DEFINE_TYPE (GsPurchaseDialog, gs_purchase_dialog, GTK_TYPE_DIALOG)

static void
gs_purchase_dialog_init (GsPurchaseDialog *dialog)
{
	gtk_widget_init_template (GTK_WIDGET (dialog));
}

static void
gs_purchase_dialog_class_init (GsPurchaseDialogClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-purchase-dialog.ui");
}

GtkWidget *
gs_purchase_dialog_new (void)
{
	return GTK_WIDGET (g_object_new (GS_TYPE_PURCHASE_DIALOG,
					 "use-header-bar", TRUE,
					 NULL));
}

/* vim: set noexpandtab: */
