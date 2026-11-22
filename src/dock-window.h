#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

#include "shell-plugin.h"

void dock_window_init(UltimateShell *shell);
GtkWidget *dock_window_launchpad_btn(void);   /* the dock's start menu (Show Apps) button */
void dock_window_reposition(UltimateShell *shell);
void dock_window_apply_settings(UltimateShell *shell);
void dock_window_shutdown(void);
gint dock_window_shelf_extent(UltimateShell *shell);

#endif
