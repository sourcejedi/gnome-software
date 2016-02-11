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

#include "gs-ubuntu-login-dialog.h"
#include "gs-utils.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define UBUNTU_LOGIN_HOST "https://login.ubuntu.com"

struct _GsUbuntuLoginDialog
{
	GtkDialog parent_instance;

	GtkWidget *content_box;
	GtkWidget *cancel_button;
	GtkWidget *next_button;
	GtkWidget *status_stack;
	GtkWidget *status_image;
	GtkWidget *status_label;
	GtkWidget *page_stack;
	GtkWidget *login_radio;
	GtkWidget *register_radio;
	GtkWidget *reset_radio;
	GtkWidget *email_entry;
	GtkWidget *password_entry;
	GtkWidget *remember_check;
	GtkWidget *passcode_entry;

	SoupSession *session;

	gchar *consumer_key;
	gchar *consumer_secret;
	gchar *token_key;
	gchar *token_secret;
};

enum
{
	PROP_0,
	PROP_SESSION,
	PROP_REMEMBER,
	PROP_CONSUMER_KEY,
	PROP_CONSUMER_SECRET,
	PROP_TOKEN_KEY,
	PROP_TOKEN_SECRET
};

G_DEFINE_TYPE (GsUbuntuLoginDialog, gs_ubuntu_login_dialog, GTK_TYPE_DIALOG)

static gboolean
is_email_address (const gchar *text)
{
	text = g_utf8_strchr (text, -1, '@');

	if (!text)
		return FALSE;

	text = g_utf8_strchr (text + 1, -1, '.');

	if (!text)
		return FALSE;

	return text[1];
}

static void
update_widgets (GsUbuntuLoginDialog *self)
{
	if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-0")) {
		gtk_widget_set_sensitive (self->next_button,
					  !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)) ||
					  (is_email_address (gtk_entry_get_text (GTK_ENTRY (self->email_entry))) &&
					   gtk_entry_get_text_length (GTK_ENTRY (self->password_entry)) > 0));
		gtk_widget_set_sensitive (self->password_entry,
					  gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)));
		gtk_widget_set_sensitive (self->remember_check,
					  gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)));
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-1")) {
		gtk_widget_set_sensitive (self->next_button, gtk_entry_get_text_length (GTK_ENTRY (self->passcode_entry)) > 0);
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-2")) {
		gtk_widget_set_visible (self->cancel_button, FALSE);
		gtk_widget_set_sensitive (self->cancel_button, FALSE);
		gtk_button_set_label (GTK_BUTTON (self->next_button), _("_Continue"));
	}
}

typedef void (*ResponseCallback) (GsUbuntuLoginDialog *self,
				  guint	      status,
				  GVariant	  *response,
				  gpointer	   user_data);

typedef struct
{
	GsUbuntuLoginDialog *dialog;
	ResponseCallback callback;
	gpointer user_data;
} RequestInfo;

static void
response_received_cb (SoupSession *session,
		      SoupMessage *message,
		      gpointer     user_data)
{
	RequestInfo *info = user_data;
	g_autoptr(GVariant) response = NULL;
	guint status;
	GBytes *bytes;
	g_autofree gchar *body = NULL;
	gsize length;

	g_object_get (message,
		      SOUP_MESSAGE_STATUS_CODE, &status,
		      SOUP_MESSAGE_RESPONSE_BODY_DATA, &bytes,
		      NULL);

	body = g_bytes_unref_to_data (bytes, &length);
	response = json_gvariant_deserialize_data (body, length, NULL, NULL);

	if (response)
		g_variant_ref_sink (response);

	if (info->callback)
		info->callback (info->dialog, status, response, info->user_data);

	g_free (info);
}

static void
send_request (GsUbuntuLoginDialog *self,
	      const gchar         *method,
	      const gchar         *uri,
	      GVariant	          *request,
	      ResponseCallback     callback,
	      gpointer	           user_data)
{
	RequestInfo *info;
	SoupMessage *message;
	gchar *body;
	gsize length;
	g_autofree gchar *url = NULL;

	if (self->session == NULL)
		self->session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT,
							       gs_user_agent (),
							       NULL);

	body = json_gvariant_serialize_data (g_variant_ref_sink (request), &length);
	g_variant_unref (request);

	url = g_strdup_printf ("%s%s", UBUNTU_LOGIN_HOST, uri);
	message = soup_message_new (method, url);

	info = g_new0 (RequestInfo, 1);
	info->dialog = self;
	info->callback = callback;
	info->user_data = user_data;

	soup_message_set_request (message, "application/json", SOUP_MEMORY_TAKE, body, length);
	soup_session_queue_message (self->session, message, response_received_cb, info);
}

