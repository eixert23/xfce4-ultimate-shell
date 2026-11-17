#ifndef SYSTRAY_H
#define SYSTRAY_H

#include "shell-plugin.h"

GtkWidget *systray_widget_new(UltimateShell *shell);
void systray_apply_settings(UltimateShell *shell);
void systray_ibus_apply_mode(UltimateShell *shell, gboolean force);
void systray_populate_settings_box(UltimateShell *shell, GtkWidget *target_vbox);

void systray_open_audio_properties_dialog(UltimateShell *shell);
GtkWidget *systray_build_audio_settings(UltimateShell *shell);

#endif
