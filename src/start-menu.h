#ifndef START_MENU_H
#define START_MENU_H

#include "shell-plugin.h"

GtkWidget *start_menu_button_new(UltimateShell *shell);
void start_menu_rebuild_style(UltimateShell *shell);
void start_menu_update_button_appearance(UltimateShell *shell);
void start_menu_open_from_dock(UltimateShell *shell, GtkWidget *anchor_btn);
void start_menu_toggle_from_key(UltimateShell *shell);   /* Super key: open / close the start menu */

/* shared app actions (also used by the taskbar / dock right-click menu) */
void     app_run_as_admin(GAppInfo *app);
gint     app_append_jump_actions(GtkWidget *menu, GAppInfo *app);
void     app_edit_launcher(GAppInfo *app);
void     app_open_uninstall(GAppInfo *app);
gboolean app_store_available(void);

void     start_menu_popup_quick_menu(GtkWidget *anchor, GdkEventButton *ev);

void     start_menu_clear_usage_history(void);

#endif
