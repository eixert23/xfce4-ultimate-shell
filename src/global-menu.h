#ifndef GLOBAL_MENU_H
#define GLOBAL_MENU_H

#include <gtk/gtk.h>
#include "window-tracker.h"

GtkWidget *global_menu_widget_new(UltimateWindowTracker *tracker);
void global_menu_set_enabled(gboolean enabled);
/* for the GNOME-mode app name: the focused app's main window (NULL on the desktop), its name, and Quit */
WnckWindow *global_menu_active_app_window(void);
const gchar *global_menu_app_name(WnckWindow *win);
void global_menu_quit_app(WnckWindow *win);

#endif
