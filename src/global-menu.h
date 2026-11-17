#ifndef GLOBAL_MENU_H
#define GLOBAL_MENU_H

#include <gtk/gtk.h>
#include "window-tracker.h"

GtkWidget *global_menu_widget_new(UltimateWindowTracker *tracker);
void global_menu_set_enabled(gboolean enabled);

#endif
