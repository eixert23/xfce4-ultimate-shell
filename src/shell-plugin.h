#ifndef SHELL_PLUGIN_H
#define SHELL_PLUGIN_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <xfconf/xfconf.h>
#include "window-tracker.h"

typedef enum {
    LAYOUT_MACOS = 0,
    LAYOUT_WINDOWS = 1
} ShellLayoutMode;

typedef enum {
    THEME_FOLLOW_XFCE = 0,
    THEME_CUSTOM_COLOR = 1,
    THEME_SPECIFIC_INSTALLED = 2
} ShellThemeMode;   /* legacy, only read once to migrate to color_mode */

typedef enum {
    COLOR_FOLLOW_GTK = 0,   /* palette read from the GTK theme */
    COLOR_LIGHT      = 1,
    COLOR_DARK       = 2,
    COLOR_CUSTOM     = 3
} ShellColorMode;

/* The one palette everything in the shell is drawn with */
typedef struct {
    GdkRGBA bg, fg, accent, accent_fg, hover, border;
    gboolean dark;
} ShellPalette;

typedef enum {
    START_STYLE_DEFAULT    = 0,
    START_STYLE_WIN7       = 1,
    START_STYLE_WIN11      = 2,
    START_STYLE_WHISKER    = 3,
    START_STYLE_FULLSCREEN = 4,
    START_STYLE_WIN10      = 5,   /* Windows 10 Tiles Menu */
    START_STYLE_GNOME      = 6    /* GNOME App Grid */
} StartMenuStyle;
#define START_STYLE_COUNT 7

typedef enum {
    START_BTN_MODE_ICON_LABEL = 0,
    START_BTN_MODE_ICON_ONLY  = 1,
    START_BTN_MODE_LABEL_ONLY = 2
} StartButtonMode;

typedef enum {
    START_CAT_DISPLAY_ICON_LABEL = 0,
    START_CAT_DISPLAY_ICON_ONLY  = 1,
    START_CAT_DISPLAY_LABEL_ONLY = 2
} StartCatDisplayMode;

typedef enum {
    START_SEARCH_POS_DEFAULT = 0,
    START_SEARCH_POS_TOP     = 1,
    START_SEARCH_POS_BOTTOM  = 2
} StartSearchPosMode;

typedef enum {
    CAL_STYLE_WIN7  = 0,
    CAL_STYLE_WIN10 = 1,
    CAL_STYLE_WIN11 = 2,
    CAL_STYLE_GNOME = 3
} CalendarStyle;

typedef enum {
    CLOCK_FMT_TIME_TOP  = 0,
    CLOCK_FMT_DATE_TOP  = 1,
    CLOCK_FMT_TIME_ONLY = 2
} ClockFormatMode;

typedef enum {
    DOCK_POS_BOTTOM = 0,
    DOCK_POS_LEFT   = 1,
    DOCK_POS_RIGHT  = 2
} DockPositionMode;

typedef enum {
    DOCK_HIDE_NEVER       = 0,
    DOCK_HIDE_INTELLIGENT = 1,
    DOCK_HIDE_ALWAYS      = 2
} DockAutohideMode;

typedef enum {
    IND_STYLE_WIN11  = 0,
    IND_STYLE_WIN10  = 1,
    IND_STYLE_PLASMA = 2,
    IND_STYLE_NONE   = 3
} TaskbarIndicatorStyle;

typedef enum {
    DOCK_IND_DOT  = 0,   /* macOS dot */
    DOCK_IND_PILL = 1,   /* wide pill (default) */
    DOCK_IND_DOTS = 2,   /* one dot per window, up to 3 */
    DOCK_IND_LINE = 3,
    DOCK_IND_NONE = 4
} DockIndicatorStyle;

typedef enum {
    TASKBAR_ALIGN_CENTER = 0,
    TASKBAR_ALIGN_LEFT   = 1
} TaskbarAlignMode;

