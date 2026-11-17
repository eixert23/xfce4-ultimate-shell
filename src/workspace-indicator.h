#ifndef WORKSPACE_INDICATOR_H
#define WORKSPACE_INDICATOR_H

#include "shell-plugin.h"

GtkWidget *workspace_indicator_new(UltimateShell *shell);
void       workspace_indicator_apply_settings(UltimateShell *shell);
gint       workspace_indicator_get_count(void);
void       workspace_indicator_set_count(gint n);

#endif
