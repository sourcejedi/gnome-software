#include "ubuntu-login-dialog.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define UBUNTU_LOGIN_HOST "https://login.ubuntu.com"

struct _UbuntuLoginDialog
{
  GtkDialog parent_instance;

  GtkBuilder *builder;
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

  gchar *host;
  gchar *consumer_key;
  gchar *consumer_secret;
  gchar *token_key;
  gchar *token_secret;
};

enum
{
  PROP_0,
  PROP_SESSION,
  PROP_HOST,
  PROP_REMEMBER,
  PROP_CONSUMER_KEY,
  PROP_CONSUMER_SECRET,
  PROP_TOKEN_KEY,
  PROP_TOKEN_SECRET
};

G_DEFINE_TYPE (UbuntuLoginDialog, ubuntu_login_dialog, GTK_TYPE_DIALOG)

static void
ubuntu_login_dialog_dispose (GObject *object)
{
  UbuntuLoginDialog *self = UBUNTU_LOGIN_DIALOG (object);

  g_clear_object (&self->builder);

  G_OBJECT_CLASS (ubuntu_login_dialog_parent_class)->dispose (object);
}

static void
ubuntu_login_dialog_finalize (GObject *object)
{
  UbuntuLoginDialog *self = UBUNTU_LOGIN_DIALOG (object);

  g_clear_pointer (&self->token_secret, g_free);
  g_clear_pointer (&self->token_key, g_free);
  g_clear_pointer (&self->consumer_secret, g_free);
  g_clear_pointer (&self->consumer_key, g_free);

  G_OBJECT_CLASS (ubuntu_login_dialog_parent_class)->finalize (object);
}

static void
ubuntu_login_dialog_get_property (GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  UbuntuLoginDialog *self = UBUNTU_LOGIN_DIALOG (object);

  switch (property_id)
    {
    case PROP_SESSION:
      g_value_set_object (value, self->session);
      break;

    case PROP_HOST:
      g_value_set_string (value, self->host);
      break;

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
ubuntu_login_dialog_set_property (GObject      *object,
                                  guint         property_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  UbuntuLoginDialog *self = UBUNTU_LOGIN_DIALOG (object);

  switch (property_id)
    {
    case PROP_SESSION:
      self->session = g_value_dup_object (value);
      break;

    case PROP_HOST:
      self->host = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
ubuntu_login_dialog_class_init (UbuntuLoginDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags param_flags;

  object_class->dispose = ubuntu_login_dialog_dispose;
  object_class->finalize = ubuntu_login_dialog_finalize;
  object_class->get_property = ubuntu_login_dialog_get_property;
  object_class->set_property = ubuntu_login_dialog_set_property;

  param_flags = G_PARAM_READABLE | G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB;

  g_object_class_install_property (object_class,
                                   PROP_SESSION,
                                   g_param_spec_object ("session",
                                                        "session",
                                                        "session",
                                                        SOUP_TYPE_SESSION,
                                                        param_flags |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT));

  g_object_class_install_property (object_class,
                                   PROP_HOST,
                                   g_param_spec_string ("host",
                                                        "host",
                                                        "host",
                                                        UBUNTU_LOGIN_HOST,
                                                        param_flags |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT));
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
update_widgets (UbuntuLoginDialog *self)
{
  if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-0"))
    {
      gtk_widget_set_sensitive (self->next_button,
                                !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)) ||
                                (is_email_address (gtk_entry_get_text (GTK_ENTRY (self->email_entry))) &&
                                 gtk_entry_get_text_length (GTK_ENTRY (self->password_entry)) > 0));
      gtk_widget_set_sensitive (self->password_entry,
                                gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)));
      gtk_widget_set_sensitive (self->remember_check,
                                gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)));
    }
  else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-1"))
    gtk_widget_set_sensitive (self->next_button, gtk_entry_get_text_length (GTK_ENTRY (self->passcode_entry)) > 0);
  else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-2"))
    {
      gtk_widget_set_visible (self->cancel_button, FALSE);
      gtk_widget_set_sensitive (self->cancel_button, FALSE);
      gtk_button_set_label (GTK_BUTTON (self->next_button), _("_Continue"));
    }
}

static void
cancel_button_clicked_cb (UbuntuLoginDialog *self,
                          GtkButton         *button)
{
  gtk_dialog_response (GTK_DIALOG (self), GTK_RESPONSE_CANCEL);
}

typedef void (*ResponseCallback) (UbuntuLoginDialog *self,
                                  guint              status,
                                  GVariant          *response,
                                  gpointer           user_data);

typedef struct
{
  UbuntuLoginDialog *dialog;
  ResponseCallback callback;
  gpointer user_data;
} RequestInfo;

static void
response_received_cb (SoupSession *session,
                      SoupMessage *message,
                      gpointer     user_data)
{
  RequestInfo *info = user_data;
  GVariant *response;
  guint status;
  GBytes *bytes;
  gchar *body;
  gsize length;

  g_object_get (message,
                SOUP_MESSAGE_STATUS_CODE, &status,
                SOUP_MESSAGE_RESPONSE_BODY_DATA, &bytes,
                NULL);

  body = g_bytes_unref_to_data (bytes, &length);
  response = json_gvariant_deserialize_data (body, length, NULL, NULL);
  g_free (body);

  if (response)
    g_variant_ref_sink (response);

  if (info->callback)
    info->callback (info->dialog, status, response, info->user_data);

  g_variant_unref (response);
  g_free (info);
}