typedef struct {
    XfcePanelPlugin       *plugin;
    XfconfChannel         *channel;
    ShellLayoutMode        mode;
    gboolean               gnome_style;
    gboolean               super_key_start;   /* the Super (Windows) key opens the start menu */   /* LAYOUT_MACOS shaped as GNOME: [WS App] [Clock] [Tray QS] + dock */
    ShellThemeMode         theme_mode;
    gint                   color_mode;         /* ShellColorMode */
    StartMenuStyle         start_style;
    CalendarStyle          cal_style;
    ClockFormatMode        clock_format;
    TaskbarIndicatorStyle  ind_style;

    /* Per-Mode Panel Row Size Persistence */
    gint                   win_panel_size;
    gint                   mac_panel_size;
    gboolean               applying_layout_size;
    gint64                 size_guard_until;   /* ignore panel size reports until then (startup, layout switch) */

    /* Clock & Date Text Customization Options */
    gint                   clock_time_pt;
    gint                   clock_date_pt;
    gint                   clock_time_weight;
    gint                   clock_date_weight;
    gboolean               clock_24h;
    gboolean               clock_show_seconds;
    gint                   clock_date_fmt;
    gint                   clock_line_spacing;
    gchar                 *clock_date_custom;   /* strftime text for "Custom" */
    gboolean               clock_mac_show_day;
    gboolean               clock_mac_show_date;
    gboolean               cal_week_numbers;

    /* Taskbar & Hover Preview Options */
    TaskbarAlignMode       tb_align;
    gint                   tb_icon_size;
    gint                   tb_icon_spacing;
    gint                   tb_icon_padding;
    gboolean               tb_show_inactive_ind;
    gint                   dock_ind_style;      /* DockIndicatorStyle */
    gboolean               ind_urgent_pulse;    /* orange pulse on apps that need attention */
    gchar                 *ind_color;           /* active,   "" = follow the accent colour */
    gchar                 *ind_inactive_color;  /* inactive, "" = same as active */
    gchar                 *ind_urgent_color;    /* attention, "" = orange */
    gboolean               show_thumbnails;
    gboolean              tb_middle_click_close;
    gboolean               show_desktop_enabled;
    gboolean               show_notif_icon;

    /* Start Button & Per-Style Menu Customization Options */
    StartButtonMode        start_btn_mode;
    gchar                 *start_btn_label;
    gchar                 *start_btn_icon;
    gint                   start_menu_w;
    gint                   start_menu_h;
    gint                   start_style_w[START_STYLE_COUNT];
    gint                   start_style_h[START_STYLE_COUNT];
    gint                   start_style_cat_icon[START_STYLE_COUNT];
    gint                   start_style_cat_pt[START_STYLE_COUNT];
    gint                   start_style_cat_weight[START_STYLE_COUNT];
    gint                   start_style_cat_spacing[START_STYLE_COUNT];
    gint                   start_style_list_icon[START_STYLE_COUNT];
    gint                   start_style_grid_icon[START_STYLE_COUNT];
    gint                   start_style_label_pt[START_STYLE_COUNT];
    gint                   start_style_grid_cols[START_STYLE_COUNT];
    gint                   start_style_fs_rows;
    gint                   start_gnome_rows;    /* GNOME App Grid rows (Launchpad uses start_style_fs_rows) */
    gchar                 *win10_tile_sizes;
    gchar                 *win10_tile_pos;
    gchar                 *win10_groups;       /* tile group names, ";"-separated, in order */     /* "app.desktop=col,row;..." free tile placement */   /* "app.desktop=s;other.desktop=w" (s/m/w) */
    StartCatDisplayMode    start_style_cat_mode[START_STYLE_COUNT];
    StartSearchPosMode     start_style_search_pos[START_STYLE_COUNT];
    gboolean               start_style_show_desc[START_STYLE_COUNT];
    gboolean               start_style_hover_cat[START_STYLE_COUNT];

    /* Favorites & Windows 7 Extra Shortcuts */
    gchar                 *start_favorites;
    gboolean               win7_show_pictures;
    gboolean               win7_show_music;
    gboolean               win7_show_videos;
    gboolean               win7_show_trash;
    gboolean               win7_show_terminal;
    gint           win7_places_weight;

    /* Shared Start Menu Visual & Footer Options */
    gint                   start_opacity;
    gboolean               start_hover_effect;
    gboolean               start_generic_names;
    gboolean               start_show_recommended;   /* Recent & Recommended section (off by default) */
    gboolean               start_recommend_files;
    gboolean               start_whisker_grid;       /* Whisker: show apps as icons */
    gboolean               win10_show_tiles;         /* Windows 10 menu: pinned tiles pane */
    gboolean               start_show_user_header;
    gboolean               start_show_lock;
    gboolean               start_show_logout;
    gboolean               start_show_restart;
    gboolean               start_show_shutdown;
    guint                  rebuild_debounce_id;

    /* System Tray & PulseAudio Customization Options */
    gboolean               tray_show_all;
    gboolean       tray_menu_is_primary;
    gboolean       tray_quick_settings;   /* Sound / Network / Power (+ Bluetooth, mic) as one Quick Settings panel */
    gboolean       tray_qs_bluetooth;     /* Bluetooth icon joins the Quick Settings group */
    gboolean       tray_request_symbolic;
    gboolean               tray_hide_new_default;
    gboolean               tray_enable_network;
    gboolean               tray_enable_audio;
    gboolean               tray_enable_battery;
    gboolean               tray_enable_clipboard;
    gboolean              tray_enable_removable;
    gboolean              tray_enable_bluetooth;
    gint                   tray_icon_size;
    gint                   tray_icon_spacing;
    gboolean               audio_key_shortcuts;
    gboolean               audio_show_rec_ind;
    gint                   audio_vol_step;
    gint                   audio_max_vol;
    gint                   audio_notif_mode;
    gchar                 *audio_mixer_cmd;
    gboolean               audio_mpris_enabled;
    gboolean               audio_mpris_keys;
    gboolean               audio_mpris_all;
    gboolean               audio_mpris_focus;
    gchar                 *audio_known_players;
    gchar                 *audio_persistent_players;
    gchar                 *audio_ignored_players;

    /* macOS Top Bar Windowck Customization Options */
    gboolean               winck_enabled;
    gint                   titlebar_btn_side; /* 0 = title bars follow the top-bar side, 3 = don't touch */
    gint                   winck_pos; /* 0 = left, after the system menu (C, H, M); 1 = far right (H, M, C) */

    /* macOS Dock Customization Options */
    DockPositionMode       dock_pos;
    DockAutohideMode       dock_autohide;
    gint                   dock_icon_size;
    gboolean               dock_magnify;
    gboolean              dock_show_tooltips;
    gint                   dock_opacity;
    gboolean               dock_full_width;   /* bottom: full width; left/right: full height */
    gint                   dock_style;        /* 0 = macOS dock, 1 = Ubuntu dock (left, full height, no zoom) */
    gint                  dock_full_width_layout;
    gboolean               dock_show_trash;
    gint                   dock_border_w;
    gint                   dock_radius_val;
    gint                   dock_pad_h;
    gint                   dock_pad_v;
    gint                   dock_zoom_scale;
    gboolean               dock_icon_shadow;
    gint                   dock_Icon_shadow_pad;
    gint                   prefs_w;
    gint                   prefs_h;
    gchar                 *dock_border_color;
    gchar                 *dock_launchpad_icon;
    gchar                 *mac_apple_icon;
    gint                   start_btn_icon_size;
    gint                   mac_apple_icon_size;
    gint                   dock_border_opacity;
    gint                   dock_edge_dist;
    gint                  dock_hide_delay_sec;
    gint                   winck_btn_pad;
    gint                  winck_btn_size;

    gboolean               dark_mode;
    gchar                 *custom_bg;
    gchar                 *custom_fg;
    gchar                 *custom_accent;
    gchar                 *ui_font;            /* "" = system font */
    gint                   ui_radius;          /* -1 = theme default */
    gint                   ui_border_w;        /* -1 = theme default */
    gchar                 *ui_border_color;    /* "" = theme default */
    gchar                 *ui_hover_color;     /* "" = theme default */
    gboolean               ui_popup_shadow;
    gboolean               ui_panel_frameless; /* make xfce4-panel's own background + edge line invisible */
    gboolean               ui_panel_shadow;    /* shell-drawn shadow under the panel */
    gboolean               wm_fill_maximized;  /* maximized terminals fill the screen (no character-size steps) */
    gchar                 *specific_theme_name;

    GtkCssProvider        *dynamic_css;
    UltimateWindowTracker *tracker;

    /* Root Panel Containers */
    GtkWidget *root_box;
    GtkWidget *left_box;
    GtkWidget *center_box;
    GtkWidget *right_box;

    /* Module Widgets */
    GtkWidget *start_btn;
    GtkWidget *mac_top_Controls_box;
    GtkWidget *windowck_widget;
    GtkWidget *global_menu_widget;
    GtkWidget *taskbar_box;
    GtkWidget *systray_widget;
    GtkWidget *clock_widget;
    GtkWidget *notif_widget;
    GtkWidget *show_desktop_btn;

    /* Standalone macOS Dock Window */
    GtkWidget *dock_window;
    GtkWidget *dock_inner_box;
    GtkWidget *dock_trash_btn;
    /* workspace indicator */
    GtkWidget *ws_widget;
    GtkWidget *app_title_widget;   /* GNOME mode: focused app icon + name */
    GtkWidget *center_group;     /* centered group inside center_box (Windows layouts) */
    gboolean   ws_enabled;
    gint       ws_style;         /* 0 GNOME pills, 1 dots, 2 numbers */
    gint       ws_size;          /* dot size, px */
    gboolean   ws_scroll;
    gboolean   ws_wrap;
    gboolean   ws_hide_single;
    gint       win_layout;
    gboolean   tray_ibus_modern;
    gint       anim_mode;     /* 0 off, 1 fade, 2 slide */
    gint       anim_style;    /* 1 fade, 2 slide - kept while animations are off */
    gint       anim_speed;    /* percent: 50 = twice as fast, 200 = half speed */   /* run IBus with the StatusNotifier tray icon */       /* Windows panel layout, see WIN_LAYOUT_* */
} UltimateShell;

