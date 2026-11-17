#ifndef NOTIFICATIONS_H
#define NOTIFICATIONS_H

#include <gtk/gtk.h>

GtkWidget *notifications_widget_new(void);

/* history snapshot for the calendar flyout (GNOME style) */
typedef struct {
    gchar     *app_name, *app_icon, *summary, *body;
    GDateTime *time;
} ShellNotifEntry;

GList   *notifications_get_history(void);      /* of ShellNotifEntry*, newest first */
void     shell_notif_entry_free(gpointer e);
void     notifications_clear_all(void);
gboolean notifications_get_dnd(void);
void     notifications_set_dnd(gboolean on);

#endif