static void
receive_login_response_cb (GsUbuntuLoginDialog *self,
			   guint	       status,
			   GVariant	      *response,
			   gpointer	       user_data)
{
	PangoAttrList *attributes;
	const gchar *code;

	gtk_label_set_text (GTK_LABEL (self->status_label), NULL);
	gtk_stack_set_visible_child_name (GTK_STACK (self->status_stack), "status-image");
	gtk_widget_set_visible (self->status_stack, FALSE);

	gtk_widget_set_sensitive (self->cancel_button, TRUE);
	gtk_widget_set_sensitive (self->next_button, TRUE);
	gtk_widget_set_sensitive (self->login_radio, TRUE);
	gtk_widget_set_sensitive (self->register_radio, TRUE);
	gtk_widget_set_sensitive (self->reset_radio, TRUE);
	gtk_widget_set_sensitive (self->email_entry, TRUE);
	gtk_widget_set_sensitive (self->password_entry, TRUE);
	gtk_widget_set_sensitive (self->remember_check, TRUE);
	gtk_widget_set_sensitive (self->passcode_entry, TRUE);

	switch (status) {
	case SOUP_STATUS_OK:
	case SOUP_STATUS_CREATED:
		g_clear_pointer (&self->token_secret, g_free);
		g_clear_pointer (&self->token_key, g_free);
		g_clear_pointer (&self->consumer_secret, g_free);
		g_clear_pointer (&self->consumer_key, g_free);

		g_variant_lookup (response, "consumer_key", "s", &self->consumer_key);
		g_variant_lookup (response, "consumer_secret", "s", &self->consumer_secret);
		g_variant_lookup (response, "token_key", "s", &self->token_key);
		g_variant_lookup (response, "token_secret", "s", &self->token_secret);

		g_object_notify (G_OBJECT (self), "consumer-key");
		g_object_notify (G_OBJECT (self), "consumer-secret");
		g_object_notify (G_OBJECT (self), "token-key");
		g_object_notify (G_OBJECT (self), "token-secret");

		gtk_stack_set_visible_child_name (GTK_STACK (self->page_stack), "page-2");
		update_widgets (self);
		break;

	default:
		g_variant_lookup (response, "code", "&s", &code);

		if (!code)
			code = "";

		if (g_str_equal (code, "TWOFACTOR_REQUIRED")) {
			gtk_stack_set_visible_child_name (GTK_STACK (self->page_stack), "page-1");
			update_widgets (self);
			break;
		}

		update_widgets (self);

		gtk_widget_set_visible (self->status_stack, TRUE);
		gtk_stack_set_visible_child_name (GTK_STACK (self->status_stack), "status-image");
		gtk_image_set_from_icon_name (GTK_IMAGE (self->status_image), "gtk-dialog-error", GTK_ICON_SIZE_BUTTON);

		attributes = pango_attr_list_new ();
		pango_attr_list_insert (attributes, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
		pango_attr_list_insert (attributes, pango_attr_foreground_new (65535, 0, 0));
		gtk_label_set_attributes (GTK_LABEL (self->status_label), attributes);
		pango_attr_list_unref (attributes);

		if (g_str_equal (code, "INVALID_CREDENTIALS")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Incorrect email or password"));
		} else if (g_str_equal (code, "ACCOUNT_SUSPENDED")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Account suspended"));
		} else if (g_str_equal (code, "ACCOUNT_DEACTIVATED")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Account deactivated"));
		} else if (g_str_equal (code, "EMAIL_INVALIDATED")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Email invalidated"));
		} else if (g_str_equal (code, "TWOFACTOR_FAILURE")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Two-factor authentication failed"));
		} else if (g_str_equal (code, "PASSWORD_POLICY_ERROR")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Password reset required"));
		} else if (g_str_equal (code, "TOO_MANY_REQUESTS")) {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("Too many requests"));
		} else {
			gtk_label_set_text (GTK_LABEL (self->status_label), _("An error occurred"));
		}

		break;
	}
}

