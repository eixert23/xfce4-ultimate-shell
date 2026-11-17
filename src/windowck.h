#ifndef WINDOWCK_H
#define WINDOWCK_H

#include <gtk/gtk.h>
#include "window-tracker.h"

GtkWidget *windowck_widget_new(UltimateWindowTracker *tracker);
void windowck_set_left_order(GtkWidget *wck_root, gboolean left_order_chm);
void windowck_shutdown(void);

#endif
