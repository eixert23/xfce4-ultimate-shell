#ifndef DOCK_WINDOW_H
#define DOCK_WINDOW_H

#include "shell-plugin.h"

void dock_window_init(UltimateShell *shell);
void dock_window_reposition(UltimateShell *shell);
void dock_window_apply_settings(UltimateShell *shell);
void dock_window_shutdown(void);
gint dock_window_shelf_extent(UltimateShell *shell);

#endif