static void
send_login_request (GsUbuntuLoginDialog *self)
{
	PangoAttrList *attributes;

	gtk_widget_set_sensitive (self->cancel_button, FALSE);
	gtk_widget_set_sensitive (self->next_button, FALSE);
	gtk_widget_set_sensitive (self->login_radio, FALSE);
	gtk_widget_set_sensitive (self->register_radio, FALSE);
	gtk_widget_set_sensitive (self->reset_radio, FALSE);
	gtk_widget_set_sensitive (self->email_entry, FALSE);
	gtk_widget_set_sensitive (self->password_entry, FALSE);
	gtk_widget_set_sensitive (self->remember_check, FALSE);
	gtk_widget_set_sensitive (self->passcode_entry, FALSE);

	gtk_widget_set_visible (self->status_stack, TRUE);
	gtk_stack_set_visible_child_name (GTK_STACK (self->status_stack), "status-spinner");
	gtk_label_set_text (GTK_LABEL (self->status_label), _("Signing in…"));

	attributes = pango_attr_list_new ();
	pango_attr_list_insert (attributes, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
	pango_attr_list_insert (attributes, pango_attr_foreground_new (0, 0, 0));
	gtk_label_set_attributes (GTK_LABEL (self->status_label), attributes);
	pango_attr_list_unref (attributes);

	if (gtk_entry_get_text_length (GTK_ENTRY (self->passcode_entry)) > 0) {
		send_request (self,
			      SOUP_METHOD_POST,
			      "/api/v2/tokens/oauth",
			      g_variant_new_parsed ("{"
						    "  'token_name' : 'GNOME Software',"
						    "  'email' : %s,"
						    "  'password' : %s,"
						    "  'otp' : %s"
						    "}",
						    gtk_entry_get_text (GTK_ENTRY (self->email_entry)),
						    gtk_entry_get_text (GTK_ENTRY (self->password_entry)),
						    gtk_entry_get_text (GTK_ENTRY (self->passcode_entry))),
			      receive_login_response_cb,
			      NULL);
	} else {
		send_request (self,
			      SOUP_METHOD_POST,
			      "/api/v2/tokens/oauth",
			      g_variant_new_parsed ("{"
						    "  'token_name' : 'GNOME Software',"
						    "  'email' : %s,"
						    "  'password' : %s"
						    "}",
						    gtk_entry_get_text (GTK_ENTRY (self->email_entry)),
						    gtk_entry_get_text (GTK_ENTRY (self->password_entry))),
			      receive_login_response_cb,
			      NULL);
	}
}

static void
next_button_clicked_cb (GsUbuntuLoginDialog *self,
			GtkButton	    *button)
{
	if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-0")) {
		if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio))) {
			send_login_request (self);
		} else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->register_radio))) {
			g_app_info_launch_default_for_uri ("https://login.ubuntu.com/+new_account", NULL, NULL);
		} else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->reset_radio))) {
			g_app_info_launch_default_for_uri ("https://login.ubuntu.com/+forgot_password", NULL, NULL);
		}
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-1")) {
		send_login_request (self);
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-2")) {
		gtk_dialog_response (GTK_DIALOG (self), GTK_RESPONSE_OK);
	}
}

static void
radio_button_toggled_cb (GsUbuntuLoginDialog *self,
			 GtkToggleButton   *toggle)
{
	update_widgets (self);
}

static void
entry_edited_cb (GsUbuntuLoginDialog *self,
		 GParamSpec	     *pspec,
		 GObject	     *object)
{
	update_widgets (self);
}

static void
remember_check_toggled_cb (GsUbuntuLoginDialog *self,
			   GtkToggleButton     *toggle)
{
	g_object_notify (G_OBJECT (self), "remember");
}

