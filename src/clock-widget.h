#ifndef CLOCK_WIDGET_H
#define CLOCK_WIDGET_H

#include "shell-plugin.h"

GtkWidget *clock_widget_new(UltimateShell *shell);
void clock_widget_rebuild_style(UltimateShell *shell);

#endif
