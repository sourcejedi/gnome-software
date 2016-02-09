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

#include "gs-review-histogram.h"

typedef struct
{
	GtkWidget	*progress5;
	GtkWidget	*progress4;
	GtkWidget	*progress3;
	GtkWidget	*progress2;
	GtkWidget	*progress1;
	GtkWidget	*label_count5;
	GtkWidget	*label_count4;
	GtkWidget	*label_count3;
	GtkWidget	*label_count2;
	GtkWidget	*label_count1;
	GtkWidget	*label_total;
} GsReviewHistogramPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsReviewHistogram, gs_review_histogram, GTK_TYPE_BIN)

static void
set_label (GtkWidget *label, guint value)
{
	g_autofree gchar *text = NULL;

	text = g_strdup_printf ("%u", value);
	gtk_label_set_text (GTK_LABEL (label), text);
}

/**
 * gs_review_histogram_set_ratings:
 **/
void
gs_review_histogram_set_ratings (GsReviewHistogram *histogram,
				 guint count1,
				 guint count2,
				 guint count3,
				 guint count4,
				 guint count5)
{
	GsReviewHistogramPrivate *priv;
	gdouble max;

	g_return_if_fail (GS_IS_REVIEW_HISTOGRAM (histogram));
	priv = gs_review_histogram_get_instance_private (histogram);

	/* Scale to maximum value */
	max = count1;
	max = count2 > max ? count2 : max;
	max = count3 > max ? count3 : max;
	max = count4 > max ? count4 : max;
	max = count5 > max ? count5 : max;

	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress5), count5 / max);
	set_label (priv->label_count5, count5);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress4), count4 / max);
	set_label (priv->label_count4, count4);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress3), count3 / max);
	set_label (priv->label_count3, count3);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress2), count2 / max);
	set_label (priv->label_count2, count2);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress1), count1 / max);
	set_label (priv->label_count1, count1);
	set_label (priv->label_total, count1 + count2 + count3 + count4 + count5);
}

static void
gs_review_histogram_init (GsReviewHistogram *histogram)
{
	gtk_widget_init_template (GTK_WIDGET (histogram));
}

static void
gs_review_histogram_class_init (GsReviewHistogramClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-review-histogram.ui");

	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, progress5);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, progress4);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, progress3);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, progress2);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, progress1);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_count5);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_count4);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_count3);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_count2);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_count1);
	gtk_widget_class_bind_template_child_private (widget_class, GsReviewHistogram, label_total);
}

/**
 * gs_review_histogram_new:
 **/
GtkWidget *
gs_review_histogram_new (void)
{
	GsReviewHistogram *histogram;
	histogram = g_object_new (GS_TYPE_REVIEW_HISTOGRAM, NULL);
	return GTK_WIDGET (histogram);
}

/* vim: set noexpandtab: */