static void
send_request (UbuntuLoginDialog *self,
              const gchar       *method,
              const gchar       *uri,
              GVariant          *request,
              ResponseCallback   callback,
              gpointer           user_data)
{
  RequestInfo *info;
  SoupMessage *message;
  gchar *body;
  gsize length;
  gchar *url;

  body = json_gvariant_serialize_data (g_variant_ref_sink (request), &length);
  g_variant_unref (request);

  url = g_strdup_printf ("%s%s", self->host, uri);
  message = soup_message_new (method, url);
  g_free (url);

  info = g_new0 (RequestInfo, 1);
  info->dialog = self;
  info->callback = callback;
  info->user_data = user_data;

  soup_message_set_request (message, "application/json", SOUP_MEMORY_TAKE, body, length);
  soup_session_queue_message (self->session, message, response_received_cb, info);
}

static void
receive_login_response_cb (UbuntuLoginDialog *self,
                           guint              status,
                           GVariant          *response,
                           gpointer           user_data)
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

  switch (status)
    {
    case 200:
    case 201:
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

      if (g_str_equal (code, "TWOFACTOR_REQUIRED"))
        {
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

      if (g_str_equal (code, "INVALID_CREDENTIALS"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Incorrect email or password"));
      else if (g_str_equal (code, "ACCOUNT_SUSPENDED"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Account suspended"));
      else if (g_str_equal (code, "ACCOUNT_DEACTIVATED"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Account deactivated"));
      else if (g_str_equal (code, "EMAIL_INVALIDATED"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Email invalidated"));
      else if (g_str_equal (code, "TWOFACTOR_FAILURE"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Two-factor authentication failed"));
      else if (g_str_equal (code, "PASSWORD_POLICY_ERROR"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Password reset required"));
      else if (g_str_equal (code, "TOO_MANY_REQUESTS"))
        gtk_label_set_text (GTK_LABEL (self->status_label), _("Too many requests"));
      else
        gtk_label_set_text (GTK_LABEL (self->status_label), _("An error occurred"));

      break;
    }
}

static void
send_login_request (UbuntuLoginDialog *self)
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

  if (gtk_entry_get_text_length (GTK_ENTRY (self->passcode_entry)) > 0)
    {
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
    }
  else
    {
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
next_button_clicked_cb (UbuntuLoginDialog *self,
                        GtkButton         *button)
{
  if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-0"))
    {
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)))
        send_login_request (self);
      else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->register_radio)))
        g_app_info_launch_default_for_uri ("https://login.ubuntu.com/+new_account", NULL, NULL);
      else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->reset_radio)))
        g_app_info_launch_default_for_uri ("https://login.ubuntu.com/+forgot_password", NULL, NULL);
    }
  else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-1"))
    send_login_request (self);
  else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-2"))
    gtk_dialog_response (GTK_DIALOG (self), GTK_RESPONSE_OK);
}

static void
radio_button_toggled_cb (UbuntuLoginDialog *self,
                         GtkToggleButton   *toggle)
{
  update_widgets (self);
}

static void
entry_edited_cb (UbuntuLoginDialog *self,
                 GParamSpec        *pspec,
                 GObject           *object)
{
  update_widgets (self);
}

static void
remember_check_toggled_cb (UbuntuLoginDialog *self,
                           GtkToggleButton   *toggle)
{
  g_object_notify (G_OBJECT (self), "remember");
}

static void
ubuntu_login_dialog_init (UbuntuLoginDialog *self)
{
  self->builder = gtk_builder_new_from_resource ("/org/gnome/Software/plugins/ubuntu-login-dialog.glade");
  self->content_box = GTK_WIDGET (gtk_builder_get_object (self->builder, "content-box"));
  self->cancel_button = GTK_WIDGET (gtk_builder_get_object (self->builder, "cancel-button"));
  self->next_button = GTK_WIDGET (gtk_builder_get_object (self->builder, "next-button"));
  self->status_stack = GTK_WIDGET (gtk_builder_get_object (self->builder, "status-stack"));
  self->status_image = GTK_WIDGET (gtk_builder_get_object (self->builder, "status-image"));
  self->status_label = GTK_WIDGET (gtk_builder_get_object (self->builder, "status-label"));
  self->page_stack = GTK_WIDGET (gtk_builder_get_object (self->builder, "page-stack"));
  self->login_radio = GTK_WIDGET (gtk_builder_get_object (self->builder, "login-radio"));
  self->register_radio = GTK_WIDGET (gtk_builder_get_object (self->builder, "register-radio"));
  self->reset_radio = GTK_WIDGET (gtk_builder_get_object (self->builder, "reset-radio"));
  self->email_entry = GTK_WIDGET (gtk_builder_get_object (self->builder, "email-entry"));
  self->password_entry = GTK_WIDGET (gtk_builder_get_object (self->builder, "password-entry"));
  self->remember_check = GTK_WIDGET (gtk_builder_get_object (self->builder, "remember-check"));
  self->passcode_entry = GTK_WIDGET (gtk_builder_get_object (self->builder, "passcode-entry"));

  g_signal_connect_swapped (self->cancel_button, "clicked", G_CALLBACK (cancel_button_clicked_cb), self);
  g_signal_connect_swapped (self->next_button, "clicked", G_CALLBACK (next_button_clicked_cb), self);
  g_signal_connect_swapped (self->login_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
  g_signal_connect_swapped (self->register_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
  g_signal_connect_swapped (self->reset_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
  g_signal_connect_swapped (self->remember_check, "toggled", G_CALLBACK (remember_check_toggled_cb), self);
  g_signal_connect_swapped (self->email_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);
  g_signal_connect_swapped (self->password_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);
  g_signal_connect_swapped (self->passcode_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);

  gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (self))), self->content_box);

  update_widgets (self);
}

GtkWidget *
ubuntu_login_dialog_new (void)
{
  return g_object_new (UBUNTU_TYPE_LOGIN_DIALOG, NULL);
}