static void
gs_ubuntu_login_dialog_init (GsUbuntuLoginDialog *self)
{
	gtk_widget_init_template (GTK_WIDGET (self));

	g_signal_connect_swapped (self->next_button, "clicked", G_CALLBACK (next_button_clicked_cb), self);
	g_signal_connect_swapped (self->login_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
	g_signal_connect_swapped (self->register_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
	g_signal_connect_swapped (self->reset_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
	g_signal_connect_swapped (self->remember_check, "toggled", G_CALLBACK (remember_check_toggled_cb), self);
	g_signal_connect_swapped (self->email_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);
	g_signal_connect_swapped (self->password_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);
	g_signal_connect_swapped (self->passcode_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);

	update_widgets (self);
}

static void
gs_ubuntu_login_dialog_dispose (GObject *object)
{
	GsUbuntuLoginDialog *self = GS_UBUNTU_LOGIN_DIALOG (object);

	g_clear_object (&self->session);

	G_OBJECT_CLASS (gs_ubuntu_login_dialog_parent_class)->dispose (object);
}

static void
gs_ubuntu_login_dialog_finalize (GObject *object)
{
	GsUbuntuLoginDialog *self = GS_UBUNTU_LOGIN_DIALOG (object);

	g_clear_pointer (&self->token_secret, g_free);
	g_clear_pointer (&self->token_key, g_free);
	g_clear_pointer (&self->consumer_secret, g_free);
	g_clear_pointer (&self->consumer_key, g_free);

	G_OBJECT_CLASS (gs_ubuntu_login_dialog_parent_class)->finalize (object);
}

static void
gs_ubuntu_login_dialog_get_property (GObject    *object,
				     guint       property_id,
				     GValue     *value,
				     GParamSpec *pspec)
{
	GsUbuntuLoginDialog *self = GS_UBUNTU_LOGIN_DIALOG (object);

	switch (property_id) {
	case PROP_REMEMBER:
		g_value_set_boolean (value, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->remember_check)));
		break;

	case PROP_CONSUMER_KEY:
		g_value_set_string (value, self->consumer_key);
		break;

	case PROP_CONSUMER_SECRET:
		g_value_set_string (value, self->consumer_secret);
		break;

	case PROP_TOKEN_KEY:
		g_value_set_string (value, self->token_key);
		break;

	case PROP_TOKEN_SECRET:
		g_value_set_string (value, self->token_secret);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
gs_ubuntu_login_dialog_class_init (GsUbuntuLoginDialogClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	GParamFlags param_flags;

	object_class->dispose = gs_ubuntu_login_dialog_dispose;
	object_class->finalize = gs_ubuntu_login_dialog_finalize;
	object_class->get_property = gs_ubuntu_login_dialog_get_property;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/plugins/gs-ubuntu-login-dialog.ui");

	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, content_box);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, cancel_button);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, next_button);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, status_stack);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, status_image);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, status_label);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, page_stack);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, login_radio);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, register_radio);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, reset_radio);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, email_entry);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, password_entry);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, remember_check);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuLoginDialog, passcode_entry);

	param_flags = G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB;

	g_object_class_install_property (object_class,
					 PROP_REMEMBER,
					 g_param_spec_boolean ("remember",
							       "remember",
							       "remember",
							       FALSE,
							       param_flags));

	g_object_class_install_property (object_class,
					 PROP_CONSUMER_KEY,
					 g_param_spec_string ("consumer-key",
							      "consumer key",
							      "consumer key",
							      NULL,
							      param_flags));

	g_object_class_install_property (object_class,
					 PROP_CONSUMER_SECRET,
					 g_param_spec_string ("consumer-secret",
							      "consumer secret",
							      "consumer secret",
							      NULL,
							      param_flags));

	g_object_class_install_property (object_class,
					 PROP_TOKEN_KEY,
					 g_param_spec_string ("token-key",
							      "token key",
							      "token key",
							      NULL,
							      param_flags));

	g_object_class_install_property (object_class,
					 PROP_TOKEN_SECRET,
					 g_param_spec_string ("token-secret",
							      "token secret",
							      "token secret",
							      NULL,
							      param_flags));
}

GtkWidget *
gs_ubuntu_login_dialog_new (void)
{
	return GTK_WIDGET (g_object_new (GS_TYPE_UBUNTU_LOGIN_DIALOG,
					 "use-header-bar", TRUE,
					 NULL));
}

/* vim: set noexpandtab: */
