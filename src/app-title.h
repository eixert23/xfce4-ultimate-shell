#ifndef APP_TITLE_H
#define APP_TITLE_H

#include <gtk/gtk.h>
#include "shell-plugin.h"

/* GNOME mode: focused app icon + name with a small app menu */
GtkWidget *app_title_new(UltimateShell *shell);
void app_title_refresh(void);

#endif
