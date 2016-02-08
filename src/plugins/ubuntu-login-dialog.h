#ifndef __UBUNTU_LOGIN_DIALOG_H__
#define __UBUNTU_LOGIN_DIALOG_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define UBUNTU_TYPE_LOGIN_DIALOG ubuntu_login_dialog_get_type ()

G_DECLARE_FINAL_TYPE (UbuntuLoginDialog, ubuntu_login_dialog, UBUNTU, LOGIN_DIALOG, GtkDialog)

GtkWidget * ubuntu_login_dialog_new (void);

G_END_DECLS

#endif /* __UBUNTU_LOGIN_DIALOG_H__ */