enum {
    WIN_LAYOUT_CLASSIC   = 0,  /* [Start Taskbar]          ...        [WS Tray Clock] */
    WIN_LAYOUT_WS_LEFT   = 1,  /* [WS]       [Start Taskbar]          [Tray Clock]    */
    WIN_LAYOUT_START_WS  = 2,  /* [Start WS] [Taskbar]                [Tray Clock]    */
    WIN_LAYOUT_WIN11     = 3   /* [ ]        [Start Taskbar]          [WS Tray Clock] */
};

void shell_apply_layout(UltimateShell *shell, ShellLayoutMode new_mode);
void shell_apply_theme(UltimateShell *shell);
void shell_set_dark_mode(UltimateShell *shell, gboolean dark);   /* Quick Settings: Light <-> Dark colours */
void shell_apply_appearance_css(UltimateShell *shell);
void shell_palette_get(UltimateShell *shell, ShellPalette *out);
GtkWidget *shell_create_popup_window(GtkWidget *toggle_btn, gint width, gint height, gboolean resizable);
void shell_toggle_popup_window(GtkToggleButton *btn, GtkWidget *popup_win);
void shell_reposition_popup_window(GtkToggleButton *btn, GtkWidget *popup_win);
gboolean shell_winck_on_left(UltimateShell *shell);
gint     shell_anim_ms(gint base_ms);   /* 0 when animations are off */
void     shell_popup_hide(GtkWidget *win);          /* close with the exit animation */
void     shell_popup_cancel_close(GtkWidget *win);
gboolean shell_place_popup_at_button(GtkWidget *btn, gint pw, gint ph, gboolean centered, gint *x, gint *y);
void shell_popup_menu_at_button(GtkMenu *menu, GtkWidget *btn, const GdkEvent *ev);

GtkWidget *shell_create_badged_icon(GtkWidget *base_icon, GtkWidget **out_badge_label);
void shell_set_badge_count(GtkWidget *badge_label, guint count);

void windowck_apply_settings(UltimateShell *shell);

void shell_launch_app_info(GAppInfo *app);
/* Colour for running-app indicators on both the taskbar and the dock */
void shell_indicator_rgba(UltimateShell *shell, GdkRGBA *out);
void shell_indicator_colors(UltimateShell *shell, GdkRGBA *active, GdkRGBA *inactive, GdkRGBA *urgent);

#endif
