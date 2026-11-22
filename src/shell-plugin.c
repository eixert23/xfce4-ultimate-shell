#include <math.h>
#include <gmodule.h>
#include <gio/gdesktopappinfo.h>
#include "shell-plugin.h"
#include "start-menu.h"
#include "clock-widget.h"
#include "dock-window.h"
#include "taskbar.h"
#include "windowck.h"
#include "global-menu.h"
#include "systray.h"
#include "win-fill.h"
#include "workspace-indicator.h"
#include "notifications.h"
#include "app-title.h"
#include <string.h>
#include <glib/gstdio.h>

GtkWidget *shell_create_badged_icon(GtkWidget *base_icon, GtkWidget **out_badge_label) {
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_halign(base_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(base_icon, GTK_ALIGN_CENTER);
    /* Give the icon 3px top & right breathing room inside the overlay so the top-right badge sits on the corner instead of covering the icon */
    gtk_widget_set_margin_top(base_icon, 3);
    gtk_widget_set_margin_end(base_icon, 4);
    gtk_widget_set_margin_start(base_icon, 2);
    gtk_widget_set_margin_bottom(base_icon, 2);
    gtk_container_add(GTK_CONTAINER(overlay), base_icon);

    GtkWidget *badge = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), "notification-badge");
    gtk_widget_set_halign(badge, GTK_ALIGN_END);
    gtk_widget_set_valign(badge, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), badge, TRUE);
    gtk_widget_set_no_show_all(badge, TRUE);
    gtk_widget_hide(badge);

    if (out_badge_label) *out_badge_label = badge;
    return overlay;
}

void shell_set_badge_count(GtkWidget *badge_label, guint count) {
    if (!GTK_IS_LABEL(badge_label)) return;
    if (count == 0) {
        gtk_widget_hide(badge_label);
    } else {
        gchar *txt = g_strdup_printf("%u", count);
        gtk_label_set_text(GTK_LABEL(badge_label), txt);
        g_free(txt);
        gtk_widget_show(badge_label);
    }
}

static gchar *rgba_to_hex(const GdkRGBA *rgba) {
    return g_strdup_printf("#%02X%02X%02X",
        CLAMP((int)(rgba->red * 255.0 + 0.5), 0, 255),
        CLAMP((int)(rgba->green * 255.0 + 0.5), 0, 255),
        CLAMP((int)(rgba->blue * 255.0 + 0.5), 0, 255));
}


/* =========================================================================
 * The palette: one set of colours for the whole shell
 * ========================================================================= */
static void rgba_or(GdkRGBA *c, const gchar *s, const gchar *fallback) {
    if (!(s && s[0] && gdk_rgba_parse(c, s))) gdk_rgba_parse(c, fallback);
}
static gdouble rgba_luma(const GdkRGBA *c) { return 0.2126 * c->red + 0.7152 * c->green + 0.0722 * c->blue; }
static gchar *rgba_css(const GdkRGBA *c) {
    return g_strdup_printf("rgba(%d,%d,%d,%.3f)", (int)(c->red * 255 + .5), (int)(c->green * 255 + .5),
                           (int)(c->blue * 255 + .5), c->alpha);
}
static gchar *rgba_css_solid(const GdkRGBA *c) {
    return g_strdup_printf("rgb(%d,%d,%d)", (int)(c->red * 255 + .5), (int)(c->green * 255 + .5), (int)(c->blue * 255 + .5));
}

/* Literal palette for Light / Dark / Custom. FALSE for "Follow GTK theme". */
static gboolean palette_literal(UltimateShell *shell, GdkRGBA *bg, GdkRGBA *fg, GdkRGBA *ac) {
    switch (shell->color_mode) {
        case COLOR_LIGHT: gdk_rgba_parse(bg, "#F5F5F7"); gdk_rgba_parse(fg, "#1D1D1F"); break;
        case COLOR_DARK:  gdk_rgba_parse(bg, "#1E1E1E"); gdk_rgba_parse(fg, "#F2F2F7"); break;
        case COLOR_CUSTOM:
            rgba_or(bg, shell->custom_bg, "#F5F5F7");      /* never falls back to pure black */
            rgba_or(fg, shell->custom_fg, rgba_luma(bg) < 0.5 ? "#F2F2F7" : "#1D1D1F");
            break;
        default: return FALSE;
    }
    rgba_or(ac, shell->custom_accent, "#0A84FF");
    return TRUE;
}

/* @define-color lines for the current palette */
static gchar *shell_palette_define_css(UltimateShell *shell) {
    GString *d = g_string_new("");
    GdkRGBA bg, fg, ac;
    gboolean literal = palette_literal(shell, &bg, &fg, &ac);
    if (literal) {
        GdkRGBA afg; gdk_rgba_parse(&afg, rgba_luma(&ac) > 0.6 ? "#111111" : "#FFFFFF");
        gchar *B = rgba_css(&bg), *BS = rgba_css_solid(&bg), *F = rgba_css(&fg), *A = rgba_css(&ac), *AF = rgba_css(&afg);
        gboolean dark = rgba_luma(&bg) < 0.5;
        g_string_append_printf(d,
            "@define-color theme_bg_color %s; @define-color theme_fg_color %s;"
            "@define-color theme_base_color mix(%s, %s, %s); @define-color theme_text_color %s;"
            "@define-color theme_selected_bg_color %s; @define-color theme_selected_fg_color %s;"
            "@define-color theme_unfocused_bg_color %s; @define-color theme_unfocused_fg_color %s;"
            "@define-color theme_unfocused_base_color mix(%s, %s, %s); @define-color theme_unfocused_text_color %s;"
            "@define-color theme_unfocused_selected_bg_color %s; @define-color theme_unfocused_selected_fg_color %s;"
            "@define-color borders alpha(%s, 0.16); @define-color unfocused_borders alpha(%s, 0.16);"
            "@define-color insensitive_fg_color alpha(%s, 0.45); @define-color ult_bg_solid %s;",
            B, F, BS, dark ? "#FFFFFF" : "#FFFFFF", dark ? "0.06" : "0.70", F, A, AF,
            B, F, BS, "#FFFFFF", dark ? "0.06" : "0.70", F, A, AF, F, F, F, BS);
        g_free(B); g_free(BS); g_free(F); g_free(A); g_free(AF);
    } else {
        g_string_append(d, "@define-color ult_bg_solid @theme_bg_color;");
    }
    if (shell->ui_hover_color && shell->ui_hover_color[0])
        g_string_append_printf(d, "@define-color ult_hover alpha(%s, 0.28);", shell->ui_hover_color);
    else
        g_string_append(d, "@define-color ult_hover alpha(@theme_fg_color, 0.10);");
    if (shell->ui_border_color && shell->ui_border_color[0])
        g_string_append_printf(d, "@define-color ult_border %s;", shell->ui_border_color);
    else
        g_string_append(d, "@define-color ult_border alpha(@theme_fg_color, 0.14);");
    return g_string_free(d, FALSE);
}

/* Resolved colours for code that draws by hand (dock, indicators, Launchpad dots) */
void shell_palette_get(UltimateShell *shell, ShellPalette *out) {
    GdkRGBA bg, fg, ac;
    if (!shell || !palette_literal(shell, &bg, &fg, &ac)) {
        gdk_rgba_parse(&bg, "#F5F5F7"); gdk_rgba_parse(&fg, "#1D1D1F"); gdk_rgba_parse(&ac, "#3584E4");
        GtkWidget *ref = shell ? shell->root_box : NULL;
        GtkStyleContext *ctx = ref ? gtk_widget_get_style_context(ref) : NULL;
        GdkRGBA t;
        if (ctx && gtk_style_context_lookup_color(ctx, "theme_bg_color", &t) && t.alpha > 0.01) bg = t;
        if (ctx && gtk_style_context_lookup_color(ctx, "theme_fg_color", &t) && t.alpha > 0.01) fg = t;
        if (ctx && gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &t) && t.alpha > 0.01) ac = t;
    }
    out->bg = bg; out->fg = fg; out->accent = ac;
    gdk_rgba_parse(&out->accent_fg, rgba_luma(&ac) > 0.6 ? "#111111" : "#FFFFFF");
    out->dark = rgba_luma(&bg) < 0.5;
    out->hover = fg; out->hover.alpha = 0.10;
    if (shell && shell->ui_hover_color && shell->ui_hover_color[0]) { gdk_rgba_parse(&out->hover, shell->ui_hover_color); out->hover.alpha *= 0.28; }
    out->border = fg; out->border.alpha = 0.14;
    if (shell && shell->ui_border_color && shell->ui_border_color[0]) gdk_rgba_parse(&out->border, shell->ui_border_color);
}

void shell_apply_theme(UltimateShell *shell) {
    if (!shell->dynamic_css) {
        shell->dynamic_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(shell->dynamic_css),
            GTK_STYLE_PROVIDER_PRIORITY_USER
        );
    }

    gint dock_radius = shell->dock_full_width ? 0 : 16;
    gchar *css = NULL;

    /* One template for every colour mode. In Light / Dark / Custom the palette
       redefines GTK's own colour names, so this CSS - and every other @theme_*
       rule in the shell - follows it; in "Follow GTK theme" the theme defines them. */
    gchar *defs = shell_palette_define_css(shell);
    gdouble bar_a = CLAMP(shell->dock_opacity, 10, 100) / 100.0;
    gdouble pop_a = CLAMP(shell->start_opacity > 0 ? shell->start_opacity : 95, 20, 100) / 100.0;
    css = g_strdup_printf(
            "%s"
            ".xfce4-panel, #ultimate-root-box {"
            "  background-color: alpha(@theme_bg_color, %.2f); background-image: none; color: @theme_fg_color; padding: 0 6px;"
            "}"
            "#ultimate-start-btn, #ultimate-clock-btn, .ultimate-module-btn {"
            "  background: transparent; background-image: none; color: @theme_fg_color; border: none; border-radius: 4px; padding: 2px 8px; font-weight: 600;"
            "}"
            "#ultimate-start-btn label, #ultimate-clock-btn label, .ultimate-module-btn label,"
            "#ultimate-start-btn image, #ultimate-clock-btn image, .ultimate-module-btn image {"
            "  color: @theme_fg_color;"
            "}"
            "#ultimate-start-btn:hover, #ultimate-clock-btn:hover, .ultimate-module-btn:hover,"
            "#ultimate-start-btn:checked, #ultimate-clock-btn:checked, .ultimate-module-btn:checked {"
            "  background-color: @theme_selected_bg_color; background-image: none; color: @theme_selected_fg_color;"
            "}"
            "#ultimate-start-btn:hover label, #ultimate-clock-btn:hover label, .ultimate-module-btn:hover label,"
            "#ultimate-start-btn:checked label, #ultimate-clock-btn:checked label, .ultimate-module-btn:checked label {"
            "  color: @theme_selected_fg_color;"
            "}"
            ".ind-win7 { background-color: @theme_selected_bg_color; border-radius: 2px; }"
            ".ind-dot  { background-color: @theme_selected_bg_color; border-radius: 50%%; }"
            "#ultimate-dock-box {"
            "  background-color: %s; background-image: none; color: @theme_fg_color; border: 1px solid @ult_border; border-radius: %dpx; padding: 6px 14px;"
            "}"
            ".ultimate-popup-win, window.ultimate-popup-win.background {"
            "  background-color: alpha(@theme_bg_color, %.2f); background-image: none; color: @theme_fg_color; border: 1px solid @ult_border; border-radius: 8px;"
            "}"
            ".ultimate-popup-win label { color: @theme_fg_color; }"
            ".ultimate-popup-win list, .ultimate-popup-win row, .ultimate-popup-win flowbox, .ultimate-popup-win flowboxchild, .ultimate-popup-win calendar, .ultimate-popup-win viewport, .ultimate-popup-win scrolledwindow {"
            "  background-color: transparent; background-image: none; color: @theme_fg_color; border: none;"
            "}"
            ".ultimate-popup-win row:hover, .ultimate-popup-win row:selected,"
            ".ultimate-popup-win flowboxchild:hover, .ultimate-popup-win flowboxchild:selected {"
            "  background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; border-radius: 6px;"
            "}"
            ".app-grid-tile { background-color: transparent; border-radius: 10px; border: 1px solid transparent; transition: all 140ms ease; }"
            ".app-grid-tile.app-tile-hovered, .app-grid-tile:hover { background-color: alpha(@theme_selected_bg_color, 0.25); border-color: @theme_selected_bg_color; border-radius: 10px; }"
            "row.app-grid-tile, .ultimate-popup-win list row.app-grid-tile { background-color: transparent; border-radius: 10px; border: 1px solid transparent; margin: 2px 4px; padding: 2px 4px; transition: all 140ms ease; }"
            "row.app-grid-tile:hover, row.app-grid-tile.app-tile-hovered, .ultimate-popup-win list row.app-grid-tile:hover, .ultimate-popup-win list row.app-grid-tile.app-tile-hovered { background-color: alpha(@theme_selected_bg_color, 0.25); border-color: @theme_selected_bg_color; border-radius: 10px; }"
            "entry.win11-search-pill { border-radius: 20px; padding: 6px 14px; }"
            ".ultimate-popup-win flowboxchild { padding: 0px; margin: 0px; border: none; }"
            "row.app-grid-tile:focus, .ultimate-popup-win list row:focus, .ultimate-popup-win flowboxchild:focus { outline: none; box-shadow: none; -gtk-outline-radius: 10px; }"
            "#ultimate-start-btn, #ultimate-clock-btn, .ultimate-module-btn { border-radius: 5px; border: 1px solid transparent; transition: all 140ms ease; }"
            ".taskbar-item-btn { min-width: 0px; min-height: 0px; padding: 0px; margin: 0px; border-radius: 5px; border: 1px solid transparent; transition: all 140ms ease; }"
            "#ultimate-start-btn:hover, #ultimate-clock-btn:hover, .ultimate-module-btn:hover, .taskbar-item-btn:hover { background-color: @ult_hover; background-image: none; border-color: transparent; border-radius: 5px; }"
            "#ultimate-clock-btn { min-width: 0px; min-height: 0px; padding: 0px 5px; margin: 0px; border-radius: 5px; }"
            ".ultimate-module-btn { min-width: 0px; min-height: 0px; padding: 0px 4px; margin: 0px; border-radius: 5px; }"
            ".start-category-btn { min-height: 28px; padding: 5px 10px; margin: 0px; border-radius: 7px; border: 1px solid transparent; transition: all 130ms ease; } .start-category-btn:hover { background-color: alpha(@theme_selected_bg_color, 0.22); background-image: none; border-color: alpha(@theme_selected_bg_color, 0.45); border-radius: 7px; }"
            ".overflow-tray-btn { min-width: 34px; min-height: 34px; padding: 4px; margin: 0px; border-radius: 6px; border: 1px solid transparent; transition: all 120ms ease; }"
            ".overflow-tray-btn:hover { background-color: @ult_hover; border-color: transparent; border-radius: 6px; }"
            "#ultimate-show-desktop-btn { min-width: 10px; padding: 0px; margin: 0px 0px 0px 4px; border-radius: 0px; border: none; border-left: 1px solid alpha(@theme_fg_color, 0.25); background-color: alpha(@theme_fg_color, 0.04); transition: all 140ms ease; }"
            "#ultimate-show-desktop-btn:hover { background-color: alpha(@theme_selected_bg_color, 0.35); border-left: 1px solid @theme_selected_bg_color; border-radius: 0px; }"
            "button.win11-all-apps-pill { background-color: alpha(@theme_fg_color, 0.08); border-radius: 6px; padding: 4px 12px; border: 1px solid @ult_border; }"
            "button.win11-all-apps-pill:hover { background-color: alpha(@theme_selected_bg_color, 0.30); border-color: @theme_selected_bg_color; }"
            ".win11-footer-bar { background-color: alpha(@theme_fg_color, 0.05); border-top: 1px solid @ult_border; padding: 12px 18px; border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; }"
            "window.popup, .popup.csd { background-color: transparent; background: transparent; } "
            "menu.ultimate-themed-menu, .ultimate-themed-menu { background-color: alpha(@theme_bg_color, %.2f); background-image: none; color: @theme_fg_color; border: 1px solid @ult_border; border-radius: 8px; padding: 6px; }"
            "menu.ultimate-themed-menu menuitem { color: @theme_fg_color; padding: 5px 8px; border-radius: 5px; }"
            "menu.ultimate-themed-menu menuitem:hover { background-color: alpha(@theme_selected_bg_color, 0.28); color: @theme_fg_color; }"
            ".winck-btn {"
            "  background: transparent; background-image: none; color: @theme_fg_color; border: none; border-radius: 4px;"
            "  min-width: 18px; min-height: 18px; padding: 2px;"
            "}"
            ".winck-btn:hover { background-color: alpha(@theme_fg_color, 0.14); }"
            "#ultimate-global-menubar { border: none; border-bottom: none; box-shadow: none; background: transparent; background-image: none;  background: transparent; color: @theme_fg_color; border: none; padding: 0; }"
            "#ultimate-global-menubar > menuitem { color: @theme_fg_color; padding: 2px 8px; border-radius: 4px; }"
            "#ultimate-global-menubar > menuitem:hover { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; }"
            "#ultimate-toast-win { background-color: @ult_bg_solid; color: @theme_fg_color; border: 1px solid @ult_border; border-radius: 8px; }"
            ".notif-badge {"
            "  background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; border-radius: 9px;"
            "  min-width: 14px; min-height: 14px; padding: 0 3px; font-size: 9px; font-weight: bold;"
            "}",
            defs, bar_a,
            "transparent", dock_radius,
            pop_a, pop_a
        );
    g_free(defs);

    gint dock_ph  = CLAMP(shell->dock_pad_h, 0, 40);
    const gchar *ico_sh  = shell->dock_icon_shadow ? "0 3px 5px rgba(0, 0, 0, 0.55)" : "none";

    gint wck_pad = CLAMP(shell->winck_btn_pad, 0, 20);
    gchar *dock_extra_css = g_strdup_printf(
        " #ultimate-macos-dock,"
        " #ultimate-dock-box {"
        "   background: transparent;"
        "   background-color: transparent;"
        "   background-image: none;"
        "   border: none;"
        "   box-shadow: none;"
        "   padding: 0px %dpx;"
        " }"
        " #ultimate-dock-box button.taskbar-item-btn,"
        " #ultimate-dock-box button.taskbar-item-btn:hover,"
        " #ultimate-dock-box button.taskbar-item-btn:active,"
        " #ultimate-dock-box button.taskbar-item-btn:checked,"
        " #ultimate-dock-box button.taskbar-item-btn:focus {"
        "   background: transparent;"
        "   background-color: transparent;"
        "   background-image: none;"
        "   border-color: transparent;"
        "   box-shadow: none;"
        "   outline: none;"
        "   padding: 2px %dpx;"
        " }"
        " #ultimate-dock-box image {"
        "   -gtk-icon-shadow: %s;"
        " }"
        " #ultimate-global-menubar { border: none; border-bottom: none; box-shadow: none; background: transparent; background-image: none; "
        "   padding: 0px;"
        "   margin: 0px;"
        " }"
        " #ultimate-global-menubar > menuitem {"
        "   padding: 2px 7px;"
        "   margin: 0px;"
        "   min-width: 0px;"
        " }"
        " button.winck-btn, button.winck-btn:hover, button.winck-btn:active, button.winck-btn:checked, button.winck-btn:focus {"
        "   background: transparent; background-color: transparent; background-image: none;"
        "   border: none; border-color: transparent; box-shadow: none; outline: none;"
        "   min-width: 0px; min-height: 0px; padding: 0px; margin: 0px %dpx;"
        " }"
        " .ultimate-popup-win entry, .ultimate-popup-win entry placeholder, entry.win11-search-pill {"
        "   font-weight: %d;"
        " }"
        " .ultimate-popup-win.fullscreen-launchpad, window.ultimate-popup-win.fullscreen-launchpad.background {"
        "   border-radius: 0px;"
        "   border: none;"
        "   box-shadow: none;"
        " }"
        " calendar:indeterminate, GtkCalendar:indeterminate, calendar.view:indeterminate {"
        "   color: rgba(0,0,0,0);"
        "   background: transparent;"
        "   background-color: transparent;"
        "   text-shadow: none;"
        " }",
        MAX(6, dock_ph),
        MAX(1, dock_ph / 3), ico_sh,
        wck_pad / 2,
        (shell->start_style_cat_weight[CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1)] >= 700) ? 700 :
        ((shell->start_style_cat_weight[CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1)] >= 500) ? 600 : 400)
    );
    gchar *full_css = g_strconcat(css, dock_extra_css, NULL);
    gtk_css_provider_load_from_data(shell->dynamic_css, full_css, -1, NULL);
    g_free(full_css);
    g_free(dock_extra_css);
    g_free(css);

    shell_apply_appearance_css(shell);
    {
        ShellPalette pl;
        shell_palette_get(shell, &pl);
        shell->dark_mode = pl.dark;
        /* xfce4-panel's own "Dark mode" makes GTK draw the dark variant of the theme in
           the panel (popups, settings window). Keep it in step with the shell colours. */
        gboolean want = (shell->color_mode == COLOR_FOLLOW_GTK) ? FALSE : pl.dark;
        if (shell->channel && xfconf_channel_get_bool(shell->channel, "/panels/dark-mode", FALSE) != want)
            xfconf_channel_set_bool(shell->channel, "/panels/dark-mode", want);
    }
    if (shell->root_box) gtk_widget_queue_draw(shell->root_box);
    if (shell->dock_window) gtk_widget_queue_draw(shell->dock_window);
}

static void save_settings_to_xfconf(UltimateShell *shell) {
    if (!shell || !shell->channel) return;
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/layout_mode", (gint)shell->mode);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/color_mode", shell->color_mode);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/custom_bg", shell->custom_bg ? shell->custom_bg : "#000000");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/custom_fg", shell->custom_fg ? shell->custom_fg : "#FFFFFF");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/custom_accent", shell->custom_accent ? shell->custom_accent : "#1A73E8");

    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/win_panel_size", shell->win_panel_size);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/mac_panel_size", shell->mac_panel_size);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/ui_font", shell->ui_font ? shell->ui_font : "");
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/ui_radius", shell->ui_radius);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/ui_border_w", shell->ui_border_w);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/ui_border_color", shell->ui_border_color ? shell->ui_border_color : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/ui_hover_color", shell->ui_hover_color ? shell->ui_hover_color : "");
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ui_popup_shadow", shell->ui_popup_shadow);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ui_panel_frameless", shell->ui_panel_frameless);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ui_panel_shadow", shell->ui_panel_shadow);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/wm_fill_maximized", shell->wm_fill_maximized);

    /* Clock & Calendar */
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/cal_style", (gint)shell->cal_style);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_format", (gint)shell->clock_format);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_time_pt", shell->clock_time_pt);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_date_pt", shell->clock_date_pt);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_time_weight", shell->clock_time_weight);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_date_weight", shell->clock_date_weight);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/clock_24h", shell->clock_24h);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/clock_show_seconds", shell->clock_show_seconds);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_date_fmt", shell->clock_date_fmt);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/clock_line_spacing", shell->clock_line_spacing);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/clock_date_custom", shell->clock_date_custom ? shell->clock_date_custom : "");
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/clock_mac_show_day", shell->clock_mac_show_day);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/clock_mac_show_date", shell->clock_mac_show_date);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/cal_week_numbers", shell->cal_week_numbers);

    /* Taskbar & Windowck */
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/tb_align", (gint)shell->tb_align);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/ind_style", (gint)shell->ind_style);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/tb_icon_size", shell->tb_icon_size);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/tb_icon_spacing", shell->tb_icon_spacing);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/tb_icon_padding", shell->tb_icon_padding);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tb_show_inactive_ind", shell->tb_show_inactive_ind);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_ind_style", shell->dock_ind_style);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ind_urgent_pulse", shell->ind_urgent_pulse);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/ind_color", shell->ind_color ? shell->ind_color : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/ind_inactive_color", shell->ind_inactive_color ? shell->ind_inactive_color : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/ind_urgent_color", shell->ind_urgent_color ? shell->ind_urgent_color : "");
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/show_thumbnails", shell->show_thumbnails);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tb_middle_click_close", shell->tb_middle_click_close);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/show_desktop_enabled", shell->show_desktop_enabled);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/show_notif_icon", shell->show_notif_icon);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/winck_enabled", shell->winck_enabled);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/winck_pos", shell->winck_pos);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ws_enabled", shell->ws_enabled);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/ws_style", shell->ws_style);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/ws_size", shell->ws_size);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ws_scroll", shell->ws_scroll);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ws_wrap", shell->ws_wrap);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/ws_hide_single", shell->ws_hide_single);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/win_layout", shell->win_layout);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/titlebar_btn_side", shell->titlebar_btn_side);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/winck_btn_pad", shell->winck_btn_pad);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/winck_btn_size", shell->winck_btn_size);

    /* Start Menu & Per-Style Settings */
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/start_style", (gint)shell->start_style);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/win10_tile_sizes", shell->win10_tile_sizes ? shell->win10_tile_sizes : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/win10_tile_pos", shell->win10_tile_pos ? shell->win10_tile_pos : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/win10_groups", shell->win10_groups ? shell->win10_groups : "Pinned");
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/start_btn_mode", (gint)shell->start_btn_mode);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/start_btn_label", shell->start_btn_label ? shell->start_btn_label : "Start");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/start_btn_icon", shell->start_btn_icon ? shell->start_btn_icon : "");
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/start_opacity", shell->start_opacity);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_hover_effect", shell->start_hover_effect);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_generic_names", shell->start_generic_names);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_show_recommended", shell->start_show_recommended);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_recommend_files", shell->start_recommend_files);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_whisker_grid", shell->start_whisker_grid);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/win10_show_tiles", shell->win10_show_tiles);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_show_user_header", shell->start_show_user_header);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_show_lock", shell->start_show_lock);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_show_logout", shell->start_show_logout);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_show_restart", shell->start_show_restart);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_show_shutdown", shell->start_show_shutdown);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/start_style_fs_rows", shell->start_style_fs_rows);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/start_gnome_rows", shell->start_gnome_rows);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/win7_show_pictures", shell->win7_show_pictures);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/win7_show_music", shell->win7_show_music);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/win7_show_videos", shell->win7_show_videos);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/win7_show_trash", shell->win7_show_trash);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/win7_show_terminal", shell->win7_show_terminal);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/win7_places_weight", shell->win7_places_weight);

    for (int i = 0; i < START_STYLE_COUNT; i++) {
        gchar key[128];
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_w_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_w[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_h_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_h[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_icon_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_cat_icon[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_pt_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_cat_pt[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_weight_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_cat_weight[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_spacing_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_cat_spacing[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_list_icon_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_list_icon[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_grid_icon_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_grid_icon[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_label_pt_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_label_pt[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_grid_cols_%d", i);
        xfconf_channel_set_int(shell->channel, key, shell->start_style_grid_cols[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_mode_%d", i);
        xfconf_channel_set_int(shell->channel, key, (gint)shell->start_style_cat_mode[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_search_pos_%d", i);
        xfconf_channel_set_int(shell->channel, key, (gint)shell->start_style_search_pos[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_show_desc_%d", i);
        xfconf_channel_set_bool(shell->channel, key, shell->start_style_show_desc[i]);
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_hover_cat_%d", i);
        xfconf_channel_set_bool(shell->channel, key, shell->start_style_hover_cat[i]);
    }

    /* System Tray & Audio */
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_show_all", shell->tray_show_all);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_hide_new_default", shell->tray_hide_new_default);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_ibus_modern", shell->tray_ibus_modern);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/anim_mode", shell->anim_mode);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/anim_style", shell->anim_style);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/anim_speed", shell->anim_speed);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_menu_is_primary", shell->tray_menu_is_primary);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_quick_settings", shell->tray_quick_settings);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_qs_bluetooth", shell->tray_qs_bluetooth);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/gnome_style", shell->gnome_style);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/super_key_start", shell->super_key_start);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_request_symbolic", shell->tray_request_symbolic);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_enable_network", shell->tray_enable_network);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_enable_audio", shell->tray_enable_audio);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_enable_battery", shell->tray_enable_battery);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_enable_clipboard", shell->tray_enable_clipboard);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_enable_removable", shell->tray_enable_removable);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_enable_bluetooth", shell->tray_enable_bluetooth);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/tray_icon_size", shell->tray_icon_size);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/tray_icon_spacing", shell->tray_icon_spacing);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/audio_vol_step", shell->audio_vol_step);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/audio_max_vol", shell->audio_max_vol);

    /* macOS Dock */
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_pos", (gint)shell->dock_pos);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_autohide", (gint)shell->dock_autohide);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_hide_delay_sec", shell->dock_hide_delay_sec);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/dock_show_tooltips", shell->dock_show_tooltips);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_icon_size", shell->dock_icon_size);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_opacity", shell->dock_opacity);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/dock_magnify", shell->dock_magnify);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/dock_full_width", shell->dock_full_width);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_style", shell->dock_style);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_full_width_layout", shell->dock_full_width_layout);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/dock_show_trash", shell->dock_show_trash);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_border_w", shell->dock_border_w);
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/dock_border_color", shell->dock_border_color ? shell->dock_border_color : "#3894FF");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/dock_launchpad_icon", shell->dock_launchpad_icon ? shell->dock_launchpad_icon : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/mac_apple_icon", shell->mac_apple_icon ? shell->mac_apple_icon : "");
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/start_btn_icon_size", shell->start_btn_icon_size);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/mac_apple_icon_size", shell->mac_apple_icon_size);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_border_opacity", shell->dock_border_opacity);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_radius_val", shell->dock_radius_val);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_pad_h", shell->dock_pad_h);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_pad_v", shell->dock_pad_v);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_zoom_scale", shell->dock_zoom_scale);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/dock_edge_dist", shell->dock_edge_dist);
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/dock_icon_shadow", shell->dock_icon_shadow);

    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/prefs_w", shell->prefs_w);
    xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/prefs_h", shell->prefs_h);
}

void shell_save_layout_and_theme(UltimateShell *shell) {
    save_settings_to_xfconf(shell);
}

static void load_settings_from_xfconf(UltimateShell *shell) {
    if (xfconf_init(NULL)) {
        shell->channel = xfconf_channel_get("xfce4-panel");
    }
    shell->mode = shell->channel ? (ShellLayoutMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/layout_mode", LAYOUT_WINDOWS) : LAYOUT_WINDOWS;
    shell->theme_mode = shell->channel ? (ShellThemeMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/theme_mode", THEME_FOLLOW_XFCE) : THEME_FOLLOW_XFCE;
    /* colour mode; older versions stored theme_mode (0 follow, 1 custom, 2 installed theme) */
    if (shell->channel && xfconf_channel_has_property(shell->channel, "/plugins/ultimateshell/color_mode"))
        shell->color_mode = CLAMP(xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/color_mode", 0), 0, 3);
    else
        shell->color_mode = (shell->theme_mode == THEME_CUSTOM_COLOR) ? COLOR_CUSTOM : COLOR_FOLLOW_GTK;
    shell->dark_mode = FALSE;      /* worked out from the palette in shell_apply_theme() */
    shell->custom_bg = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/custom_bg", "#000000") : g_strdup("#000000");
    shell->custom_fg = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/custom_fg", "#FFFFFF") : g_strdup("#FFFFFF");
    shell->custom_accent = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/custom_accent", "#1A73E8") : g_strdup("#1A73E8");
    shell->specific_theme_name = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/specific_theme_name", "") : g_strdup("");

    shell->win_panel_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/win_panel_size", 42) : 42;
    shell->mac_panel_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/mac_panel_size", 28) : 28;
#define UI_S(k) (shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/" k, "") : g_strdup(""))
#define UI_I(k, d) (shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/" k, d) : d)
    g_free(shell->ui_font);         shell->ui_font = UI_S("ui_font");
    g_free(shell->ui_border_color); shell->ui_border_color = UI_S("ui_border_color");
    g_free(shell->ui_hover_color);  shell->ui_hover_color = UI_S("ui_hover_color");
    shell->ui_radius   = CLAMP(UI_I("ui_radius", -1), -1, 24);
    shell->ui_border_w = CLAMP(UI_I("ui_border_w", -1), -1, 4);
    shell->ui_panel_frameless = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ui_panel_frameless", TRUE) : TRUE;
    shell->ui_panel_shadow = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ui_panel_shadow", TRUE) : TRUE;
    shell->wm_fill_maximized = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/wm_fill_maximized", TRUE) : TRUE;
    shell->ui_popup_shadow = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ui_popup_shadow", TRUE) : TRUE;
#undef UI_S
#undef UI_I

    /* Clock & Calendar */
    shell->cal_style = shell->channel ? (CalendarStyle)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/cal_style", CAL_STYLE_WIN11) : CAL_STYLE_WIN11;
    shell->clock_format = shell->channel ? (ClockFormatMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_format", CLOCK_FMT_TIME_TOP) : CLOCK_FMT_TIME_TOP;
    shell->clock_time_pt = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_time_pt", 10) : 10;
    shell->clock_date_pt = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_date_pt", 9) : 9;
    shell->clock_time_weight = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_time_weight", 600) : 600;
    shell->clock_date_weight = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_date_weight", 400) : 400;
    shell->clock_24h = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/clock_24h", FALSE) : FALSE;
    shell->clock_show_seconds = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/clock_show_seconds", FALSE) : FALSE;
    shell->clock_date_fmt = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_date_fmt", 5) : 5;   /* new: your language's date */
    shell->clock_date_custom = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/clock_date_custom", "%a %-d %b") : g_strdup("%a %-d %b");
    shell->clock_mac_show_day  = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/clock_mac_show_day", TRUE) : TRUE;
    shell->clock_mac_show_date = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/clock_mac_show_date", TRUE) : TRUE;
    shell->cal_week_numbers    = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/cal_week_numbers", FALSE) : FALSE;
    shell->clock_line_spacing = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/clock_line_spacing", 0) : 0;

    /* Taskbar & Windowck */
    shell->tb_align = shell->channel ? (TaskbarAlignMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/tb_align", TASKBAR_ALIGN_CENTER) : TASKBAR_ALIGN_CENTER;
    shell->ind_style = shell->channel ? (TaskbarIndicatorStyle)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/ind_style", IND_STYLE_WIN11) : IND_STYLE_WIN11;
    shell->tb_icon_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/tb_icon_size", 24) : 24;
    shell->tb_icon_spacing = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/tb_icon_spacing", 4) : 4;
    shell->tb_icon_padding = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/tb_icon_padding", 2) : 2;
    shell->tb_show_inactive_ind = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tb_show_inactive_ind", TRUE) : TRUE;
    shell->dock_ind_style = shell->channel ? CLAMP(xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_ind_style", DOCK_IND_PILL), 0, 4) : DOCK_IND_PILL;
    shell->ind_urgent_pulse = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ind_urgent_pulse", TRUE) : TRUE;
    g_free(shell->ind_color);
    shell->ind_color = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/ind_color", "") : g_strdup("");
    g_free(shell->ind_inactive_color);
    shell->ind_inactive_color = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/ind_inactive_color", "") : g_strdup("");
    g_free(shell->ind_urgent_color);
    shell->ind_urgent_color = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/ind_urgent_color", "") : g_strdup("");
    shell->show_thumbnails = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/show_thumbnails", TRUE) : TRUE;
    shell->tb_middle_click_close = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tb_middle_click_close", FALSE) : FALSE;
    shell->show_desktop_enabled = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/show_desktop_enabled", TRUE) : TRUE;
    shell->show_notif_icon = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/show_notif_icon", TRUE) : TRUE;
    shell->winck_enabled = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/winck_enabled", FALSE) : FALSE;
    shell->winck_pos = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/winck_pos", 1) : 1;   /* default: far right */
    shell->ws_enabled     = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ws_enabled", TRUE) : TRUE;
    shell->ws_style       = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/ws_style", 0) : 0;
    shell->ws_size        = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/ws_size", 7) : 7;
    shell->ws_scroll      = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ws_scroll", TRUE) : TRUE;
    shell->ws_wrap        = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ws_wrap", FALSE) : FALSE;
    shell->ws_hide_single = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/ws_hide_single", TRUE) : TRUE;
    /* first run: pick the layout that matches the old "Taskbar Alignment" choice */
    shell->win_layout = shell->channel
        ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/win_layout",
                                 shell->tb_align == TASKBAR_ALIGN_LEFT ? WIN_LAYOUT_CLASSIC : WIN_LAYOUT_START_WS)
        : WIN_LAYOUT_START_WS;
    shell->win_layout = CLAMP(shell->win_layout, 0, 3);
    shell->titlebar_btn_side = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/titlebar_btn_side", 0) : 0;
    if (shell->titlebar_btn_side == 1) { shell->winck_pos = 0; shell->titlebar_btn_side = 0; }
    else if (shell->titlebar_btn_side == 2) { shell->winck_pos = 1; shell->titlebar_btn_side = 0; }
    else if (shell->titlebar_btn_side != 3) shell->titlebar_btn_side = 0;
    shell->winck_btn_pad = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/winck_btn_pad", 4) : 4;
    shell->winck_btn_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/winck_btn_size", 22) : 22;

    /* Start Menu & Per-Style Settings */
    shell->start_style = shell->channel ? (StartMenuStyle)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/start_style", START_STYLE_WIN11) : START_STYLE_WIN11;
    shell->start_style = (StartMenuStyle)CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    g_free(shell->win10_tile_sizes);
    shell->win10_tile_sizes = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/win10_tile_sizes", "") : g_strdup("");
    g_free(shell->win10_tile_pos);
    shell->win10_tile_pos = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/win10_tile_pos", "") : g_strdup("");
    g_free(shell->win10_groups);
    shell->win10_groups = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/win10_groups", "Pinned") : g_strdup("Pinned");
    shell->start_btn_mode = shell->channel ? (StartButtonMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/start_btn_mode", START_BTN_MODE_ICON_ONLY) : START_BTN_MODE_ICON_ONLY;
    shell->start_btn_label = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/start_btn_label", "Start") : g_strdup("Start");
    shell->start_btn_icon = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/start_btn_icon", "") : g_strdup("");
    shell->start_opacity = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/start_opacity", 95) : 95;
    shell->start_hover_effect = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_hover_effect", TRUE) : TRUE;
    shell->start_generic_names = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_generic_names", FALSE) : FALSE;
    shell->start_show_recommended = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_show_recommended", FALSE) : FALSE;
    shell->start_recommend_files = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_recommend_files", TRUE) : TRUE;
    shell->start_whisker_grid = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_whisker_grid", FALSE) : FALSE;
    shell->win10_show_tiles = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/win10_show_tiles", TRUE) : TRUE;
    shell->start_show_user_header = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_show_user_header", TRUE) : TRUE;
    shell->start_show_lock = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_show_lock", TRUE) : TRUE;
    shell->start_show_logout = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_show_logout", TRUE) : TRUE;
    shell->start_show_restart = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_show_restart", TRUE) : TRUE;
    shell->start_show_shutdown = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_show_shutdown", TRUE) : TRUE;
    shell->start_style_fs_rows = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/start_style_fs_rows", 4) : 4;
    shell->start_gnome_rows = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/start_gnome_rows", 3) : 3;
    shell->start_favorites = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/start_favorites", "xfce4-terminal.desktop;thunar.desktop;firefox.desktop;xfce4-settings-manager.desktop") : g_strdup("xfce4-terminal.desktop;thunar.desktop;firefox.desktop;xfce4-settings-manager.desktop");
    shell->win7_show_pictures = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/win7_show_pictures", TRUE) : TRUE;
    shell->win7_show_music = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/win7_show_music", TRUE) : TRUE;
    shell->win7_show_videos = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/win7_show_videos", TRUE) : TRUE;
    shell->win7_show_trash = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/win7_show_trash", TRUE) : TRUE;
    shell->win7_show_terminal = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/win7_show_terminal", TRUE) : TRUE;
    shell->win7_places_weight = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/win7_places_weight", 600) : 600;

    gint def_w[START_STYLE_COUNT] = { 420, 520, 600, 540, 900, 760, 900 };
    gint def_h[START_STYLE_COUNT] = { 520, 540, 620, 540, 680, 600, 680 };
    for (int i = 0; i < START_STYLE_COUNT; i++) {
        gchar key[128];
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_w_%d", i);
        shell->start_style_w[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, def_w[i]) : def_w[i];
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_h_%d", i);
        shell->start_style_h[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, def_h[i]) : def_h[i];
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_icon_%d", i);
        shell->start_style_cat_icon[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, 20) : 20;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_pt_%d", i);
        shell->start_style_cat_pt[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, 10) : 10;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_weight_%d", i);
        shell->start_style_cat_weight[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, 600) : 600;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_spacing_%d", i);
        shell->start_style_cat_spacing[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, 4) : 4;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_list_icon_%d", i);
        shell->start_style_list_icon[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, 28) : 28;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_grid_icon_%d", i);
        gint def_gi = (i == START_STYLE_GNOME) ? 48 : 42;   /* GNOME App Grid: 8 × 3 of 48 px icons */
        shell->start_style_grid_icon[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, def_gi) : def_gi;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_label_pt_%d", i);
        shell->start_style_label_pt[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, 10) : 10;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_grid_cols_%d", i);
        gint def_gc = (i == START_STYLE_GNOME) ? 8 : 6;
        shell->start_style_grid_cols[i] = shell->channel ? xfconf_channel_get_int(shell->channel, key, def_gc) : def_gc;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_cat_mode_%d", i);
        shell->start_style_cat_mode[i] = shell->channel ? (StartCatDisplayMode)xfconf_channel_get_int(shell->channel, key, START_CAT_DISPLAY_ICON_LABEL) : START_CAT_DISPLAY_ICON_LABEL;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_search_pos_%d", i);
        shell->start_style_search_pos[i] = shell->channel ? (StartSearchPosMode)xfconf_channel_get_int(shell->channel, key, START_SEARCH_POS_DEFAULT) : START_SEARCH_POS_DEFAULT;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_show_desc_%d", i);
        shell->start_style_show_desc[i] = shell->channel ? xfconf_channel_get_bool(shell->channel, key, TRUE) : TRUE;
        g_snprintf(key, sizeof(key), "/plugins/ultimateshell/start_hover_cat_%d", i);
        shell->start_style_hover_cat[i] = shell->channel ? xfconf_channel_get_bool(shell->channel, key, TRUE) : TRUE;
    }
    /* the first 0.4.1 test build saved the GNOME App Grid with the generic 6 columns / 42 px */
    if (shell->channel && !xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_gnome_defaults_set", FALSE)) {
        shell->start_style_grid_cols[START_STYLE_GNOME] = 8;
        shell->start_style_grid_icon[START_STYLE_GNOME] = 48;
        xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_gnome_defaults_set", TRUE);
    }
    if (shell->channel && !xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/start_win10_defaults_set", FALSE)) {
        shell->start_style_grid_cols[START_STYLE_WIN10] = 4;   /* medium tiles per row */
        xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/start_win10_defaults_set", TRUE);
    }

    /* System Tray & Audio */
    shell->tray_show_all = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_show_all", FALSE) : FALSE;
    shell->tray_hide_new_default = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_hide_new_default", FALSE) : FALSE;
    shell->tray_ibus_modern = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_ibus_modern", TRUE) : TRUE;
    shell->anim_mode  = CLAMP(shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/anim_mode", 1) : 1, 0, 2);
    shell->anim_style = CLAMP(shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/anim_style",
                                                                     shell->anim_mode ? shell->anim_mode : 1) : 1, 1, 2);
    if (shell->anim_mode != 0) shell->anim_mode = shell->anim_style;
    shell->anim_speed = CLAMP(shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/anim_speed", 100) : 100, 50, 200);
    shell->tray_menu_is_primary = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_menu_is_primary", FALSE) : FALSE;
    shell->tray_quick_settings = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_quick_settings", FALSE) : FALSE;
    shell->tray_qs_bluetooth = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_qs_bluetooth", FALSE) : FALSE;
    shell->gnome_style = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/gnome_style", FALSE) : FALSE;
    shell->super_key_start = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/super_key_start", TRUE) : TRUE;
    shell->tray_request_symbolic = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_request_symbolic", FALSE) : FALSE;
    shell->tray_enable_network = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_enable_network", TRUE) : TRUE;
    shell->tray_enable_audio = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_enable_audio", TRUE) : TRUE;
    shell->tray_enable_battery = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_enable_battery", TRUE) : TRUE;
    shell->tray_enable_clipboard = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_enable_clipboard", TRUE) : TRUE;
    shell->tray_enable_removable = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_enable_removable", TRUE) : TRUE;
    shell->tray_enable_bluetooth = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_enable_bluetooth", TRUE) : TRUE;
    shell->tray_icon_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/tray_icon_size", 18) : 18;
    shell->tray_icon_spacing = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/tray_icon_spacing", 4) : 4;
    shell->audio_vol_step = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/audio_vol_step", 5) : 5;
    shell->audio_max_vol = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/audio_max_vol", 100) : 100;
    /* Sound page: all of these are saved by the page, so read them back (they used to reset every start) */
#define US_B(k, d) (shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/" k, d) : d)
#define US_I(k, d) (shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/" k, d) : d)
#define US_S(k, d) (shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/" k, d) : g_strdup(d))
    shell->audio_key_shortcuts = US_B("audio_key_shortcuts", TRUE);
    shell->audio_show_rec_ind  = US_B("audio_show_rec_ind", FALSE);
    shell->audio_notif_mode    = US_I("audio_notif_mode", 0);
    shell->audio_mixer_cmd     = US_S("audio_mixer_cmd", "pavucontrol");
    shell->audio_mpris_enabled = US_B("audio_mpris_enabled", TRUE);
    shell->audio_mpris_keys    = US_B("audio_mpris_keys", TRUE);
    shell->audio_mpris_all     = US_B("audio_mpris_all", FALSE);
    shell->audio_mpris_focus   = US_B("audio_mpris_focus", FALSE);
    shell->audio_known_players      = US_S("audio_known_players", "strawberry;vlc");
    shell->audio_ignored_players    = US_S("audio_ignored_players", "");
    /* Persistent is now an explicit list (empty = none). First run: the two defaults. */
    shell->audio_persistent_players = US_S("audio_persistent_players", "strawberry;vlc");
#undef US_B
#undef US_I
#undef US_S

    /* macOS Dock */
    shell->dock_pos = shell->channel ? (DockPositionMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_pos", DOCK_POS_BOTTOM) : DOCK_POS_BOTTOM;
    shell->dock_autohide = shell->channel ? (DockAutohideMode)xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_autohide", DOCK_HIDE_INTELLIGENT) : DOCK_HIDE_INTELLIGENT;
    shell->dock_hide_delay_sec = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_hide_delay_sec", 10) : 10;
    shell->dock_show_tooltips = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/dock_show_tooltips", FALSE) : FALSE;
    shell->dock_icon_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_icon_size", 36) : 36;
    shell->dock_opacity = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_opacity", 88) : 88;
    shell->dock_magnify = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/dock_magnify", TRUE) : TRUE;
    shell->dock_full_width = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/dock_full_width", FALSE) : FALSE;
    shell->dock_full_width_layout = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_full_width_layout", 0) : 0;
    shell->dock_style = shell->channel ? CLAMP(xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_style", 0), 0, 1) : 0;
    if (shell->dock_style == 1) {   /* Ubuntu dock: left edge, full height, no zoom */
        shell->dock_pos = DOCK_POS_LEFT; shell->dock_full_width = TRUE; shell->dock_magnify = FALSE;
    }
    shell->dock_show_trash = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/dock_show_trash", TRUE) : TRUE;
    shell->dock_border_w = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_border_w", 1) : 1;
    shell->dock_border_color = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/dock_border_color", "#3894FF") : g_strdup("#3894FF");
    shell->dock_launchpad_icon = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/dock_launchpad_icon", "") : g_strdup("");
    shell->mac_apple_icon = shell->channel ? xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/mac_apple_icon", "") : g_strdup("");
    shell->start_btn_icon_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/start_btn_icon_size", 24) : 24;
    shell->mac_apple_icon_size = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/mac_apple_icon_size", 18) : 18;
    shell->dock_border_opacity = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_border_opacity", 65) : 65;
    shell->dock_radius_val = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_radius_val", 16) : 16;
    shell->dock_pad_h = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_pad_h", 12) : 12;
    shell->dock_pad_v = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_pad_v", 6) : 6;
    shell->dock_zoom_scale = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_zoom_scale", 148) : 148;
    shell->dock_edge_dist = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/dock_edge_dist", 6) : 6;
    shell->dock_icon_shadow = shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/dock_icon_shadow", TRUE) : TRUE;

    shell->prefs_w = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/prefs_w", 640) : 640;
    shell->prefs_h = shell->channel ? xfconf_channel_get_int(shell->channel, "/plugins/ultimateshell/prefs_h", 680) : 680;
}

/* =========================================================================
 * Popup Window Positioning & Lifecycle Helpers
 * ========================================================================= */
/* -------------------------------------------------------------------------
 * One placement rule for every popup & menu attached to a panel/dock button:
 * open away from the bar, 6 px past the bar's visible edge; centered on the
 * button (or start-aligned for big popups), kept inside the work area.
 * ------------------------------------------------------------------------- */
#define SHELL_POPUP_GAP 6

typedef struct {
    GtkPositionType edge;      /* which screen edge the bar sits on */
    GdkRectangle    btn;       /* button, screen coords */
    gint            bar_line;  /* bar's inner edge, screen coords (x for side bars, y otherwise) */
    GdkRectangle    area;      /* monitor work area */
    GdkRectangle    mon;       /* monitor geometry */
} ShellBarAnchor;

static gboolean shell_bar_anchor(GtkWidget *btn, ShellBarAnchor *a) {
    if (!btn || !gtk_widget_get_window(btn)) return FALSE;
    GtkWidget *top = gtk_widget_get_toplevel(btn);
    if (!GTK_IS_WINDOW(top) || !gtk_widget_get_window(top)) return FALSE;

    gint ox = 0, oy = 0, bx = 0, by = 0;
    gdk_window_get_origin(gtk_widget_get_window(top), &ox, &oy);
    gtk_widget_translate_coordinates(btn, top, 0, 0, &bx, &by);
    a->btn.x = ox + bx; a->btn.y = oy + by;
    a->btn.width = gtk_widget_get_allocated_width(btn);
    a->btn.height = gtk_widget_get_allocated_height(btn);

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_monitor_at_point(display, a->btn.x + a->btn.width / 2, a->btn.y + a->btn.height / 2);
    if (!m) m = gdk_display_get_primary_monitor(display);
    a->mon = (GdkRectangle){ 0, 0, 1920, 1080 };
    if (m) { gdk_monitor_get_geometry(m, &a->mon); gdk_monitor_get_workarea(m, &a->area); }
    else a->area = a->mon;

    UltimateShell *dock_shell = g_object_get_data(G_OBJECT(top), "ultimate-dock-shell");
    if (dock_shell) {
        gint ext = dock_window_shelf_extent(dock_shell);
        if (dock_shell->dock_pos == DOCK_POS_LEFT)       { a->edge = GTK_POS_LEFT;  a->bar_line = a->mon.x + ext; }
        else if (dock_shell->dock_pos == DOCK_POS_RIGHT) { a->edge = GTK_POS_RIGHT; a->bar_line = a->mon.x + a->mon.width - ext; }
        else                                             { a->edge = GTK_POS_BOTTOM; a->bar_line = a->mon.y + a->mon.height - ext; }
        /* never place the popup across the button itself */
        if (a->edge == GTK_POS_BOTTOM) a->bar_line = MIN(a->bar_line, a->btn.y);
        if (a->edge == GTK_POS_LEFT)   a->bar_line = MAX(a->bar_line, a->btn.x + a->btn.width);
        if (a->edge == GTK_POS_RIGHT)  a->bar_line = MIN(a->bar_line, a->btn.x);
        return TRUE;
    }

    gint tw = gtk_widget_get_allocated_width(top), th = gtk_widget_get_allocated_height(top);
    if (tw >= th) {
        if (oy + th / 2 < a->mon.y + a->mon.height / 2) { a->edge = GTK_POS_TOP;    a->bar_line = oy + th; }
        else                                             { a->edge = GTK_POS_BOTTOM; a->bar_line = oy; }
    } else {
        if (ox + tw / 2 < a->mon.x + a->mon.width / 2)  { a->edge = GTK_POS_LEFT;   a->bar_line = ox + tw; }
        else                                             { a->edge = GTK_POS_RIGHT;  a->bar_line = ox; }
    }
    return TRUE;
}

static void shell_bar_place(const ShellBarAnchor *a, gint pw, gint ph, gboolean centered, gint *ox, gint *oy) {
    const gint G = SHELL_POPUP_GAP;
    gint x, y;
    if (a->edge == GTK_POS_LEFT || a->edge == GTK_POS_RIGHT) {
        x = (a->edge == GTK_POS_LEFT) ? a->bar_line + G : a->bar_line - pw - G;
        y = centered ? a->btn.y + a->btn.height / 2 - ph / 2 : a->btn.y;
        y = MIN(y, a->area.y + a->area.height - ph - G);
        y = MAX(y, a->area.y + G);
    } else {
        y = (a->edge == GTK_POS_TOP) ? a->bar_line + G : a->bar_line - ph - G;
        x = centered ? a->btn.x + a->btn.width / 2 - pw / 2 : a->btn.x;
        if (!centered && x < a->mon.x + 16) x = a->mon.x + G;   /* Start menu snaps to the corner */
        x = MIN(x, a->mon.x + a->mon.width - pw - G);
        x = MAX(x, a->mon.x + G);
    }
    /* keep it on screen along the away-from-bar axis too */
    y = CLAMP(y, a->mon.y + G, MAX(a->mon.y + G, a->mon.y + a->mon.height - ph - G));
    x = CLAMP(x, a->mon.x + G, MAX(a->mon.x + G, a->mon.x + a->mon.width - pw - G));
    *ox = x; *oy = y;
}

/* Public: place a popup window of pw×ph next to a bar button (used by Launchpad etc.) */
gboolean shell_place_popup_at_button(GtkWidget *btn, gint pw, gint ph, gboolean centered, gint *x, gint *y) {
    ShellBarAnchor a;
    if (!shell_bar_anchor(btn, &a)) return FALSE;
    shell_bar_place(&a, pw, ph, centered, x, y);
    return TRUE;
}

/* Public: pop a GtkMenu up next to a bar button, by the same rule */
void shell_popup_menu_at_button(GtkMenu *menu, GtkWidget *btn, const GdkEvent *ev) {
    ShellBarAnchor a;
    GtkWidget *top = btn ? gtk_widget_get_toplevel(btn) : NULL;
    if (!top || !gtk_widget_get_window(top) || !shell_bar_anchor(btn, &a)) {
        gtk_menu_popup_at_pointer(menu, ev);
        return;
    }
    gint ox = 0, oy = 0;
    gdk_window_get_origin(gtk_widget_get_window(top), &ox, &oy);
    GdkRectangle r;
    GdkGravity rg, mg;
    gint dx = 0, dy = 0;
    switch (a.edge) {
    case GTK_POS_LEFT:
        r = (GdkRectangle){ a.bar_line - ox - 1, a.btn.y - oy, 1, a.btn.height };
        rg = GDK_GRAVITY_EAST;  mg = GDK_GRAVITY_WEST;  dx = SHELL_POPUP_GAP; break;
    case GTK_POS_RIGHT:
        r = (GdkRectangle){ a.bar_line - ox, a.btn.y - oy, 1, a.btn.height };
        rg = GDK_GRAVITY_WEST;  mg = GDK_GRAVITY_EAST;  dx = -SHELL_POPUP_GAP; break;
    case GTK_POS_TOP:
        r = (GdkRectangle){ a.btn.x - ox, a.bar_line - oy - 1, a.btn.width, 1 };
        rg = GDK_GRAVITY_SOUTH; mg = GDK_GRAVITY_NORTH; dy = SHELL_POPUP_GAP; break;
    default:
        r = (GdkRectangle){ a.btn.x - ox, a.bar_line - oy, a.btn.width, 1 };
        rg = GDK_GRAVITY_NORTH; mg = GDK_GRAVITY_SOUTH; dy = -SHELL_POPUP_GAP; break;
    }
    g_object_set(menu, "rect-anchor-dx", dx, "rect-anchor-dy", dy,
                 "anchor-hints", GDK_ANCHOR_FLIP | GDK_ANCHOR_SLIDE | GDK_ANCHOR_RESIZE, NULL);
    gtk_menu_popup_at_rect(menu, gtk_widget_get_window(top), &r, rg, mg, ev);
}

void shell_reposition_popup_window(GtkToggleButton *btn, GtkWidget *popup_win) {
    if (!popup_win) return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "is-fullscreen-start")) == 1) {
        gint fs_x = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "fs-x"));
        gint fs_y = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "fs-y"));
        gint fs_w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "fs-w"));
        gint fs_h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "fs-h"));
        if (fs_w > 200 && fs_h > 200) {
            gtk_widget_set_size_request(popup_win, fs_w, fs_h);
            gtk_window_resize(GTK_WINDOW(popup_win), fs_w, fs_h);
            gtk_window_move(GTK_WINDOW(popup_win), fs_x, fs_y);
        }
        return;
    }
    if (!btn || !gtk_widget_get_window(GTK_WIDGET(btn))) return;
    if (g_object_get_data(G_OBJECT(popup_win), "dock-anchor-btn") != NULL) return;
    GtkWidget *outer_popup = NULL;
    {   /* button lives inside another popup (e.g. the tray overflow): place next to that popup's button */
        GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(btn));
        GtkWidget *outer = (top && top != popup_win && g_object_get_data(G_OBJECT(top), "is-ultimate-popup"))
                           ? g_object_get_data(G_OBJECT(top), "toggle-btn") : NULL;
        if (GTK_IS_WIDGET(outer) && gtk_widget_get_window(outer)) {
            btn = GTK_TOGGLE_BUTTON(outer);
            outer_popup = top;
        }
    }

    gboolean center_on_btn = (g_object_get_data(G_OBJECT(popup_win), "center-on-button") != NULL);

    GtkRequisition nat_req = { 0, 0 };
    gtk_widget_get_preferred_size(popup_win, NULL, &nat_req);
    gint pw = gtk_widget_get_allocated_width(popup_win);
    gint ph = gtk_widget_get_allocated_height(popup_win);
    /* If window was reset to 1x1 (like overflow_popup) or not yet allocated, use its natural requisition */
    if (pw <= 10 || (nat_req.width > 10 && center_on_btn)) pw = MAX(pw, nat_req.width);
    if (ph <= 10 || (nat_req.height > 10 && center_on_btn)) ph = MAX(ph, nat_req.height);
    if (pw <= 10) pw = 120;
    if (ph <= 10) ph = 60;

    gint x = 0, y = 0;
    if (shell_place_popup_at_button(GTK_WIDGET(btn), pw, ph, center_on_btn, &x, &y)) {
        /* never cover the popup it was opened from: sit beside it instead */
        if (GTK_IS_WINDOW(outer_popup) && gtk_widget_get_visible(outer_popup) && gtk_widget_get_window(outer_popup)) {
            gint ox = 0, oy = 0;
            gdk_window_get_origin(gtk_widget_get_window(outer_popup), &ox, &oy);
            gint ow = gtk_widget_get_allocated_width(outer_popup), oh = gtk_widget_get_allocated_height(outer_popup);
            GdkMonitor *m = gdk_display_get_monitor_at_point(gdk_display_get_default(), ox, oy);
            GdkRectangle g = { 0, 0, 1920, 1080 };
            if (m) gdk_monitor_get_geometry(m, &g);
            const gint G = 6;
            gboolean overlap = x < ox + ow && x + pw > ox && y < oy + oh && y + ph > oy;
            if (overlap) {
                gint tw = 0, th = 0;
                GtkWidget *bar = gtk_widget_get_toplevel(GTK_WIDGET(btn));
                if (bar) { tw = gtk_widget_get_allocated_width(bar); th = gtk_widget_get_allocated_height(bar); }
                if (tw >= th) {      /* horizontal bar: to the left of it, or to the right if no room */
                    x = (ox - pw - G >= g.x + G) ? ox - pw - G : ox + ow + G;
                    x = CLAMP(x, g.x + G, MAX(g.x + G, g.x + g.width - pw - G));
                } else {             /* side bar: above it, or below */
                    y = (oy - ph - G >= g.y + G) ? oy - ph - G : oy + oh + G;
                    y = CLAMP(y, g.y + G, MAX(g.y + G, g.y + g.height - ph - G));
                }
            }
        }
        /* remember the real spot: the opening slide animates towards it */
        g_object_set_data(G_OBJECT(popup_win), "anim-tx", GINT_TO_POINTER(x + 1));
        g_object_set_data(G_OBJECT(popup_win), "anim-ty", GINT_TO_POINTER(y + 1));
        gtk_window_move(GTK_WINDOW(popup_win), x, y);
    }
}

static void on_popup_win_size_allocate(GtkWidget *popup_win, GdkRectangle *alloc, GtkToggleButton *btn) {
    if (g_object_get_data(G_OBJECT(popup_win), "dock-anchor-btn") != NULL) return;
    if (gtk_widget_get_visible(popup_win) && btn) {
        shell_reposition_popup_window(btn, popup_win);
    }
}

/* =========================================================================
 * Animations
 * ========================================================================= */
static UltimateShell *g_anim_shell = NULL;

gint shell_anim_ms(gint base_ms) {
    if (!g_anim_shell || g_anim_shell->anim_mode == 0) return 0;
    gint ms = base_ms * CLAMP(g_anim_shell->anim_speed, 50, 200) / 100;
    return MAX(ms, 1);
}

static gboolean shell_compositing_active(void) {
    GdkScreen *scr = gdk_screen_get_default();
    return scr && gdk_screen_is_composited(scr);
}

/* hover / press transitions of every themed widget follow the setting */
static void shell_apply_animation_css(UltimateShell *shell) {
    static GtkCssProvider *prov = NULL;
    if (!prov) {
        prov = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
                                                  GTK_STYLE_PROVIDER_PRIORITY_USER + 5);
    }
    gchar *css;
    if (!shell || shell->anim_mode == 0) {
        css = g_strdup("* { transition: none; }");
    } else {
        gint ms = shell_anim_ms(140);
        css = g_strdup_printf(
            "button, menuitem, .ultimate-module-btn, .taskbar-item-btn, #ultimate-start-btn, #ultimate-clock-btn,"
            " .winck-btn, .app-grid-tile, .win10-tile, .ap-dot, .ap-icon-btn, scale slider, switch, switch slider {"
            " transition-duration: %dms; transition-timing-function: ease-out; }", ms);
    }
    gtk_css_provider_load_from_data(prov, css, -1, NULL);
    g_free(css);
}

typedef struct {
    GtkWidget *win;
    gint dx, dy;                 /* slide offset towards the bar (0 for Fade) */
    gint bx, by;                 /* closing: where the popup sat when it started to close */
    gdouble from;                /* closing: opacity at the start */
    gboolean closing;
    gint64 start_us, dur_us;
    guint id;
} PopupAnim;

static gboolean popup_anim_tick(gpointer data) {
    PopupAnim *a = data;
    gdouble t = (gdouble)(g_get_monotonic_time() - a->start_us) / a->dur_us;
    if (t >= 1.0 || !gtk_widget_get_visible(a->win)) t = 1.0;

    if (a->closing) {
        gdouble e = t * t * t;                               /* ease-in cubic */
        gtk_widget_set_opacity(a->win, a->from * (1.0 - e));
        if (a->dx || a->dy)
            gtk_window_move(GTK_WINDOW(a->win), a->bx + (gint)lround(a->dx * e), a->by + (gint)lround(a->dy * e));
        if (t >= 1.0) {
            GtkWidget *w = a->win;
            a->id = 0;
            g_object_set_data(G_OBJECT(w), "popup-anim", NULL);   /* frees a */
            if (gtk_widget_get_visible(w)) gtk_widget_hide(w);
            gtk_widget_set_opacity(w, 1.0);
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    gdouble e = 1.0 - pow(1.0 - t, 3.0);                 /* ease-out cubic */
    gtk_widget_set_opacity(a->win, e);
    /* Slide only towards a position the shell itself placed the popup at (read every frame,
       since the popup may be re-placed while it opens). Never guess: a guessed position
       sent popups to the top-left corner. */
    gint tx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a->win), "anim-tx")) - 1;
    gint ty = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a->win), "anim-ty")) - 1;
    if ((a->dx || a->dy) && tx >= 0 && ty >= 0) {
        if (t >= 1.0) gtk_window_move(GTK_WINDOW(a->win), tx, ty);
        else gtk_window_move(GTK_WINDOW(a->win), tx + (gint)lround(a->dx * (1.0 - e)), ty + (gint)lround(a->dy * (1.0 - e)));
    }
    if (t >= 1.0) {
        a->id = 0;                                   /* this source ends by returning REMOVE */
        g_object_set_data(G_OBJECT(a->win), "popup-anim", NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void popup_anim_free(gpointer p) {
    PopupAnim *a = p;
    if (a->id) g_source_remove(a->id);
    g_free(a);
}

/* Slide style: offset pointing from the popup towards the bar it belongs to */
static void popup_slide_offset(GtkWidget *win, GtkWidget *anchor, gint *dx, gint *dy) {
    *dx = *dy = 0;
    if (!g_anim_shell || g_anim_shell->anim_mode != 2) return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "is-fullscreen-start")) == 1) return;
    GtkWidget *dock_btn = g_object_get_data(G_OBJECT(win), "dock-anchor-btn");
    if (GTK_IS_WIDGET(dock_btn)) anchor = dock_btn;
    if (!GTK_IS_WIDGET(anchor) || !gtk_widget_get_window(anchor)) return;
    GtkWidget *top = gtk_widget_get_toplevel(anchor);
    if (!gtk_widget_get_window(top)) return;
    gint ax = 0, ay = 0;
    gdk_window_get_origin(gtk_widget_get_window(top), &ax, &ay);
    GdkMonitor *m = gdk_display_get_monitor_at_window(gdk_display_get_default(), gtk_widget_get_window(anchor));
    GdkRectangle g = { 0, 0, 1920, 1080 };
    if (m) gdk_monitor_get_geometry(m, &g);
    gint tw = gtk_widget_get_allocated_width(top);
    gint th = gtk_widget_get_allocated_height(top);
    const gint D = 14;
    if (tw >= th) *dy = (ay + th / 2 < g.y + g.height / 2) ? -D : D;   /* top bar: up, bottom bar: down */
    else          *dx = (ax + tw / 2 < g.x + g.width / 2)  ? -D : D;   /* side bar */
}

/* Popup entrance: Fade = fade in; Slide = fade in while sliding out of the bar */
static void on_popup_map_animate(GtkWidget *win, GtkWidget *anchor) {
    gint ms = shell_anim_ms(200);
    if (ms <= 0 || !shell_compositing_active()) { gtk_widget_set_opacity(win, 1.0); return; }
    PopupAnim *a = g_new0(PopupAnim, 1);
    a->win = win;
    popup_slide_offset(win, anchor, &a->dx, &a->dy);
    a->start_us = g_get_monotonic_time();
    a->dur_us = (gint64)ms * 1000;
    gtk_widget_set_opacity(win, 0.0);
    a->id = g_timeout_add(15, popup_anim_tick, a);
    g_object_set_data_full(G_OBJECT(win), "popup-anim", a, popup_anim_free);
}

/* Close a shell popup: Fade = fade out; Slide = fade out while sliding back into the bar.
   Instant when animations are off or without compositing. */
void shell_popup_hide(GtkWidget *win) {
    if (!GTK_IS_WIDGET(win) || !gtk_widget_get_visible(win)) return;
    PopupAnim *cur = g_object_get_data(G_OBJECT(win), "popup-anim");
    if (cur && cur->closing) return;                      /* already on its way out */
    gint ms = shell_anim_ms(160);
    if (ms <= 0 || !shell_compositing_active() || !gtk_widget_get_mapped(win)) {
        g_object_set_data(G_OBJECT(win), "popup-anim", NULL);
        gtk_widget_hide(win);
        gtk_widget_set_opacity(win, 1.0);
        return;
    }
    /* a popup can ask for a CSS class while it closes (full-screen grids: icons shrink away) */
    const gchar *cc = g_object_get_data(G_OBJECT(win), "close-class");
    if (cc) gtk_style_context_add_class(gtk_widget_get_style_context(win), cc);
    PopupAnim *a = g_new0(PopupAnim, 1);
    a->win = win;
    a->closing = TRUE;
    a->from = gtk_widget_get_opacity(win);
    popup_slide_offset(win, g_object_get_data(G_OBJECT(win), "toggle-btn"), &a->dx, &a->dy);
    gtk_window_get_position(GTK_WINDOW(win), &a->bx, &a->by);
    a->start_us = g_get_monotonic_time();
    a->dur_us = (gint64)ms * 1000;
    a->id = g_timeout_add(15, popup_anim_tick, a);
    g_object_set_data_full(G_OBJECT(win), "popup-anim", a, popup_anim_free);   /* replaces an opening one */
}

/* Opening again while it is still closing: stop the close and show it fully */
void shell_popup_cancel_close(GtkWidget *win) {
    if (!GTK_IS_WIDGET(win)) return;
    PopupAnim *cur = g_object_get_data(G_OBJECT(win), "popup-anim");
    if (cur && cur->closing) {
        g_object_set_data(G_OBJECT(win), "popup-anim", NULL);
        gtk_widget_set_opacity(win, 1.0);
        const gchar *cc = g_object_get_data(G_OBJECT(win), "close-class");
        if (cc) gtk_style_context_remove_class(gtk_widget_get_style_context(win), cc);
    }
}

/* placed elsewhere next time (dock anchor, fullscreen): don't slide to a stale spot */
/* Decide a moment later: clicking another icon in the outer popup first closes the open
   child (focus moves) and only then activates the outer popup / opens the next child. */
static gboolean outer_popup_check_later(gpointer data) {
    GtkWidget *parent = data;
    if (GTK_IS_WINDOW(parent) && gtk_widget_get_visible(parent)) {
        GtkWidget *child = g_object_get_data(G_OBJECT(parent), "child-popup");
        gboolean child_open = GTK_IS_WIDGET(child) && gtk_widget_get_visible(child);
        if (!child_open && !gtk_window_is_active(GTK_WINDOW(parent))) {
            gtk_widget_hide(parent);
            GtkWidget *pb = g_object_get_data(G_OBJECT(parent), "toggle-btn");
            if (GTK_IS_TOGGLE_BUTTON(pb)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pb), FALSE);
        }
    }
    g_object_unref(parent);
    return G_SOURCE_REMOVE;
}

static void on_popup_unmap_forget(GtkWidget *win, gpointer d) {
    /* a popup opened from inside another one closed: close the outer one too unless it is in use */
    GtkWidget *parent = g_object_get_data(G_OBJECT(win), "parent-popup");
    g_object_set_data(G_OBJECT(win), "parent-popup", NULL);
    if (GTK_IS_WINDOW(parent)) {
        if (g_object_get_data(G_OBJECT(parent), "child-popup") == win)
            g_object_set_data(G_OBJECT(parent), "child-popup", NULL);
        g_timeout_add(200, outer_popup_check_later, g_object_ref(parent));
    }
    g_object_set_data(G_OBJECT(win), "anim-tx", NULL);
    g_object_set_data(G_OBJECT(win), "anim-ty", NULL);
    g_object_set_data(G_OBJECT(win), "popup-anim", NULL);
    gtk_widget_set_opacity(win, 1.0);
    const gchar *cc = g_object_get_data(G_OBJECT(win), "close-class");
    if (cc) gtk_style_context_remove_class(gtk_widget_get_style_context(win), cc);
}

static gboolean on_popup_focus_out(GtkWidget *popup_win, GdkEventFocus *event, GtkToggleButton *btn) {
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "context-menu-open")) == 1) {
        return FALSE;
    }
    GtkWidget *child = g_object_get_data(G_OBJECT(popup_win), "child-popup");
    if (GTK_IS_WIDGET(child) && gtk_widget_get_visible(child)) return FALSE;   /* e.g. overflow → sound popup */
    gint64 *last_hide = g_new(gint64, 1);
    *last_hide = g_get_monotonic_time();
    g_object_set_data_full(G_OBJECT(popup_win), "last-focus-hide-us", last_hide, g_free);

    shell_popup_hide(popup_win);
    if (GTK_IS_TOGGLE_BUTTON(btn) && gtk_toggle_button_get_active(btn)) {
        gtk_toggle_button_set_active(btn, FALSE);
    }
    return FALSE;
}

/* Esc closes any shell popup (clock & calendar, tray popups, Quick Settings, ...). Runs after
   the popup's own key handling, so a popup that uses Esc itself (clear a search) still does. */
static gboolean on_popup_escape(GtkWidget *popup_win, GdkEventKey *ev, GtkToggleButton *btn) {
    if (ev->keyval != GDK_KEY_Escape) return FALSE;
    shell_popup_hide(popup_win);
    if (GTK_IS_TOGGLE_BUTTON(btn) && gtk_toggle_button_get_active(btn)) gtk_toggle_button_set_active(btn, FALSE);
    return TRUE;
}

GtkWidget *shell_create_popup_window(GtkWidget *toggle_btn, gint width, gint height, gboolean resizable) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_set_data(G_OBJECT(win), "is-ultimate-popup", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(win), "toggle-btn", toggle_btn);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "ultimate-popup-win");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
    gtk_window_set_resizable(GTK_WINDOW(win), resizable);

    GdkScreen *scr = gtk_widget_get_screen(win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis) gtk_widget_set_visual(win, vis);
    gtk_widget_set_app_paintable(win, FALSE);

    gtk_widget_add_events(win, GDK_FOCUS_CHANGE_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(on_popup_focus_out), toggle_btn);
    g_signal_connect_after(win, "key-press-event", G_CALLBACK(on_popup_escape), toggle_btn);
    g_signal_connect_after(win, "map", G_CALLBACK(on_popup_map_animate), toggle_btn);
    g_signal_connect(win, "unmap", G_CALLBACK(on_popup_unmap_forget), NULL);
    g_signal_connect(win, "size-allocate", G_CALLBACK(on_popup_win_size_allocate), toggle_btn);

    return win;
}

void shell_toggle_popup_window(GtkToggleButton *btn, GtkWidget *popup_win) {
    if (!popup_win) return;
    g_object_set_data(G_OBJECT(popup_win), "dock-anchor-btn", NULL);
    if (gtk_toggle_button_get_active(btn)) {
        GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(btn));
        if (top && top != popup_win && g_object_get_data(G_OBJECT(top), "is-ultimate-popup")) {
            g_object_set_data(G_OBJECT(popup_win), "parent-popup", top);
            g_object_set_data(G_OBJECT(top), "child-popup", popup_win);
        }
        shell_popup_cancel_close(popup_win);
        shell_reposition_popup_window(btn, popup_win);
        gtk_widget_show_all(popup_win);
        shell_reposition_popup_window(btn, popup_win);
        gtk_window_present(GTK_WINDOW(popup_win));
    } else {
        shell_popup_hide(popup_win);
    }
}

/* =========================================================================
 * Layout Mode Switching (macOS Global Menu + Dock vs Windows Taskbar)
 * ========================================================================= */
static void sync_xfce_panel_edge_and_size(UltimateShell *shell, gboolean is_mac) {
    if (!shell || !shell->channel) return;

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *mon = gdk_display_get_primary_monitor(dpy);
    if (!mon) mon = gdk_display_get_monitor(dpy, 0);
    GdkRectangle geom = { 0, 0, 1920, 1080 };
    if (mon) gdk_monitor_get_geometry(mon, &geom);

    gint target_sz = is_mac ? CLAMP(shell->mac_panel_size, 22, 48)
                            : CLAMP(shell->win_panel_size, 32, 64);
    /* XFCE Panel snap positions: p=6 is Top-Center, p=8 is Bottom-Center */
    gint target_y  = is_mac ? geom.y : (geom.y + geom.height - target_sz);
    gchar *pos_str = g_strdup_printf("p=%d;x=%d;y=%d",
                                     is_mac ? 6 : 8,
                                     geom.x + (geom.width / 2),
                                     target_y);

    shell->applying_layout_size = TRUE;
    /* the panel answers later, and may first report its old size: ignore it for a while */
    shell->size_guard_until = MAX(shell->size_guard_until, g_get_monotonic_time() + 3 * G_USEC_PER_SEC);
    const gchar *panels[] = { "/panels/panel-1", "/panels/panel-0" };
    for (gsize i = 0; i < G_N_ELEMENTS(panels); i++) {
        gchar prop_pos[64], prop_sz[64], prop_lock[64];
        g_snprintf(prop_pos, sizeof(prop_pos), "%s/position", panels[i]);
        g_snprintf(prop_sz, sizeof(prop_sz), "%s/size", panels[i]);
        g_snprintf(prop_lock, sizeof(prop_lock), "%s/position-locked", panels[i]);
        if (xfconf_channel_has_property(shell->channel, prop_pos) || i == 0) {
            gchar *cur_pos = xfconf_channel_get_string(shell->channel, prop_pos, "");
            guint  cur_sz  = xfconf_channel_get_uint(shell->channel, prop_sz, 0);
            gboolean cur_lock = xfconf_channel_get_bool(shell->channel, prop_lock, FALSE);

            if (g_strcmp0(cur_pos, pos_str) != 0) {
                xfconf_channel_set_string(shell->channel, prop_pos, pos_str);
            }
            if (cur_sz != (guint)target_sz) {
                xfconf_channel_set_uint(shell->channel, prop_sz, (guint)target_sz);
            }
            if (!cur_lock) {
                xfconf_channel_set_bool(shell->channel, prop_lock, TRUE);
            }
            g_free(cur_pos);
        }
    }
    shell->applying_layout_size = FALSE;
    g_free(pos_str);
}


/* GTK apps send their menus to the top bar through appmenu-gtk-module, loaded via the
   xsettings "Gtk/Modules" list. Add/remove ONLY our module so other modules the user has
   (e.g. canberra-gtk-module for sounds) are kept. */
static gboolean appmenu_module_installed(void) {
    const gchar *bases[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
                             "/usr/lib/i386-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/local/lib", NULL };
    for (gint i = 0; bases[i]; i++) {
        gchar *p = g_build_filename(bases[i], "gtk-3.0", "modules", "libappmenu-gtk-module.so", NULL);
        gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
        g_free(p);
        if (ok) return TRUE;
    }
    return FALSE;
}

static void notify_appmenu_missing_once(void) {
    static gboolean shown = FALSE;
    if (shown) return;
    shown = TRUE;
    gchar *argv[] = { "notify-send", "-a", "Ultimate Shell", "-i", "dialog-information",
        "Global menus for GTK apps",
        "Install the app-menu helper so GTK apps (Thunar, Mousepad, GIMP…) show their menus in the top bar:\n"
        "sudo apt install appmenu-gtk3-module appmenu-gtk2-module\nThen log out and back in.", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}


/* Firefox / Thunderbird (and forks) have their own switch for the global menu:
   widget.gtk.global-menu.enabled. We set it in each profile's user.js inside a marked
   block, so it follows the layout (on in macOS layout, off in Windows layout).
   Takes effect the next time the browser starts. */
#define ULT_MOZ_BEGIN "// >>> Ultimate Shell: global menu (managed automatically)"
#define ULT_MOZ_END   "// <<< Ultimate Shell: global menu"

static void mozilla_profile_set_global_menu(const gchar *profile_dir, gboolean on) {
    gchar *userjs = g_build_filename(profile_dir, "user.js", NULL);
    gchar *old = NULL;
    g_file_get_contents(userjs, &old, NULL, NULL);

    /* drop our previous block, keep everything else the user has */
    GString *out = g_string_new("");
    if (old) {
        gchar **lines = g_strsplit(old, "\n", -1);
        gboolean inside = FALSE;
        for (gint i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], ULT_MOZ_BEGIN)) { inside = TRUE; continue; }
            if (inside) { if (g_str_has_prefix(lines[i], ULT_MOZ_END)) inside = FALSE; continue; }
            if (lines[i + 1] == NULL && lines[i][0] == '\0') break;   /* trailing newline */
            g_string_append(out, lines[i]);
            g_string_append_c(out, '\n');
        }
        g_strfreev(lines);
    }
    g_string_append_printf(out,
        ULT_MOZ_BEGIN "\n"
        "user_pref(\"widget.gtk.global-menu.enabled\", %s);\n"
        "user_pref(\"widget.gtk.global-menu.wayland.enabled\", %s);\n"
        ULT_MOZ_END "\n", on ? "true" : "false", on ? "true" : "false");

    if (g_strcmp0(old ? old : "", out->str) != 0) {
        g_file_set_contents(userjs, out->str, -1, NULL);
    }
    g_string_free(out, TRUE);
    g_free(old);
    g_free(userjs);
}

static void mozilla_set_global_menu(gboolean on) {
    const gchar *home = g_get_home_dir();
    const gchar *roots[] = {
        ".mozilla/firefox", ".thunderbird", ".librewolf", ".floorp", ".waterfox",
        "snap/firefox/common/.mozilla/firefox", "snap/thunderbird/common/.thunderbird", NULL
    };
    for (gint r = 0; roots[r]; r++) {
        gchar *root = g_build_filename(home, roots[r], NULL);
        GDir *dir = g_dir_open(root, 0, NULL);
        if (dir) {
            const gchar *name;
            while ((name = g_dir_read_name(dir)) != NULL) {
                gchar *prof = g_build_filename(root, name, NULL);
                gchar *prefs = g_build_filename(prof, "prefs.js", NULL);
                if (g_file_test(prefs, G_FILE_TEST_EXISTS)) mozilla_profile_set_global_menu(prof, on);
                g_free(prefs);
                g_free(prof);
            }
            g_dir_close(dir);
        }
        g_free(root);
    }
}

static void apply_appmenu_xsettings(XfconfChannel *xset, gboolean is_mac) {
    if (!xset) return;
    xfconf_channel_set_bool(xset, "/Gtk/ShellShowsMenubar", is_mac);
    xfconf_channel_set_bool(xset, "/Gtk/ShellShowsAppMenu", is_mac);

    gchar *cur = xfconf_channel_get_string(xset, "/Gtk/Modules", "");
    gchar **parts = g_strsplit(cur ? cur : "", ":", -1);
    GString *out = g_string_new("");
    for (gint i = 0; parts && parts[i]; i++) {
        g_strstrip(parts[i]);
        if (!parts[i][0] || g_strcmp0(parts[i], "appmenu-gtk-module") == 0 ||
            g_strcmp0(parts[i], "unity-gtk-module") == 0 ||
            g_strcmp0(parts[i], "ultimate-accels-gtk-module") == 0) continue;
        if (out->len) g_string_append_c(out, ':');
        g_string_append(out, parts[i]);
    }
    if (is_mac) {
        if (out->len) g_string_append_c(out, ':');
        g_string_append(out, "appmenu-gtk-module:ultimate-accels-gtk-module");
    }
    if (g_strcmp0(cur ? cur : "", out->str) != 0) {
        if (out->len) xfconf_channel_set_string(xset, "/Gtk/Modules", out->str);
        else xfconf_channel_reset_property(xset, "/Gtk/Modules", FALSE);
    }
    g_string_free(out, TRUE);
    g_strfreev(parts);
    g_free(cur);

    if (is_mac && !appmenu_module_installed()) notify_appmenu_missing_once();
    mozilla_set_global_menu(is_mac);
}

/* -------------------------------------------------------------------------
 * Window title-bar buttons (xfwm4 + GTK header bars) on the left or right.
 * The user's own values are saved once, so "Don't touch" restores them.
 * ------------------------------------------------------------------------- */
#define XFWM_LAYOUT_LEFT   "CHM|"
#define XFWM_LAYOUT_RIGHT  "O|HMC"
#define GTK_LAYOUT_LEFT    "close,minimize,maximize:"
#define GTK_LAYOUT_RIGHT   "menu:minimize,maximize,close"

/* Where the top-bar buttons really go: a fixed title-bar side (Left / Right) wins,
   so the top bar and the window title bars always match. */
gboolean shell_winck_on_left(UltimateShell *shell) {
    if (!shell) return FALSE;
    return shell->winck_pos == 0;
}

static void titlebar_save_originals(UltimateShell *shell, XfconfChannel *xfwm, XfconfChannel *xset) {
    if (!shell->channel || xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tb_orig_saved", FALSE)) return;
    gchar *a = xfconf_channel_get_string(xfwm, "/general/button_layout", NULL);
    gchar *b = xfconf_channel_get_string(xset, "/Gtk/DecorationLayout", NULL);
    /* "" = was not set (restore by resetting) */
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/tb_orig_xfwm", a ? a : "");
    xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/tb_orig_gtk",  b ? b : "");
    xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tb_orig_saved", TRUE);
    g_free(a); g_free(b);
}

static void titlebar_set(XfconfChannel *ch, const gchar *prop, const gchar *val) {
    gchar *cur = xfconf_channel_get_string(ch, prop, NULL);
    if (!val || !val[0]) {
        if (cur) xfconf_channel_reset_property(ch, prop, FALSE);
    } else if (g_strcmp0(cur, val) != 0) {
        xfconf_channel_set_string(ch, prop, val);
    }
    g_free(cur);
}

static void apply_titlebar_buttons(UltimateShell *shell) {
    if (!shell || !xfconf_init(NULL)) return;
    XfconfChannel *xfwm = xfconf_channel_get("xfwm4");
    XfconfChannel *xset = xfconf_channel_get("xsettings");

    if (shell->titlebar_btn_side == 3) {               /* Don't touch: give back the user's own values */
        if (shell->channel && xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tb_orig_saved", FALSE)) {
            gchar *a = xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/tb_orig_xfwm", "");
            gchar *b = xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/tb_orig_gtk", "");
            titlebar_set(xfwm, "/general/button_layout", a);
            titlebar_set(xset, "/Gtk/DecorationLayout", b);
            g_free(a); g_free(b);
            xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tb_orig_saved", FALSE);
        }
        return;
    }

    titlebar_save_originals(shell, xfwm, xset);
    gboolean left = (shell->mode == LAYOUT_MACOS && !shell->gnome_style && shell->winck_pos == 0);   /* same side as the top-bar buttons */

    titlebar_set(xfwm, "/general/button_layout", left ? XFWM_LAYOUT_LEFT : XFWM_LAYOUT_RIGHT);
    titlebar_set(xset, "/Gtk/DecorationLayout",  left ? GTK_LAYOUT_LEFT  : GTK_LAYOUT_RIGHT);
}

static void on_titlebar_follow_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->titlebar_btn_side = gtk_toggle_button_get_active(b) ? 0 : 3;
    shell_apply_layout(shell, shell->mode);   /* top-bar buttons follow too */
    save_settings_to_xfconf(shell);
}

/* Move a panel widget into box at position pos (-1 = last), keeping it alive. */
static void shell_place(GtkWidget *w, GtkWidget *box, gint pos, gboolean expand, guint pad) {
    if (!w || !box) return;
    GtkWidget *p = gtk_widget_get_parent(w);
    g_object_ref(w);
    if (p && p != box) { gtk_container_remove(GTK_CONTAINER(p), w); p = NULL; }
    if (!p) gtk_box_pack_start(GTK_BOX(box), w, expand, FALSE, pad);
    else    gtk_box_set_child_packing(GTK_BOX(box), w, expand, FALSE, pad, GTK_PACK_START);
    gtk_box_reorder_child(GTK_BOX(box), w, pos);
    g_object_unref(w);
}

void shell_apply_layout(UltimateShell *shell, ShellLayoutMode new_mode) {
    if (!shell) return;
    shell->mode = new_mode;
    gboolean is_mac = (new_mode == LAYOUT_MACOS);
    gboolean gnome = is_mac && shell->gnome_style;   /* top bar + dock, GNOME arrangement */

    sync_xfce_panel_edge_and_size(shell, is_mac);
    global_menu_set_enabled(is_mac && !gnome);
    windowck_apply_settings(shell);

    if (xfconf_init(NULL)) {
        apply_appmenu_xsettings(xfconf_channel_get("xsettings"), is_mac && !gnome);
    }
    apply_titlebar_buttons(shell);

    if (shell->taskbar_box) {
        GtkWidget *parent = gtk_widget_get_parent(shell->taskbar_box);
        g_object_ref_sink(shell->taskbar_box);
        if (parent) gtk_container_remove(GTK_CONTAINER(parent), shell->taskbar_box);

        if (is_mac) {
            gtk_box_pack_start(GTK_BOX(shell->dock_inner_box), shell->taskbar_box, FALSE, FALSE, 0);
            gtk_widget_set_halign(shell->taskbar_box, GTK_ALIGN_CENTER);
        } else {
            gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->taskbar_box), GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_margin_top(shell->taskbar_box, 0);
            gtk_widget_set_margin_bottom(shell->taskbar_box, 0);
            gtk_widget_set_margin_start(shell->taskbar_box, 0);
            gtk_widget_set_margin_end(shell->taskbar_box, 0);
            gtk_widget_set_valign(shell->taskbar_box, GTK_ALIGN_FILL);
            gtk_widget_set_vexpand(shell->taskbar_box, TRUE);
            /* Panel layout decides where the taskbar goes (Start / indicator are placed below) */
            shell->tb_align = (shell->win_layout == WIN_LAYOUT_CLASSIC) ? TASKBAR_ALIGN_LEFT : TASKBAR_ALIGN_CENTER;
            if (shell->win_layout == WIN_LAYOUT_CLASSIC) {
                gtk_box_pack_start(GTK_BOX(shell->left_box), shell->taskbar_box, FALSE, FALSE, 0);
                gtk_widget_set_halign(shell->taskbar_box, GTK_ALIGN_START);
            } else {
                gtk_box_pack_start(GTK_BOX(shell->center_group), shell->taskbar_box, FALSE, FALSE, 0);
                gtk_widget_set_halign(shell->taskbar_box, GTK_ALIGN_CENTER);
            }
        }
        g_object_unref(shell->taskbar_box);
        gtk_widget_show_all(shell->taskbar_box);
    }
    if (shell->left_box)
        gtk_box_set_spacing(GTK_BOX(shell->left_box), is_mac ? 6 : CLAMP(shell->tb_icon_spacing, 0, 24));

    if (shell->windowck_widget) {
        GtkWidget *w_parent = gtk_widget_get_parent(shell->windowck_widget);
        g_object_ref_sink(shell->windowck_widget);
        if (w_parent) gtk_container_remove(GTK_CONTAINER(w_parent), shell->windowck_widget);

        if (!shell_winck_on_left(shell)) {
            /* Last on the right edge of the top bar */
            gtk_box_pack_end(GTK_BOX(shell->right_box), shell->windowck_widget, FALSE, FALSE, 4);
        } else {
            /* Immediately after the Apple Menu (before Global Menu) */
            gtk_box_pack_start(GTK_BOX(shell->mac_top_Controls_box), shell->windowck_widget, FALSE, FALSE, 4);
            gtk_box_reorder_child(GTK_BOX(shell->mac_top_Controls_box), shell->windowck_widget, 0);
        }
        g_object_unref(shell->windowck_widget);
    }

    /* ---- Start button + workspace indicator per layout ---- */
    if (shell->center_group)
        gtk_box_set_spacing(GTK_BOX(shell->center_group), CLAMP(shell->tb_icon_spacing, 0, 24));
    if (gnome) {
        /* [Workspaces  App name]      [Clock]      [▲ Quick Settings] - no start button */
        shell_place(shell->start_btn, shell->left_box, 0, FALSE, 0);
        shell_place(shell->mac_top_Controls_box, shell->left_box, 1, FALSE, 0);
        if (shell->ws_widget) shell_place(shell->ws_widget, shell->left_box, 2, FALSE, 4);
        if (shell->app_title_widget) shell_place(shell->app_title_widget, shell->left_box, 3, FALSE, 2);
        if (shell->clock_widget && shell->center_group) shell_place(shell->clock_widget, shell->center_group, 0, FALSE, 0);
    } else if (is_mac) {
        shell_place(shell->start_btn, shell->left_box, 0, FALSE, 0);              /* Apple menu */
        shell_place(shell->mac_top_Controls_box, shell->left_box, 1, FALSE, 0);
        if (shell->ws_widget) {
            if (shell_winck_on_left(shell))
                shell_place(shell->ws_widget, shell->right_box, -1, FALSE, 4);   /* takes the far-right spot */
            else
                shell_place(shell->ws_widget, shell->mac_top_Controls_box, 0, FALSE, 4); /* between Apple menu and global menu */
        }
    } else {
        gint L = shell->win_layout;
        gboolean start_centered = (L == WIN_LAYOUT_WS_LEFT || L == WIN_LAYOUT_WIN11);
        if (start_centered) shell_place(shell->start_btn, shell->center_group, 0, FALSE, 0);
        else                shell_place(shell->start_btn, shell->left_box, 0, FALSE, 0);
        if (L == WIN_LAYOUT_CLASSIC && shell->taskbar_box)
            gtk_box_reorder_child(GTK_BOX(shell->left_box), shell->taskbar_box, 1);
        if (start_centered && shell->taskbar_box)
            gtk_box_reorder_child(GTK_BOX(shell->center_group), shell->taskbar_box, 1);
        if (shell->ws_widget) {
            if (L == WIN_LAYOUT_WS_LEFT)       shell_place(shell->ws_widget, shell->left_box, 0, FALSE, 2);
            else if (L == WIN_LAYOUT_START_WS) shell_place(shell->ws_widget, shell->left_box, 1, FALSE, 2);
            else                               shell_place(shell->ws_widget, shell->right_box, 0, FALSE, 4); /* before the tray */
        }
    }

    /* outside GNOME mode the clock sits in the right zone, after the tray */
    if (!gnome && shell->clock_widget && shell->right_box &&
        gtk_widget_get_parent(shell->clock_widget) != shell->right_box) {
        gint pos = 1;
        GList *kids = gtk_container_get_children(GTK_CONTAINER(shell->right_box));
        gint k = 0;
        for (GList *l = kids; l; l = l->next, k++) if (l->data == shell->systray_widget) pos = k + 1;
        g_list_free(kids);
        GtkWidget *cp = gtk_widget_get_parent(shell->clock_widget);
        g_object_ref(shell->clock_widget);
        if (cp) gtk_container_remove(GTK_CONTAINER(cp), shell->clock_widget);
        gtk_box_pack_start(GTK_BOX(shell->right_box), shell->clock_widget, FALSE, FALSE, 2);
        gtk_box_reorder_child(GTK_BOX(shell->right_box), shell->clock_widget, pos);
        g_object_unref(shell->clock_widget);
    }
    if (shell->start_btn) {
        gtk_widget_set_no_show_all(shell->start_btn, gnome);   /* the workspace pill / Show Apps open it instead */
        if (gnome) gtk_widget_hide(shell->start_btn); else gtk_widget_show(shell->start_btn);
    }
    if (shell->app_title_widget) {
        if (gnome) app_title_refresh(); else gtk_widget_hide(shell->app_title_widget);
    }

    if (shell->mac_top_Controls_box) {
        if (is_mac && !gnome) {
            gtk_widget_show_all(shell->mac_top_Controls_box);
        } else {
            gtk_widget_hide(shell->mac_top_Controls_box);
        }
    }

    if (shell->windowck_widget) {
        if (is_mac && !gnome && shell->winck_enabled) {
            gtk_widget_show(shell->windowck_widget);
            windowck_apply_settings(shell);
        } else {
            gtk_widget_hide(shell->windowck_widget);
            windowck_apply_settings(shell);
        }
    }

    if (shell->show_desktop_btn) {
        if (!is_mac && shell->show_desktop_enabled) gtk_widget_show(shell->show_desktop_btn);
        else gtk_widget_hide(shell->show_desktop_btn);
    }

    if (shell->notif_widget) {
        if (shell->show_notif_icon) gtk_widget_show(shell->notif_widget);
        else gtk_widget_hide(shell->notif_widget);
    }
    workspace_indicator_apply_settings(shell);   /* after every show_all above */

    if (shell->dock_window) {
        if (is_mac) {
            gtk_widget_show_all(shell->dock_window);
            if (!shell->dock_show_trash && shell->dock_trash_btn) gtk_widget_hide(shell->dock_trash_btn);
            dock_window_apply_settings(shell);
        } else {
            gtk_widget_hide(shell->dock_window);
        }
    }

    start_menu_update_button_appearance(shell);
    taskbar_update_icon_sizes(shell);
    shell_apply_theme(shell);
}

static void on_show_desktop_clicked(GtkButton *btn, UltimateShell *shell) {
    if (!shell || !shell->tracker || !shell->tracker->screen) return;
    gboolean showing = wnck_screen_get_showing_desktop(shell->tracker->screen);
    wnck_screen_toggle_showing_desktop(shell->tracker->screen, !showing);
}

static gboolean on_size_changed(XfcePanelPlugin *plugin, gint size, UltimateShell *shell) {
    gint nrows = xfce_panel_plugin_get_nrows(plugin);
    if (nrows < 1) nrows = 1;
    gtk_widget_set_size_request(GTK_WIDGET(plugin), -1, size / nrows);
    return TRUE;
}

static gboolean on_plugin_size_changed(XfcePanelPlugin *plugin, gint size, UltimateShell *shell) {
    /* only a size the user picked (Panel Preferences) is remembered - never the
       sizes the panel passes through while starting or switching layouts */
    if (shell && !shell->applying_layout_size && g_get_monotonic_time() >= shell->size_guard_until
        && size >= 20 && size <= 96) {
        if (shell->mode == LAYOUT_MACOS) shell->mac_panel_size = size;
        else shell->win_panel_size = size;
        save_settings_to_xfconf(shell);
    }
    return TRUE;
}

/* =========================================================================
 * Complete 6-Tab Preferences Dialog (with SpinButtons, Indentation & Saved Size)
 * ========================================================================= */
static gboolean on_deferred_session_logout(gpointer data) {
    g_spawn_command_line_async(
        "sh -c 'xfconf-query -c xfce4-panel -l >/dev/null 2>&1; sync; sleep 0.2; "
        "xfce4-session-logout --logout --fast 2>/dev/null || xfce4-session-logout --logout'",
        NULL
    );
    return G_SOURCE_REMOVE;
}

/* ---- Effects (General page) ---- */
static void on_compositing_toggled(GtkToggleButton *b, UltimateShell *shell) {
    if (!xfconf_init(NULL)) return;
    xfconf_channel_set_bool(xfconf_channel_get("xfwm4"), "/general/use_compositing", gtk_toggle_button_get_active(b));
}
static void anim_controls_sensitive(GtkWidget *any, UltimateShell *shell) {
    GtkWidget *style_row = g_object_get_data(G_OBJECT(any), "style-row");
    GtkWidget *speed_row = g_object_get_data(G_OBJECT(any), "speed-row");
    if (style_row) gtk_widget_set_sensitive(style_row, shell->anim_mode != 0);
    if (speed_row) gtk_widget_set_sensitive(speed_row, shell->anim_mode != 0);
}
static void on_anim_enabled_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->anim_mode = gtk_toggle_button_get_active(b) ? shell->anim_style : 0;
    shell_apply_animation_css(shell);
    save_settings_to_xfconf(shell);
    anim_controls_sensitive(GTK_WIDGET(b), shell);
}
static void on_anim_style_changed(GtkComboBox *c, UltimateShell *shell) {
    shell->anim_style = gtk_combo_box_get_active(c) == 1 ? 2 : 1;
    if (shell->anim_mode != 0) shell->anim_mode = shell->anim_style;
    shell_apply_animation_css(shell);
    save_settings_to_xfconf(shell);
}
static gchar *on_anim_speed_format(GtkScale *s, gdouble v, gpointer d) {
    gint p = (gint)v;
    return g_strdup(p < 85 ? "Fast" : p > 130 ? "Slow" : "Normal");
}
static void on_anim_speed_changed(GtkRange *r, UltimateShell *shell) {
    /* the slider runs fast → slow left to right */
    shell->anim_speed = CLAMP((gint)gtk_range_get_value(r), 50, 200);
    shell_apply_animation_css(shell);
    save_settings_to_xfconf(shell);
}

static gint shell_layout_combo_index(UltimateShell *shell) {
    if (shell->mode != LAYOUT_MACOS) return 1;
    return shell->gnome_style ? 2 : 0;
}

/* ---- Super (Windows) key opens the start menu ----
   xfce4-keyboard-shortcuts runs our command on Super_L; the command sends the panel a
   plugin event that lands in on_plugin_remote_event. A Super_L binding that was there
   before (e.g. Whisker Menu) is kept and put back when the option is turned off. */
#define SUPER_KEY_PROP "/commands/custom/Super_L"
#define SUPER_KEY_CMD  "xfce4-panel --plugin-event=ultimateshell:popup:bool:true"
static gboolean on_plugin_remote_event(XfcePanelPlugin *plugin, const gchar *name, const GValue *value, UltimateShell *shell) {
    if (g_strcmp0(name, "popup") != 0) return FALSE;
    start_menu_toggle_from_key(shell);
    return TRUE;
}
static void super_key_apply(UltimateShell *shell) {
    if (!shell || !shell->channel || !xfconf_init(NULL)) return;
    XfconfChannel *kb = xfconf_channel_get("xfce4-keyboard-shortcuts");
    if (!kb) return;
    gchar *cur = xfconf_channel_get_string(kb, SUPER_KEY_PROP, NULL);
    gboolean ours = (g_strcmp0(cur, SUPER_KEY_CMD) == 0);
    if (shell->super_key_start) {
        if (!ours) {
            /* remember what was bound before (empty = nothing) */
            xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/super_key_orig", cur ? cur : "");
            xfconf_channel_set_string(kb, SUPER_KEY_PROP, SUPER_KEY_CMD);
        }
    } else if (ours) {
        gchar *orig = xfconf_channel_get_string(shell->channel, "/plugins/ultimateshell/super_key_orig", "");
        if (orig && orig[0]) xfconf_channel_set_string(kb, SUPER_KEY_PROP, orig);
        else xfconf_channel_reset_property(kb, SUPER_KEY_PROP, FALSE);
        g_free(orig);
    }
    g_free(cur);
}
static void on_super_key_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->super_key_start = gtk_toggle_button_get_active(b);
    save_settings_to_xfconf(shell);
    super_key_apply(shell);
}

static void on_layout_combo_changed(GtkComboBox *combo, UltimateShell *shell) {
    gint active = gtk_combo_box_get_active(combo);
    ShellLayoutMode target_mode = (active == 1) ? LAYOUT_WINDOWS : LAYOUT_MACOS;   /* GNOME = top bar + dock too */
    gboolean target_gnome = (active == 2);

    if (active == shell_layout_combo_index(shell)) return;

    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(combo));
    GtkWindow *parent_win = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;

    GtkWidget *dlg = gtk_message_dialog_new(
        parent_win,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Switch Desktop Shell Paradigm & Log Out?"
    );
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dlg),
        "Switching to %s requires logging out of your session so Global Menu modules, "
        "X11 struts, and panel layouts initialize cleanly. "
        "Please make sure any open documents are saved before continuing.",
        target_gnome ? "GNOME Top Bar + Bottom Dock Layout"
        : (target_mode == LAYOUT_MACOS)
            ? "macOS Menu Bar + Bottom Dock Layout"
            : "Windows 11 / 10 / Plasma Taskbar Layout"
    );
    gtk_dialog_add_buttons(
        GTK_DIALOG(dlg),
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Switch & Log Out", GTK_RESPONSE_ACCEPT,
        NULL
    );
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp != GTK_RESPONSE_ACCEPT) {
        g_signal_handlers_block_by_func(combo, G_CALLBACK(on_layout_combo_changed), shell);
        gtk_combo_box_set_active(combo, shell_layout_combo_index(shell));
        g_signal_handlers_unblock_by_func(combo, G_CALLBACK(on_layout_combo_changed), shell);
        return;
    }

    /* Silently persist the new mode, panel edge/size, and xsettings to xfconf
       without rearranging live panel widgets before the session closes */
    shell->mode = target_mode;
    shell->gnome_style = target_gnome;
    save_settings_to_xfconf(shell);
    /* put the panel at the new layout's edge and height now, so it comes back right */
    sync_xfce_panel_edge_and_size(shell, target_mode == LAYOUT_MACOS);
    shell->size_guard_until = G_MAXINT64;           /* nothing more is remembered before logout */

    /* Pre-save GTK xsettings for the next login session without moving the live panel */
    XfconfChannel *xset = xfconf_channel_get("xsettings");
    apply_appmenu_xsettings(xset, target_mode == LAYOUT_MACOS && !target_gnome);
    if (gdk_display_get_default()) {
        gdk_display_sync(gdk_display_get_default());
    }
    g_timeout_add(150, on_deferred_session_logout, NULL);
}






typedef struct {
    UltimateShell *shell;
    gboolean       syncing;

    /* Per-style rows (shown/hidden dynamically based on shell->start_style) */
    GtkWidget *row_list_icon;
    GtkWidget *spin_list_icon;

    GtkWidget *row_grid_icon;
    GtkWidget *spin_grid_icon;

    GtkWidget *row_grid_cols;
    GtkWidget *spin_grid_cols;

    GtkWidget *row_fs_rows;
    GtkWidget *spin_fs_rows;

    GtkWidget *row_label_pt;
    GtkWidget *spin_label_pt;

    GtkWidget *row_search_pos;
    GtkWidget *combo_search_pos;

    GtkWidget *chk_show_desc;
    GtkWidget *chk_win10_tiles;

    /* Category sidebar / filter bar group (Whisker & Fullscreen Launchpad) */
    GtkWidget *box_cat_group;
    GtkWidget *combo_cat_mode;
    GtkWidget *spin_cat_icon;
    GtkWidget *spin_cat_pt;
    GtkWidget *combo_cat_weight;
    GtkWidget *spin_cat_spacing;
    GtkWidget *chk_hover_cat;

    /* Windows 7 Right-Pane Shortcuts group */
    GtkWidget *box_win7_group;

    /* Popup Card Footer & Header group (hidden in Fullscreen Launchpad) */
    GtkWidget *box_footer_group;
} StartTabUI;

static StartTabUI *g_start_tab_ui = NULL;

static void sync_start_tab_controls(StartTabUI *ui) {
    if (!ui || !ui->shell) return;
    ui->syncing = TRUE;

    UltimateShell *shell = ui->shell;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);

    /* Update widget values to match the newly selected style's slot [st] */
    if (GTK_IS_SPIN_BUTTON(ui->spin_list_icon))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_list_icon), shell->start_style_list_icon[st]);
    if (GTK_IS_SPIN_BUTTON(ui->spin_grid_icon))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_grid_icon), shell->start_style_grid_icon[st]);
    if (GTK_IS_SPIN_BUTTON(ui->spin_grid_cols))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_grid_cols), shell->start_style_grid_cols[st]);
    if (GTK_IS_SPIN_BUTTON(ui->spin_fs_rows))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_fs_rows),
                                  shell->start_style == START_STYLE_GNOME ? shell->start_gnome_rows : shell->start_style_fs_rows);
    if (GTK_IS_SPIN_BUTTON(ui->spin_label_pt))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_label_pt), shell->start_style_label_pt[st]);
    if (GTK_IS_COMBO_BOX(ui->combo_search_pos))
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->combo_search_pos), (gint)shell->start_style_search_pos[st]);
    if (GTK_IS_TOGGLE_BUTTON(ui->chk_show_desc))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->chk_show_desc), shell->start_style_show_desc[st]);

    if (GTK_IS_COMBO_BOX(ui->combo_cat_mode))
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->combo_cat_mode), (gint)shell->start_style_cat_mode[st]);
    if (GTK_IS_SPIN_BUTTON(ui->spin_cat_icon))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cat_icon), shell->start_style_cat_icon[st]);
    if (GTK_IS_SPIN_BUTTON(ui->spin_cat_pt))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cat_pt), shell->start_style_cat_pt[st]);
    if (GTK_IS_COMBO_BOX(ui->combo_cat_weight)) {
        gint w = shell->start_style_cat_weight[st];
        gint idx = (w >= 700) ? 2 : ((w >= 500) ? 1 : 0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->combo_cat_weight), idx);
    }
    if (GTK_IS_SPIN_BUTTON(ui->spin_cat_spacing))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->spin_cat_spacing), shell->start_style_cat_spacing[st]);
    if (GTK_IS_TOGGLE_BUTTON(ui->chk_hover_cat))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->chk_hover_cat), shell->start_style_hover_cat[st]);

    /* Determine which controls apply to the selected StartMenuStyle:
     * 0 = START_STYLE_DEFAULT    (List only + Search Pos + Desc)
     * 1 = START_STYLE_WIN7       (List + Win7 Right-Pane Shortcuts + Desc)
     * 2 = START_STYLE_WIN11      (Pinned Grid + All Apps List + Search Pos + Desc)
     * 3 = START_STYLE_WHISKER    (List + Category Sidebar + Search Pos + Desc)
     * 4 = START_STYLE_FULLSCREEN (Grid + FS Rows + Category Filter Bar)
     */
    gboolean has_list    = (st == START_STYLE_DEFAULT || st == START_STYLE_WIN7 || st == START_STYLE_WIN11 || st == START_STYLE_WHISKER || st == START_STYLE_WIN10);
    gboolean has_grid    = (st == START_STYLE_WIN11   || st == START_STYLE_FULLSCREEN || st == START_STYLE_GNOME || st == START_STYLE_WIN10);
    gboolean has_fs_rows = (st == START_STYLE_FULLSCREEN || st == START_STYLE_GNOME);
    gboolean has_search  = (st == START_STYLE_DEFAULT || st == START_STYLE_WIN11 || st == START_STYLE_WHISKER || st == START_STYLE_WIN10);
    gboolean has_desc    = has_list;
    gboolean has_cats    = (st == START_STYLE_WIN7 || st == START_STYLE_WHISKER || st == START_STYLE_FULLSCREEN);
    gboolean has_win7    = (st == START_STYLE_WIN7);
    gboolean has_footer  = (st != START_STYLE_FULLSCREEN && st != START_STYLE_GNOME);

    if (ui->row_list_icon)  gtk_widget_set_visible(ui->row_list_icon,  has_list);
    if (ui->row_grid_icon)  gtk_widget_set_visible(ui->row_grid_icon,  has_grid && st != START_STYLE_WIN10);
    if (ui->row_grid_cols)  gtk_widget_set_visible(ui->row_grid_cols,  has_grid);
    if (ui->row_fs_rows)    gtk_widget_set_visible(ui->row_fs_rows,    has_fs_rows);
    if (ui->row_label_pt)   gtk_widget_set_visible(ui->row_label_pt,   TRUE);
    if (ui->row_search_pos) gtk_widget_set_visible(ui->row_search_pos, has_search);
    if (ui->chk_show_desc)  gtk_widget_set_visible(ui->chk_show_desc,  has_desc);
    if (ui->chk_win10_tiles) gtk_widget_set_visible(ui->chk_win10_tiles, st == START_STYLE_WIN10);
    if (ui->box_cat_group)  gtk_widget_set_visible(ui->box_cat_group,  has_cats);
    if (ui->box_win7_group) gtk_widget_set_visible(ui->box_win7_group, has_win7);
    if (ui->box_footer_group) gtk_widget_set_visible(ui->box_footer_group, has_footer);

    ui->syncing = FALSE;
}

/* one line under the style list: what the short name means */
static const gchar *start_style_blurb(gint st) {
    static const gchar *b[START_STYLE_COUNT] = {
        "A single scrolling list of apps with search.",
        "Two columns like Windows 7: apps and categories left, places and power right.",
        "Centred like Windows 11: pinned apps grid, All apps list and Recommended.",
        "Like Whisker Menu: categories down the side, their apps next to them.",
        "Full screen like macOS: pages of app icons with category pills.",
        "Like Windows 10: A–Z list on the left, resizable pinned tiles on the right.",
        "Full screen like GNOME: workspace overview on top, pages of apps below.",
    };
    return b[CLAMP(st, 0, START_STYLE_COUNT - 1)];
}

static void on_start_style_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->start_style = (StartMenuStyle)gtk_combo_box_get_active(combo);
    GtkWidget *blurb = g_object_get_data(G_OBJECT(combo), "style-blurb");
    if (GTK_IS_LABEL(blurb)) gtk_label_set_text(GTK_LABEL(blurb), start_style_blurb(shell->start_style));
    if (g_start_tab_ui) sync_start_tab_controls(g_start_tab_ui);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_btn_mode_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->start_btn_mode = (StartButtonMode)gtk_combo_box_get_active(combo);
    start_menu_update_button_appearance(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_btn_label_changed(GtkEditable *ed, UltimateShell *shell) {
    g_free(shell->start_btn_label);
    shell->start_btn_label = g_strdup(gtk_entry_get_text(GTK_ENTRY(ed)));
    start_menu_update_button_appearance(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_opacity_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->start_opacity = gtk_spin_button_get_value_as_int(spin);
    shell_apply_theme(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_list_icon_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_list_icon[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_grid_icon_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_grid_icon[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_grid_cols_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_grid_cols[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_fs_rows_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    if (shell->start_style == START_STYLE_GNOME) shell->start_gnome_rows = gtk_spin_button_get_value_as_int(spin);
    else shell->start_style_fs_rows = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_label_pt_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_label_pt[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_search_pos_changed(GtkComboBox *combo, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_search_pos[st] = (StartSearchPosMode)gtk_combo_box_get_active(combo);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_show_desc_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_show_desc[st] = gtk_toggle_button_get_active(btn);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_cat_mode_changed(GtkComboBox *combo, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_cat_mode[st] = (StartCatDisplayMode)gtk_combo_box_get_active(combo);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_cat_icon_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_cat_icon[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_cat_pt_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_cat_pt[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_cat_weight_changed(GtkComboBox *combo, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    gint idx = gtk_combo_box_get_active(combo);
    shell->start_style_cat_weight[st] = (idx == 2) ? 700 : ((idx == 1) ? 600 : 400);
    shell_apply_theme(shell);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_cat_spacing_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_cat_spacing[st] = gtk_spin_button_get_value_as_int(spin);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_hover_cat_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    if (g_start_tab_ui && g_start_tab_ui->syncing) return;
    gint st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
    shell->start_style_hover_cat[st] = gtk_toggle_button_get_active(btn);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_win7_places_weight_changed(GtkComboBox *combo, UltimateShell *shell) {
    gint idx = gtk_combo_box_get_active(combo);
    shell->win7_places_weight = (idx == 2) ? 700 : ((idx == 1) ? 600 : 400);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_win7_shortcut_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    const gchar *id = g_object_get_data(G_OBJECT(btn), "win7-id");
    gboolean act = gtk_toggle_button_get_active(btn);
    if (g_strcmp0(id, "pic") == 0)       shell->win7_show_pictures = act;
    else if (g_strcmp0(id, "mus") == 0)  shell->win7_show_music = act;
    else if (g_strcmp0(id, "vid") == 0)  shell->win7_show_videos = act;
    else if (g_strcmp0(id, "trsh") == 0) shell->win7_show_trash = act;
    else if (g_strcmp0(id, "term") == 0) shell->win7_show_terminal = act;
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_footer_btn_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    const gchar *id = g_object_get_data(G_OBJECT(btn), "pwr-id");
    gboolean act = gtk_toggle_button_get_active(btn);
    if (g_strcmp0(id, "lock") == 0)      shell->start_show_lock = act;
    else if (g_strcmp0(id, "out") == 0)  shell->start_show_logout = act;
    else if (g_strcmp0(id, "reb") == 0)  shell->start_show_restart = act;
    else if (g_strcmp0(id, "shut") == 0) shell->start_show_shutdown = act;
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_user_hdr_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->start_show_user_header = gtk_toggle_button_get_active(btn);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_date_fmt_changed(GtkComboBox *c, UltimateShell *shell) {
    shell->clock_date_fmt = CLAMP(gtk_combo_box_get_active(c), 0, 7);
    GtkWidget *ce = g_object_get_data(G_OBJECT(c), "custom-entry");
    if (ce) gtk_widget_set_sensitive(ce, shell->clock_date_fmt == 7);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}
static void on_clock_date_custom_changed(GtkEditable *e, UltimateShell *shell) {
    g_free(shell->clock_date_custom);
    shell->clock_date_custom = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    if (shell->clock_date_fmt == 7) clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}
static void on_clock_spacing_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->clock_line_spacing = gtk_spin_button_get_value_as_int(s);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}
static void on_clock_bool_toggled(GtkToggleButton *b, UltimateShell *shell) {
    const gchar *p = g_object_get_data(G_OBJECT(b), "clk-prop");
    gboolean v = gtk_toggle_button_get_active(b);
    if (g_strcmp0(p, "day") == 0) shell->clock_mac_show_day = v;
    else if (g_strcmp0(p, "date") == 0) shell->clock_mac_show_date = v;
    else if (g_strcmp0(p, "weeks") == 0) shell->cal_week_numbers = v;
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_generic_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->start_generic_names = gtk_toggle_button_get_active(btn);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_recommended_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->start_show_recommended = gtk_toggle_button_get_active(btn);
    GtkWidget *files = g_object_get_data(G_OBJECT(btn), "files-chk");
    if (files) gtk_widget_set_sensitive(files, shell->start_show_recommended);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}
static void on_win10_tiles_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->win10_show_tiles = gtk_toggle_button_get_active(b);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_whisker_grid_changed(GtkComboBox *c, UltimateShell *shell) {
    shell->start_whisker_grid = (gtk_combo_box_get_active(c) == 1);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_rec_files_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->start_recommend_files = gtk_toggle_button_get_active(btn);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}
static void on_start_clear_history_clicked(GtkButton *b, UltimateShell *shell) {
    start_menu_clear_usage_history();
    start_menu_rebuild_style(shell);
}

static void on_start_hover_fx_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->start_hover_effect = gtk_toggle_button_get_active(btn);
    start_menu_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static GtkWidget *create_tracked_spin_row(GtkWidget *parent_vbox, const gchar *label_text,
                                          gint min_val, gint max_val, gint step_val, gint cur_val,
                                          GCallback cb, UltimateShell *shell, GtkWidget **out_spin) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);

    GtkWidget *spin = gtk_spin_button_new_with_range(min_val, max_val, step_val);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), CLAMP(cur_val, min_val, max_val));
    gtk_widget_set_size_request(spin, 90, -1);
    g_signal_connect(spin, "value-changed", cb, shell);
    gtk_box_pack_end(GTK_BOX(hbox), spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(parent_vbox), hbox, FALSE, FALSE, 0);
    if (out_spin) *out_spin = spin;
    return hbox;
}

static void on_win_layout_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->win_layout = CLAMP(gtk_combo_box_get_active(combo), 0, 3);
    shell_apply_layout(shell, shell->mode);
    taskbar_update_icon_sizes(shell);
    save_settings_to_xfconf(shell);
}

/* ---- Workspaces page ---- */
static void on_ws_enabled_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ws_enabled = gtk_toggle_button_get_active(b);
    workspace_indicator_apply_settings(shell); save_settings_to_xfconf(shell);
}
static void on_ws_style_changed(GtkComboBox *c, UltimateShell *shell) {
    shell->ws_style = CLAMP(gtk_combo_box_get_active(c), 0, 2);
    workspace_indicator_apply_settings(shell); save_settings_to_xfconf(shell);
}
static void on_ws_size_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->ws_size = gtk_spin_button_get_value_as_int(s);
    workspace_indicator_apply_settings(shell); save_settings_to_xfconf(shell);
}
static void on_ws_count_spin(GtkSpinButton *s, UltimateShell *shell) {
    workspace_indicator_set_count(gtk_spin_button_get_value_as_int(s));
}
static void on_ws_scroll_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ws_scroll = gtk_toggle_button_get_active(b); save_settings_to_xfconf(shell);
}
static void on_ws_wrap_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ws_wrap = gtk_toggle_button_get_active(b); save_settings_to_xfconf(shell);
}
static void on_ws_single_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ws_hide_single = gtk_toggle_button_get_active(b);
    workspace_indicator_apply_settings(shell); save_settings_to_xfconf(shell);
}
static void on_ws_names_clicked(GtkButton *b, gpointer d) {
    g_spawn_command_line_async("xfwm4-workspace-settings", NULL);
}

void shell_indicator_rgba(UltimateShell *shell, GdkRGBA *out) {
    if (shell && shell->ind_color && shell->ind_color[0] && gdk_rgba_parse(out, shell->ind_color)) return;
    ShellPalette pl;
    shell_palette_get(shell, &pl);
    *out = pl.accent;                       /* default: the palette accent */
}

void shell_indicator_colors(UltimateShell *shell, GdkRGBA *active, GdkRGBA *inactive, GdkRGBA *urgent) {
    GdkRGBA a;
    shell_indicator_rgba(shell, &a);
    if (active) *active = a;
    if (inactive) {
        if (!(shell && shell->ind_inactive_color && shell->ind_inactive_color[0] &&
              gdk_rgba_parse(inactive, shell->ind_inactive_color))) *inactive = a;
    }
    if (urgent) {
        if (!(shell && shell->ind_urgent_color && shell->ind_urgent_color[0] &&
              gdk_rgba_parse(urgent, shell->ind_urgent_color))) gdk_rgba_parse(urgent, "#FF9500");
    }
}

static void indicators_redraw(UltimateShell *shell) {
    taskbar_update_icon_sizes(shell);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
    if (shell->dock_window) gtk_widget_queue_draw(shell->dock_window);
    save_settings_to_xfconf(shell);
}

static void on_dock_ind_style_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->dock_ind_style = gtk_combo_box_get_active(combo);
    indicators_redraw(shell);
}

static void on_ind_urgent_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->ind_urgent_pulse = gtk_toggle_button_get_active(btn);
    indicators_redraw(shell);
}

/* Indicator colour rows: "field" points at the gchar* in UltimateShell, "" = default */
static void on_ind_color_set(GtkColorButton *btn, UltimateShell *shell) {
    gchar **field = g_object_get_data(G_OBJECT(btn), "field");
    if (!field) return;
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    g_free(*field);
    *field = (c.alpha < 0.999) ? gdk_rgba_to_string(&c) : rgba_to_hex(&c);
    GtkWidget *chk = g_object_get_data(G_OBJECT(btn), "default-chk");
    if (GTK_IS_TOGGLE_BUTTON(chk)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), FALSE);
    indicators_redraw(shell);
}

static void on_ind_default_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    GtkWidget *cb = g_object_get_data(G_OBJECT(btn), "color-btn");
    gchar **field = cb ? g_object_get_data(G_OBJECT(cb), "field") : NULL;
    if (!field) return;
    if (gtk_toggle_button_get_active(btn)) {
        g_free(*field);
        *field = g_strdup("");
    } else if (!*field || !(*field)[0]) {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cb), &c);
        g_free(*field);
        *field = rgba_to_hex(&c);
    }
    gtk_widget_set_sensitive(cb, !gtk_toggle_button_get_active(btn));
    indicators_redraw(shell);
}

static void add_ind_color_row(GtkWidget *box, const gchar *label, const gchar *def_label,
                              gchar **field, const GdkRGBA *cur, UltimateShell *shell) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_size_request(lbl, 150, -1);
    GtkWidget *chk = gtk_check_button_new_with_label(def_label);
    GtkWidget *cb = gtk_color_button_new_with_rgba(cur);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(cb), TRUE);
    gboolean is_def = !(*field && (*field)[0]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), is_def);
    gtk_widget_set_sensitive(cb, !is_def);
    g_object_set_data(G_OBJECT(cb), "field", field);
    g_object_set_data(G_OBJECT(cb), "default-chk", chk);
    g_object_set_data(G_OBJECT(chk), "color-btn", cb);
    g_signal_connect(chk, "toggled", G_CALLBACK(on_ind_default_toggled), shell);
    g_signal_connect(cb, "color-set", G_CALLBACK(on_ind_color_set), shell);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), chk, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), cb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}

static void on_ind_style_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->ind_style = (TaskbarIndicatorStyle)gtk_combo_box_get_active(combo);
    taskbar_update_icon_sizes(shell);
    save_settings_to_xfconf(shell);
}

static void on_inactive_ind_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tb_show_inactive_ind = gtk_toggle_button_get_active(btn);
    taskbar_update_icon_sizes(shell);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
    save_settings_to_xfconf(shell);
}

static void on_thumbnails_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->show_thumbnails = gtk_toggle_button_get_active(btn);
    save_settings_to_xfconf(shell);
}

static void on_tb_mid_click_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->tb_middle_click_close = (gtk_combo_box_get_active(combo) == 1);
    save_settings_to_xfconf(shell);
}

static void on_tb_icon_size_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->tb_icon_size = gtk_spin_button_get_value_as_int(spin);
    taskbar_update_icon_sizes(shell);
    save_settings_to_xfconf(shell);
}

static void on_tb_spacing_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->tb_icon_spacing = gtk_spin_button_get_value_as_int(spin);
    taskbar_update_icon_sizes(shell);
    save_settings_to_xfconf(shell);
}

static void on_tb_padding_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->tb_icon_padding = gtk_spin_button_get_value_as_int(spin);
    taskbar_update_icon_sizes(shell);
    save_settings_to_xfconf(shell);
}

static void on_winck_size_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->winck_btn_size = gtk_spin_button_get_value_as_int(spin);
    windowck_apply_settings(shell);
    shell_apply_theme(shell);
    save_settings_to_xfconf(shell);
}

static void on_winck_pad_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->winck_btn_pad = gtk_spin_button_get_value_as_int(spin);
    windowck_apply_settings(shell);
    shell_apply_theme(shell);
    save_settings_to_xfconf(shell);
}

/* Dock style: the Ubuntu dock fixes position, full height and zoom; their controls are locked */
static void dock_style_sync_controls(GtkWidget *style_combo, UltimateShell *shell) {
    gboolean ubu = (shell->dock_style == 1);
    const gchar *keys[] = { "w-pos", "w-mag", "w-full", "w-fwl", "w-zoom", "w-edge", "w-radius" };
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        GtkWidget *w = g_object_get_data(G_OBJECT(style_combo), keys[i]);
        if (GTK_IS_WIDGET(w)) gtk_widget_set_sensitive(w, !ubu);
    }
    GtkWidget *pos = g_object_get_data(G_OBJECT(style_combo), "w-pos");
    GtkWidget *mag = g_object_get_data(G_OBJECT(style_combo), "w-mag");
    GtkWidget *full = g_object_get_data(G_OBJECT(style_combo), "w-full");
    if (GTK_IS_COMBO_BOX(pos)) gtk_combo_box_set_active(GTK_COMBO_BOX(pos), (gint)shell->dock_pos);
    if (GTK_IS_TOGGLE_BUTTON(mag)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mag), shell->dock_magnify);
    if (GTK_IS_TOGGLE_BUTTON(full)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(full), shell->dock_full_width);
}
static void on_dock_style_changed(GtkComboBox *combo, UltimateShell *shell) {
    gint st = gtk_combo_box_get_active(combo) == 1 ? 1 : 0;
    if (st == shell->dock_style) return;
    shell->dock_style = st;
    if (st == 1) { shell->dock_pos = DOCK_POS_LEFT; shell->dock_full_width = TRUE; shell->dock_magnify = FALSE; }
    else         { shell->dock_pos = DOCK_POS_BOTTOM; shell->dock_full_width = FALSE; shell->dock_magnify = TRUE; }
    dock_style_sync_controls(GTK_WIDGET(combo), shell);   /* their handlers apply + save too */
    taskbar_update_icon_sizes(shell);                      /* buttons re-laid for the new style */
    dock_window_apply_settings(shell);
    shell_apply_theme(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_pos_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->dock_pos = (DockPositionMode)gtk_combo_box_get_active(combo);
    if (shell->dock_style == 1 && shell->dock_pos != DOCK_POS_LEFT) {   /* the Ubuntu dock lives on the left */
        shell->dock_pos = DOCK_POS_LEFT;
        g_signal_handlers_block_by_func(combo, G_CALLBACK(on_dock_pos_changed), shell);
        gtk_combo_box_set_active(combo, DOCK_POS_LEFT);
        g_signal_handlers_unblock_by_func(combo, G_CALLBACK(on_dock_pos_changed), shell);
    }
    taskbar_update_icon_sizes(shell);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_hide_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->dock_autohide = (DockAutohideMode)gtk_combo_box_get_active(combo);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_tooltips_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->dock_show_tooltips = gtk_toggle_button_get_active(btn);
    save_settings_to_xfconf(shell);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
}

static void on_dock_hide_delay_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_hide_delay_sec = gtk_spin_button_get_value_as_int(spin);
    save_settings_to_xfconf(shell);
}

static void on_dock_size_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_icon_size = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_opacity_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_opacity = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_border_w_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_border_w = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_border_color_set(GtkColorButton *btn, UltimateShell *shell) {
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    g_free(shell->dock_border_color);
    shell->dock_border_color = rgba_to_hex(&c);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_border_opacity_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_border_opacity = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_radius_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_radius_val = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_pad_h_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_pad_h = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_pad_v_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_pad_v = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_zoom_scale_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_zoom_scale = gtk_spin_button_get_value_as_int(spin);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
    save_settings_to_xfconf(shell);
}

static void on_dock_edge_dist_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->dock_edge_dist = gtk_spin_button_get_value_as_int(spin);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_icon_shadow_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->dock_icon_shadow = gtk_toggle_button_get_active(btn);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
    if (shell->dock_window) gtk_widget_queue_draw(shell->dock_window);   /* shadow on/off at once */
}

static void on_dock_mag_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->dock_magnify = gtk_toggle_button_get_active(btn);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_fw_layout_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->dock_full_width_layout = gtk_combo_box_get_active(combo);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_full_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->dock_full_width = gtk_toggle_button_get_active(btn);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_dock_trash_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->dock_show_trash = gtk_toggle_button_get_active(btn);
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

/* Clock & Calendar Callbacks */
static void on_cal_style_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->cal_style = (CalendarStyle)gtk_combo_box_get_active(combo);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_fmt_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->clock_format = (ClockFormatMode)gtk_combo_box_get_active(combo);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_24h_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->clock_24h = gtk_toggle_button_get_active(btn);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_sec_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->clock_show_seconds = gtk_toggle_button_get_active(btn);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_time_pt_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->clock_time_pt = gtk_spin_button_get_value_as_int(spin);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_date_pt_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->clock_date_pt = gtk_spin_button_get_value_as_int(spin);
    clock_widget_rebuild_style(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_time_weight_changed(GtkComboBox *combo, UltimateShell *shell) {
    gint idx = gtk_combo_box_get_active(combo);
    shell->clock_time_weight = (idx == 2) ? 700 : ((idx == 1) ? 600 : 400);
    clock_widget_rebuild_style(shell);
    shell_apply_theme(shell);
    save_settings_to_xfconf(shell);
}

static void on_clock_date_weight_changed(GtkComboBox *combo, UltimateShell *shell) {
    gint idx = gtk_combo_box_get_active(combo);
    shell->clock_date_weight = (idx == 2) ? 700 : ((idx == 1) ? 600 : 400);
    clock_widget_rebuild_style(shell);
    shell_apply_theme(shell);
    save_settings_to_xfconf(shell);
}

/* System Tray & Audio Callbacks */
static void on_tray_ibus_modern_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_ibus_modern = gtk_toggle_button_get_active(btn);
    save_settings_to_xfconf(shell);
    systray_ibus_apply_mode(shell, TRUE);
}

static void on_tray_hide_new_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_hide_new_default = gtk_toggle_button_get_active(btn);
    save_settings_to_xfconf(shell);
}

static void on_tray_quick_settings_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->tray_quick_settings = gtk_toggle_button_get_active(b);
    GtkWidget *bt = g_object_get_data(G_OBJECT(b), "qs-bt");
    if (bt) gtk_widget_set_sensitive(bt, shell->tray_quick_settings);
    save_settings_to_xfconf(shell);
    systray_quick_settings_changed(shell);
}

static void on_tray_menu_primary_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_menu_is_primary = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_symbolic_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_request_symbolic = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_show_all_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_show_all = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_net_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_enable_network = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_aud_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_enable_audio = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_bat_always_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    if (shell->channel) {
        xfconf_channel_set_bool(shell->channel, "/plugins/ultimateshell/tray_battery_always_show",
                                gtk_toggle_button_get_active(btn));
    }
    systray_apply_settings(shell);
}

static void on_tray_bat_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_enable_battery = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_bt_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_enable_bluetooth = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_rem_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_enable_removable = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_clip_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->tray_enable_clipboard = gtk_toggle_button_get_active(btn);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_icon_size_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->tray_icon_size = gtk_spin_button_get_value_as_int(spin);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_tray_spacing_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->tray_icon_spacing = gtk_spin_button_get_value_as_int(spin);
    systray_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

G_GNUC_UNUSED static void on_audio_step_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->audio_vol_step = gtk_spin_button_get_value_as_int(spin);
    save_settings_to_xfconf(shell);
}

static void on_winck_enabled_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->winck_enabled = gtk_toggle_button_get_active(btn);
    shell_apply_layout(shell, shell->mode);
    save_settings_to_xfconf(shell);
}

static void on_winck_pos_changed(GtkComboBox *combo, UltimateShell *shell) {
    shell->winck_pos = gtk_combo_box_get_active(combo);
    shell_apply_layout(shell, shell->mode);
    save_settings_to_xfconf(shell);
}

static void on_show_desktop_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->show_desktop_enabled = gtk_toggle_button_get_active(btn);
    shell_apply_layout(shell, shell->mode);
    save_settings_to_xfconf(shell);
}

static void on_show_notif_toggled(GtkToggleButton *btn, UltimateShell *shell) {
    shell->show_notif_icon = gtk_toggle_button_get_active(btn);
    shell_apply_layout(shell, shell->mode);
    save_settings_to_xfconf(shell);
}

/* Remember the window size once resizing has stopped - only those two values
   (saving everything on every resize step flooded xfconf: "Operation was cancelled") */
static guint g_prefs_size_save_id = 0;
static gboolean prefs_size_save_later(gpointer d) {
    UltimateShell *shell = d;
    g_prefs_size_save_id = 0;
    if (shell && shell->channel) {
        xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/prefs_w", shell->prefs_w);
        xfconf_channel_set_int(shell->channel, "/plugins/ultimateshell/prefs_h", shell->prefs_h);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_prefs_configure_event(GtkWidget *dlg, GdkEventConfigure *ev, UltimateShell *shell) {
    if (shell && ev->width >= 360 && ev->height >= 300 &&
        (shell->prefs_w != ev->width || shell->prefs_h != ev->height)) {
        shell->prefs_w = ev->width;
        shell->prefs_h = ev->height;
        if (g_prefs_size_save_id) g_source_remove(g_prefs_size_save_id);
        g_prefs_size_save_id = g_timeout_add(600, prefs_size_save_later, shell);
    }
    return FALSE;
}

static void add_spin_setting_row(GtkWidget *parent_vbox, const gchar *label_text,
                                 gint min_val, gint max_val, gint step_val, gint cur_val,
                                 GCallback cb, UltimateShell *shell) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    /* "Dock Icon Size (24 – 64 px):" -> "Dock Icon Size:" with the range as a tooltip */
    gchar *short_text = g_strdup(label_text);
    gchar *paren = g_strrstr(short_text, " (");
    gchar *range_tip = NULL;
    if (paren && (strstr(paren, "–") || strstr(paren, " - ")) && g_str_has_suffix(paren, "):")) {
        range_tip = g_strndup(paren + 2, strlen(paren) - 4);
        strcpy(paren, ":");
    }
    GtkWidget *lbl = gtk_label_new(short_text);
    if (range_tip) {
        gchar *tip = g_strdup_printf("Range: %s", range_tip);
        gtk_widget_set_tooltip_text(lbl, tip);
        g_free(tip);
    }
    g_free(short_text);
    g_free(range_tip);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);

    GtkWidget *spin = gtk_spin_button_new_with_range(min_val, max_val, step_val);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), CLAMP(cur_val, min_val, max_val));
    gtk_widget_set_size_request(spin, 90, -1);
    g_signal_connect(spin, "value-changed", cb, shell);
    gtk_box_pack_end(GTK_BOX(hbox), spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(parent_vbox), hbox, FALSE, FALSE, 0);
}

/* Paint each settings page's scroll area with the shell background before its
   children draw. Without this the margins around the page content were left
   unpainted and showed as black strips. */
static gboolean on_settings_page_bg_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    ShellPalette pl;
    shell_palette_get(g_anim_shell, &pl);
    cairo_set_source_rgb(cr, pl.bg.red, pl.bg.green, pl.bg.blue);   /* solid, never see-through */
    cairo_paint(cr);
    return FALSE;                                                    /* children draw on top */
}

static GtkWidget *wrap_tab_in_scrolled(GtkWidget *vbox) {
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 22);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    /* The page sits in an event box with its own window, painted with the palette
       background - including the margins around the content, which otherwise were
       left unpainted (transparent, shown black) by the stack underneath. */
    GtkWidget *bgbox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(bgbox), TRUE);
    gtk_widget_set_app_paintable(bgbox, TRUE);
    g_signal_connect(bgbox, "draw", G_CALLBACK(on_settings_page_bg_draw), NULL);
    gtk_container_add(GTK_CONTAINER(bgbox), vbox);
    gtk_container_add(GTK_CONTAINER(scroll), bgbox);
    g_signal_connect(scroll, "draw", G_CALLBACK(on_settings_page_bg_draw), NULL);
    GtkWidget *vp = gtk_bin_get_child(GTK_BIN(scroll));             /* the viewport GTK added */
    if (GTK_IS_VIEWPORT(vp)) {
        gtk_viewport_set_shadow_type(GTK_VIEWPORT(vp), GTK_SHADOW_NONE);
        g_signal_connect(vp, "draw", G_CALLBACK(on_settings_page_bg_draw), NULL);
    }
    return scroll;
}


typedef void (*IconApplyFunc)(UltimateShell *shell, const gchar *new_val);

typedef struct {
    UltimateShell *shell;
    GtkWidget     *entry;
    GtkWidget     *preview_img;
    const gchar   *fallback_icon;
    const gchar   *dialog_title;
    IconApplyFunc  apply_cb;
} IconPickerRowData;

static void refresh_icon_picker_preview(IconPickerRowData *d, const gchar *val) {
    if (!d || !GTK_IS_IMAGE(d->preview_img)) return;
    const gchar *use_str = (val && val[0] != '\0') ? val : d->fallback_icon;
    if (use_str && g_file_test(use_str, G_FILE_TEST_EXISTS)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(use_str, 20, 20, TRUE, NULL);
        if (pb) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(d->preview_img), pb);
            g_object_unref(pb);
            return;
        }
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(d->preview_img),
                                 (use_str && use_str[0] != '\0') ? use_str : "image-x-generic",
                                 GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(d->preview_img), 20);
}

static void on_icon_picker_entry_changed(GtkEditable *ed, IconPickerRowData *d) {
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(ed));
    refresh_icon_picker_preview(d, txt);
    if (d->apply_cb) d->apply_cb(d->shell, txt);
}

/* Try opening native Xfce ExoIconChooserDialog via runtime dlopen (zero compile-time symbol risk & zero lag!) */
typedef GtkWidget* (*ExoChooserNewFunc)(const gchar *title, GtkWindow *parent, const gchar *first_button_text, ...);
typedef gchar*     (*ExoChooserGetIconFunc)(GtkWidget *chooser);
typedef gboolean   (*ExoChooserSetIconFunc)(GtkWidget *chooser, const gchar *icon);

static gboolean try_run_native_exo_icon_chooser(GtkWindow *parent, const gchar *title, const gchar *cur_icon, gchar **out_selected) {
    static gboolean tried_load = FALSE;
    static GModule *exo_mod = NULL;
    static ExoChooserNewFunc fn_new = NULL;
    static ExoChooserGetIconFunc fn_get = NULL;
    static ExoChooserSetIconFunc fn_set = NULL;

    if (!tried_load) {
        tried_load = TRUE;
        const gchar *libs[] = {
            "libexo-2.so.0",
            "libexo-2.so",
            "/usr/lib/x86_64-linux-gnu/libexo-2.so.0",
            "/usr/lib/libexo-2.so.0",
            NULL
        };
        for (int i = 0; libs[i] != NULL; i++) {
            exo_mod = g_module_open(libs[i], G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
            if (exo_mod) break;
        }
        if (exo_mod) {
            g_module_symbol(exo_mod, "exo_icon_chooser_dialog_new", (gpointer *)&fn_new);
            g_module_symbol(exo_mod, "exo_icon_chooser_dialog_get_icon", (gpointer *)&fn_get);
            g_module_symbol(exo_mod, "exo_icon_chooser_dialog_set_icon", (gpointer *)&fn_set);
        }
    }

    if (!fn_new || !fn_get) return FALSE;

    GtkWidget *dlg = fn_new(title ? title : "Select an Icon",
                            parent,
                            "_Cancel", GTK_RESPONSE_CANCEL,
                            "_OK", GTK_RESPONSE_ACCEPT,
                            NULL);
    if (!dlg) return FALSE;

    if (fn_set && cur_icon && cur_icon[0] != '\0') {
        fn_set(dlg, cur_icon);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        *out_selected = fn_get(dlg);
    }
    gtk_widget_destroy(dlg);
    return TRUE;
}

/* Fast Capped Fallback Icon Chooser (Only renders up to 90 matching icons at a time — 0ms lag) */
enum {
    ICON_COL_PIXBUF = 0,
    ICON_COL_NAME,
    ICON_NUM_COLS
};

typedef struct {
    GtkListStore *store;
    GtkWidget    *icon_view;
    GtkWidget    *search_entry;
    GtkWidget    *combo_ctx;
    GtkWidget    *dialog;
    GList        *all_names;
    GtkIconTheme *theme;
    gchar        *chosen_result;
} FastIconChooserUI;

static gint classify_icon_context(const gchar *name) {
    if (!name) return 1;
    if (strstr(name, "distributor-logo") || strstr(name, "start-here") || strstr(name, "xfce") ||
        strstr(name, "debian") || strstr(name, "ubuntu") || strstr(name, "arch") ||
        strstr(name, "fedora") || strstr(name, "linux") || strstr(name, "system") ||
        strstr(name, "folder") || strstr(name, "user-"))
        return 2;
    if (strstr(name, "applications-"))
        return 3;
    if (strstr(name, "edit-") || strstr(name, "document-") || strstr(name, "window-") ||
        strstr(name, "list-") || strstr(name, "media-") || strstr(name, "go-"))
        return 4;
    return 1;
}

static void rebuild_fast_icon_store(FastIconChooserUI *ui) {
    gint sel_ctx = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->combo_ctx));
    const gchar *q = gtk_entry_get_text(GTK_ENTRY(ui->search_entry));
    gchar *ql = (q && q[0] != '\0') ? g_ascii_strdown(q, -1) : NULL;

    g_object_ref(ui->store);
    gtk_icon_view_set_model(GTK_ICON_VIEW(ui->icon_view), NULL);
    gtk_list_store_clear(ui->store);

    gint added = 0;
    const gint MAX_SHOW = 90;

    for (GList *l = ui->all_names; l != NULL && added < MAX_SHOW; l = l->next) {
        const gchar *name = (const gchar *)l->data;
        if (!name || g_str_has_suffix(name, "-symbolic.symbolic")) continue;

        if (sel_ctx > 0 && sel_ctx < 5) {
            if (classify_icon_context(name) != sel_ctx) continue;
        }

        if (ql) {
            gchar *nl = g_ascii_strdown(name, -1);
            gboolean ok = (strstr(nl, ql) != NULL);
            g_free(nl);
            if (!ok) continue;
        }

        GdkPixbuf *pb = gtk_icon_theme_load_icon(ui->theme, name, 48, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
        if (pb) {
            GtkTreeIter iter;
            gtk_list_store_append(ui->store, &iter);
            gtk_list_store_set(ui->store, &iter, ICON_COL_PIXBUF, pb, ICON_COL_NAME, name, -1);
            g_object_unref(pb);
            added++;
        }
    }

    gtk_icon_view_set_model(GTK_ICON_VIEW(ui->icon_view), GTK_TREE_MODEL(ui->store));
    g_object_unref(ui->store);
    g_free(ql);
}

static void on_fast_chooser_filter_changed(GtkWidget *w, FastIconChooserUI *ui) {
    if (GTK_IS_COMBO_BOX(w) && gtk_combo_box_get_active(GTK_COMBO_BOX(w)) == 5) {
        GtkWidget *fc = gtk_file_chooser_dialog_new(
            "Select Image File", GTK_WINDOW(ui->dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Icon & Image Files (*.svg, *.png, *.xpm, *.ico)");
        gtk_file_filter_add_pixbuf_formats(filter);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), filter);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), "/usr/share/pixmaps");
        if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
            gchar *fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
            if (fname) {
                g_free(ui->chosen_result);
                ui->chosen_result = fname;
                gtk_widget_destroy(fc);
                gtk_dialog_response(GTK_DIALOG(ui->dialog), GTK_RESPONSE_ACCEPT);
                return;
            }
        }
        gtk_widget_destroy(fc);
        gtk_combo_box_set_active(GTK_COMBO_BOX(w), 2);
        return;
    }
    rebuild_fast_icon_store(ui);
}

static void on_fast_icon_view_activated(GtkIconView *iv, GtkTreePath *path, FastIconChooserUI *ui) {
    gtk_dialog_response(GTK_DIALOG(ui->dialog), GTK_RESPONSE_ACCEPT);
}

static gint compare_icon_names_priority(gconstpointer a, gconstpointer b) {
    const gchar *sa = (const gchar *)a;
    const gchar *sb = (const gchar *)b;
    gboolean pa = (g_str_has_prefix(sa, "distributor-logo") || g_str_has_prefix(sa, "start-here") || g_str_has_prefix(sa, "applications-"));
    gboolean pb = (g_str_has_prefix(sb, "distributor-logo") || g_str_has_prefix(sb, "start-here") || g_str_has_prefix(sb, "applications-"));
    if (pa && !pb) return -1;
    if (!pa && pb) return 1;
    return g_ascii_strcasecmp(sa, sb);
}

static void on_icon_picker_browse_clicked(GtkButton *btn, IconPickerRowData *d) {
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(btn));
    GtkWindow *parent_win = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;
    const gchar *cur = gtk_entry_get_text(GTK_ENTRY(d->entry));

    gchar *exo_selected = NULL;
    if (try_run_native_exo_icon_chooser(parent_win,
                                        d->dialog_title ? d->dialog_title : "Select an Icon",
                                        (cur && cur[0] != '\0') ? cur : d->fallback_icon,
                                        &exo_selected)) {
        if (exo_selected) {
            gtk_entry_set_text(GTK_ENTRY(d->entry), exo_selected);
            g_free(exo_selected);
        }
        return;
    }

    FastIconChooserUI *ui = g_new0(FastIconChooserUI, 1);
    ui->theme = gtk_icon_theme_get_default();
    ui->dialog = gtk_dialog_new_with_buttons(
        d->dialog_title ? d->dialog_title : "Select an Icon",
        parent_win,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_ACCEPT,
        NULL
    );
    gtk_window_set_default_size(GTK_WINDOW(ui->dialog), 680, 520);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(ui->dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_box_pack_start(GTK_BOX(content), vbox, TRUE, TRUE, 0);

    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(row1), gtk_label_new("Select icon from:"), FALSE, FALSE, 0);
    ui->combo_ctx = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->combo_ctx), "All Icons (Top 90 Matches)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->combo_ctx), "Application Icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->combo_ctx), "Distributor Logos, Places & System Icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->combo_ctx), "Category Icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->combo_ctx), "Action Icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->combo_ctx), "Image Files on Disk (.svg / .png)...");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->combo_ctx), 2);
    gtk_box_pack_start(GTK_BOX(row1), ui->combo_ctx, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row1, FALSE, FALSE, 0);

    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(row2), gtk_label_new("Search icon:"), FALSE, FALSE, 0);
    ui->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->search_entry), "Type to filter (e.g. debian, distributor-logo, start-here, launchpad)...");
    gtk_box_pack_start(GTK_BOX(row2), ui->search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row2, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    ui->store = gtk_list_store_new(ICON_NUM_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    ui->icon_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(ui->store));
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(ui->icon_view), ICON_COL_PIXBUF);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(ui->icon_view), ICON_COL_NAME);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(ui->icon_view), 92);
    gtk_icon_view_set_columns(GTK_ICON_VIEW(ui->icon_view), -1);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(ui->icon_view), 8);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(ui->icon_view), 8);
    gtk_container_add(GTK_CONTAINER(scroll), ui->icon_view);

    ui->all_names = g_list_sort(gtk_icon_theme_list_icons(ui->theme, NULL), compare_icon_names_priority);
    rebuild_fast_icon_store(ui);

    g_signal_connect(ui->search_entry, "search-changed", G_CALLBACK(on_fast_chooser_filter_changed), ui);
    g_signal_connect(ui->combo_ctx, "changed", G_CALLBACK(on_fast_chooser_filter_changed), ui);
    g_signal_connect(ui->icon_view, "item-activated", G_CALLBACK(on_fast_icon_view_activated), ui);

    gtk_widget_show_all(ui->dialog);
    gtk_widget_grab_focus(ui->search_entry);

    if (gtk_dialog_run(GTK_DIALOG(ui->dialog)) == GTK_RESPONSE_ACCEPT) {
        if (ui->chosen_result) {
            gtk_entry_set_text(GTK_ENTRY(d->entry), ui->chosen_result);
        } else {
            GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(ui->icon_view));
            if (sel) {
                GtkTreePath *path = (GtkTreePath *)sel->data;
                GtkTreeIter iter;
                if (gtk_tree_model_get_iter(GTK_TREE_MODEL(ui->store), &iter, path)) {
                    gchar *ico_name = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, ICON_COL_NAME, &ico_name, -1);
                    if (ico_name) {
                        gtk_entry_set_text(GTK_ENTRY(d->entry), ico_name);
                        g_free(ico_name);
                    }
                }
                g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
            }
        }
    }

    g_list_free_full(ui->all_names, g_free);
    g_free(ui->chosen_result);
    g_object_unref(ui->store);
    gtk_widget_destroy(ui->dialog);
    g_free(ui);
}

static void on_icon_picker_reset_clicked(GtkButton *btn, IconPickerRowData *d) {
    gtk_entry_set_text(GTK_ENTRY(d->entry), "");
}

static void apply_win_start_icon_cb(UltimateShell *shell, const gchar *val) {
    g_free(shell->start_btn_icon);
    shell->start_btn_icon = g_strdup(val ? val : "");
    start_menu_update_button_appearance(shell);
    save_settings_to_xfconf(shell);
}

static void apply_mac_apple_icon_cb(UltimateShell *shell, const gchar *val) {
    g_free(shell->mac_apple_icon);
    shell->mac_apple_icon = g_strdup(val ? val : "");
    start_menu_update_button_appearance(shell);
    save_settings_to_xfconf(shell);
}

static void apply_dock_launchpad_icon_cb(UltimateShell *shell, const gchar *val) {
    g_free(shell->dock_launchpad_icon);
    shell->dock_launchpad_icon = g_strdup(val ? val : "");
    dock_window_apply_settings(shell);
    save_settings_to_xfconf(shell);
}

static void on_start_btn_icon_size_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->start_btn_icon_size = gtk_spin_button_get_value_as_int(spin);
    start_menu_update_button_appearance(shell);
    save_settings_to_xfconf(shell);
}

static void on_mac_apple_icon_size_spin(GtkSpinButton *spin, UltimateShell *shell) {
    shell->mac_apple_icon_size = gtk_spin_button_get_value_as_int(spin);
    start_menu_update_button_appearance(shell);
    save_settings_to_xfconf(shell);
}

static GtkWidget *create_icon_picker_row(GtkWidget *parent_vbox,
                                         const gchar *label_text,
                                         const gchar *placeholder,
                                         const gchar *cur_val,
                                         const gchar *fallback_icon,
                                         IconApplyFunc apply_cb,
                                         UltimateShell *shell) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);

    IconPickerRowData *d = g_new0(IconPickerRowData, 1);
    d->shell = shell;
    d->fallback_icon = fallback_icon;
    d->dialog_title = label_text;
    d->apply_cb = apply_cb;
    g_object_set_data_full(G_OBJECT(hbox), "picker-data", d, g_free);

    d->preview_img = gtk_image_new();
    gtk_widget_set_size_request(d->preview_img, 24, 24);

    d->entry = gtk_entry_new();
    gtk_widget_set_size_request(d->entry, 165, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->entry), placeholder);
    gtk_entry_set_text(GTK_ENTRY(d->entry), cur_val ? cur_val : "");
    refresh_icon_picker_preview(d, cur_val);
    g_signal_connect(d->entry, "changed", G_CALLBACK(on_icon_picker_entry_changed), d);

    GtkWidget *browse_btn = gtk_button_new_with_label("Choose Icon...");
    gtk_widget_set_tooltip_text(browse_btn, "Open visual icon chooser");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_icon_picker_browse_clicked), d);

    GtkWidget *reset_btn = gtk_button_new_from_icon_name("edit-clear-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(reset_btn, "Reset to default icon");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_icon_picker_reset_clicked), d);

    gtk_box_pack_end(GTK_BOX(hbox), reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), browse_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), d->entry, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), d->preview_img, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(parent_vbox), hbox, FALSE, FALSE, 0);
    return hbox;
}

static GtkWidget *g_active_prefs_dlg = NULL;

/* =========================================================================
 * Settings window: sidebar pages, per-page reset, export/import, search
 * ========================================================================= */
#define ULT_PROP_ROOT "/plugins/ultimateshell"

enum { PAGE_ANY = -1, PAGE_MAC_ONLY = 0, PAGE_WIN_ONLY = 1 };

static void settings_restart_panel(void) {
    gchar *argv[] = { "xfce4-panel", "-r", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

/* Settings were just written straight into xfconf (theme, import, page reset).
   Load them into memory too before restarting: closing the settings window
   saves everything, and with the old values still in memory that save used to
   overwrite what was just applied (a light theme came back dark). */
static void load_settings_from_xfconf(UltimateShell *shell);
static void settings_sync_memory_then_restart(UltimateShell *shell) {
    load_settings_from_xfconf(shell);
    settings_restart_panel();
}

static void on_restart_panel_clicked(GtkButton *btn, gpointer data) {
    settings_restart_panel();
}

static gboolean settings_confirm(GtkWidget *any, const gchar *title, const gchar *body, const gchar *ok) {
    GtkWidget *top = gtk_widget_get_toplevel(any);
    GtkWidget *d = gtk_message_dialog_new(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", body);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, ok, GTK_RESPONSE_ACCEPT, NULL);
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_ACCEPT;
}

/* ---- Reset this page: reset every key whose name starts with one of the page's prefixes ---- */
static void on_reset_page_clicked(GtkButton *btn, UltimateShell *shell) {
    const gchar *const *prefixes = g_object_get_data(G_OBJECT(btn), "reset-prefixes");
    const gchar *title = g_object_get_data(G_OBJECT(btn), "page-title");
    if (!prefixes || !shell || !shell->channel) return;
    gchar *q = g_strdup_printf("Reset “%s” to defaults?", title ? title : "this page");
    if (!settings_confirm(GTK_WIDGET(btn), q,
            "Only the settings on this page go back to their defaults. The panel restarts to apply them.",
            "_Reset")) { g_free(q); return; }
    g_free(q);

    GHashTable *props = xfconf_channel_get_properties(shell->channel, ULT_PROP_ROOT);
    if (props) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, props);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            const gchar *full = (const gchar *)k;
            const gchar *base = full + strlen(ULT_PROP_ROOT "/");
            if (!g_str_has_prefix(full, ULT_PROP_ROOT "/")) continue;
            for (gint i = 0; prefixes[i]; i++) {
                if (g_str_has_prefix(base, prefixes[i])) {
                    xfconf_channel_reset_property(shell->channel, full, FALSE);
                    break;
                }
            }
        }
        g_hash_table_destroy(props);
    }
    settings_sync_memory_then_restart(shell);
}

/* ---- Export / Import (a simple key file: name=type:value) ---- */
static void on_export_clicked(GtkButton *btn, UltimateShell *shell) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(btn));
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Export Ultimate Shell Settings",
        GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "_Export", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), "ultimate-shell-settings.ini");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT && shell->channel) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        GKeyFile *kf = g_key_file_new();
        GHashTable *props = xfconf_channel_get_properties(shell->channel, ULT_PROP_ROOT);
        if (props) {
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init(&it, props);
            while (g_hash_table_iter_next(&it, &k, &v)) {
                const gchar *full = (const gchar *)k;
                if (!g_str_has_prefix(full, ULT_PROP_ROOT "/")) continue;
                const gchar *base = full + strlen(ULT_PROP_ROOT "/");
                if (g_strcmp0(base, "prefs_w") == 0 || g_strcmp0(base, "prefs_h") == 0) continue;
                GValue *val = (GValue *)v;
                gchar *enc = NULL;
                if (G_VALUE_HOLDS_BOOLEAN(val))     enc = g_strdup_printf("b:%s", g_value_get_boolean(val) ? "true" : "false");
                else if (G_VALUE_HOLDS_INT(val))    enc = g_strdup_printf("i:%d", g_value_get_int(val));
                else if (G_VALUE_HOLDS_UINT(val))   enc = g_strdup_printf("u:%u", g_value_get_uint(val));
                else if (G_VALUE_HOLDS_DOUBLE(val)) { gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
                                                     enc = g_strdup_printf("d:%s", g_ascii_dtostr(buf, sizeof buf, g_value_get_double(val))); }
                else if (G_VALUE_HOLDS_STRING(val)) enc = g_strdup_printf("s:%s", g_value_get_string(val) ? g_value_get_string(val) : "");
                if (enc) { g_key_file_set_string(kf, "ultimateshell", base, enc); g_free(enc); }
            }
            g_hash_table_destroy(props);
        }
        GError *err = NULL;
        if (!g_key_file_save_to_file(kf, path, &err)) {
            GtkWidget *d = gtk_message_dialog_new(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Couldn't export settings");
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", err ? err->message : "");
            gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
            g_clear_error(&err);
        }
        g_key_file_free(kf);
        g_free(path);
    }
    g_object_unref(fc);
}

static void on_import_clicked(GtkButton *btn, UltimateShell *shell) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(btn));
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Import Ultimate Shell Settings",
        GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "_Import", "_Cancel");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT && shell->channel) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
            settings_confirm(GTK_WIDGET(btn), "Import these settings?",
                "Your current Ultimate Shell settings will be replaced by the ones in the file. The panel restarts to apply them.",
                "_Import")) {
            gsize nkeys = 0;
            gchar **keys = g_key_file_get_keys(kf, "ultimateshell", &nkeys, NULL);
            for (gsize i = 0; keys && i < nkeys; i++) {
                if (strchr(keys[i], '/') || strstr(keys[i], "..")) continue;   /* only plain names */
                gchar *enc = g_key_file_get_string(kf, "ultimateshell", keys[i], NULL);
                if (!enc || strlen(enc) < 2 || enc[1] != ':') { g_free(enc); continue; }
                gchar *full = g_strdup_printf(ULT_PROP_ROOT "/%s", keys[i]);
                const gchar *v = enc + 2;
                switch (enc[0]) {
                    case 'b': xfconf_channel_set_bool(shell->channel, full, g_strcmp0(v, "true") == 0); break;
                    case 'i': xfconf_channel_set_int(shell->channel, full, (gint)g_ascii_strtoll(v, NULL, 10)); break;
                    case 'u': xfconf_channel_set_uint(shell->channel, full, (guint)g_ascii_strtoull(v, NULL, 10)); break;
                    case 'd': xfconf_channel_set_double(shell->channel, full, g_ascii_strtod(v, NULL)); break;
                    case 's': xfconf_channel_set_string(shell->channel, full, v); break;
                    default: break;
                }
                g_free(full);
                g_free(enc);
            }
            g_strfreev(keys);
            settings_sync_memory_then_restart(shell);
        }
        g_key_file_free(kf);
        g_free(path);
    }
    g_object_unref(fc);
}


/* =========================================================================
 * Appearance overrides (General tab) + Shell themes
 * ========================================================================= */

/* Extra look settings on top of the base theme CSS: font, corner radius,
   borders, shadow and hover colour. Empty / -1 = leave the default look. */
void shell_apply_appearance_css(UltimateShell *shell) {
    static GtkCssProvider *prov = NULL;
    if (!prov) {
        prov = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
                                                  GTK_STYLE_PROVIDER_PRIORITY_USER + 110);   /* above the global menu's own rules */
    }
    GString *css = g_string_new("");
    const gchar *ALL = "#ultimate-root-box, #ultimate-root-box *, .ultimate-popup-win, .ultimate-popup-win *,"
                       " menu.ultimate-themed-menu, menu.ultimate-themed-menu *, #ultimate-macos-dock *";
    const gchar *POPUPS = ".ultimate-popup-win, menu.ultimate-themed-menu, .ultimate-themed-menu";
    const gchar *BTNS = ".taskbar-item-btn, #ultimate-start-btn, #ultimate-clock-btn, .ultimate-module-btn, .app-grid-tile,"
                        " .taskbar-item-btn:hover, #ultimate-start-btn:hover, #ultimate-clock-btn:hover,"
                        " .ultimate-module-btn:hover, .app-grid-tile:hover, .app-grid-tile.app-tile-hovered";
    const gchar *HOVER = "#ultimate-start-btn:hover, #ultimate-clock-btn:hover, .ultimate-module-btn:hover,"
                         " .taskbar-item-btn:hover, .start-category-btn:hover, .overflow-tray-btn:hover,"
                         " .app-grid-tile:hover, .app-grid-tile.app-tile-hovered, menu.ultimate-themed-menu menuitem:hover";

    /* Readable text on any background: the global menu and the panel buttons used
       fixed white text, which vanished on light themes */
    {
        /* palette names: defined by the GTK theme, or by the shell palette */
        const gchar *FG = "@theme_fg_color", *ACC = "@theme_selected_bg_color", *ACC_FG = "@theme_selected_fg_color";
        g_string_append_printf(css,
            "menubar > menuitem, #ultimate-global-menubar > menuitem,"
            " menubar > menuitem > label, #ultimate-global-menubar > menuitem > label { color: %s; }"
            "menubar > menuitem:hover, #ultimate-global-menubar > menuitem:hover { background-color: %s; }"
            "menubar > menuitem:hover > label, #ultimate-global-menubar > menuitem:hover > label { color: %s; }"
            "menu.ultimate-themed-menu menuitem, .ultimate-themed-menu menuitem,"
            " menu.ultimate-themed-menu menuitem label, .ultimate-themed-menu menuitem label { color: %s; }"
            "menu.ultimate-themed-menu menuitem:hover, .ultimate-themed-menu menuitem:hover { background-color: %s; }"
            /* shell menus hover with the subtle @ult_hover tint, so text keeps the normal colour */
            "menu.ultimate-themed-menu menuitem:hover > label, .ultimate-themed-menu menuitem:hover > label,"
            " menu.ultimate-themed-menu menuitem:hover > arrow, menu.ultimate-themed-menu menuitem:hover > label > accelerator { color: %s; }"
            "menu.ultimate-themed-menu menuitem:disabled > label, .ultimate-themed-menu menuitem:disabled > label { color: alpha(%s, 0.45); }"
            "#ultimate-start-btn:hover, #ultimate-clock-btn:hover, .ultimate-module-btn:hover, .taskbar-item-btn:hover"
            " { background-color: @ult_hover; border-color: transparent; }"
            "#ultimate-start-btn:hover label, #ultimate-clock-btn:hover label, .ultimate-module-btn:hover label,"
            " #ultimate-start-btn:hover image, #ultimate-clock-btn:hover image, .ultimate-module-btn:hover image { color: %s; }"
            /* panel buttons stay flat: a dark GTK theme must not paint dark tiles on a light bar */
            ".taskbar-item-btn, #ultimate-start-btn, #ultimate-clock-btn, .ultimate-module-btn, .overflow-tray-btn"
            " { background-image: none; box-shadow: none; text-shadow: none; -gtk-icon-shadow: none; }"
            ".taskbar-item-btn:not(:hover):not(:checked), #ultimate-start-btn:not(:hover):not(:checked),"
            " #ultimate-clock-btn:not(:hover):not(:checked), .ultimate-module-btn:not(:hover):not(:checked)"
            " { background-color: transparent; border-color: transparent; }"
            /* no theme border / line along the panel edge */
            /* xfce4-panel draws its edge line itself, in the window's border-color: make it invisible */
            ".xfce4-panel, .xfce4-panel.background, XfcePanelWindow, window.xfce4-panel, #ultimate-root-box"
            " { border: none; border-color: transparent; box-shadow: none; outline: none; }",
            FG, ACC, ACC_FG, FG, ACC, FG, FG, FG);
    }

    if (shell->ui_font && shell->ui_font[0]) {
        PangoFontDescription *fd = pango_font_description_from_string(shell->ui_font);
        const gchar *fam = pango_font_description_get_family(fd);
        gint sz = pango_font_description_get_size(fd) / PANGO_SCALE;
        g_string_append_printf(css, "%s {", ALL);
        if (fam && fam[0]) g_string_append_printf(css, " font-family: \"%s\";", fam);
        if (sz > 0) g_string_append_printf(css, " font-size: %dpt;", sz);
        g_string_append(css, " }");
        pango_font_description_free(fd);
    }
    if (shell->ui_radius >= 0) {
        g_string_append_printf(css, "%s { border-radius: %dpx; }", POPUPS, shell->ui_radius);
        g_string_append_printf(css, "%s { border-radius: %dpx; }", BTNS, MIN(shell->ui_radius, 14));
    }
    if (shell->ui_border_w >= 0 || (shell->ui_border_color && shell->ui_border_color[0])) {
        gint bw = shell->ui_border_w >= 0 ? shell->ui_border_w : 1;
        const gchar *bc = "@ult_border";
        g_string_append_printf(css, "%s { border: %dpx solid %s; }", POPUPS, bw, bc);
    }
    /* the dock shows hover by magnifying, never with a button background */
    g_string_append(css,
        "#ultimate-macos-dock button, #ultimate-macos-dock button:hover, #ultimate-macos-dock button:active,"
        " #ultimate-macos-dock button:checked, #ultimate-macos-dock .taskbar-item-btn:hover"
        " { background: transparent; background-image: none; border-color: transparent; box-shadow: none; }");
    if (!shell->ui_popup_shadow)
        g_string_append_printf(css, "%s { box-shadow: none; }", POPUPS);
    g_string_append_printf(css, "%s { background-color: @ult_hover; background-image: none; border-color: transparent; }", HOVER);

    /* Shell palette (Light / Dark / Custom): GTK draws entries, checks, sliders and
       tooltips with the GTK theme's own colours, so give them the palette inside the
       shell's popups, menus and settings window. Follow-GTK mode needs none of this. */
    if (shell->color_mode != COLOR_FOLLOW_GTK) {
        const gchar *SC[] = { ".ultimate-popup-win", "menu.ultimate-themed-menu", ".ultimate-settings-win", NULL };
        for (gint i = 0; SC[i]; i++) {
            const gchar *S = SC[i];
            g_string_append_printf(css,
                "%1$s entry, %1$s spinbutton, %1$s spinbutton entry {"
                " background-color: @theme_base_color; background-image: none; color: @theme_fg_color;"
                " border-color: @ult_border; caret-color: @theme_fg_color; box-shadow: none; text-shadow: none; }"
                "%1$s entry:focus, %1$s spinbutton:focus { border-color: @theme_selected_bg_color; }"
                "%1$s entry placeholder, %1$s entry image { color: alpha(@theme_fg_color, 0.55); }"
                "%1$s selection, %1$s entry selection { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; }"
                "%1$s button:not(.flat) { background-color: alpha(@theme_fg_color, 0.06); background-image: none;"
                " color: @theme_fg_color; border-color: @ult_border; box-shadow: none; text-shadow: none; -gtk-icon-shadow: none; }"
                "%1$s button:hover { background-color: @ult_hover; background-image: none; }"
                "%1$s button:checked:not(.flat), %1$s button:active:not(.flat) { background-color: alpha(@theme_fg_color, 0.16); }"
                "%1$s button label, %1$s button image { color: @theme_fg_color; }"
                "%1$s check, %1$s radio { background-color: @theme_base_color; background-image: none;"
                " border: 1px solid alpha(@theme_fg_color, 0.35); color: @theme_selected_fg_color; box-shadow: none; -gtk-icon-shadow: none; }"
                "%1$s check:checked, %1$s radio:checked, %1$s check:indeterminate {"
                " background-color: @theme_selected_bg_color; border-color: @theme_selected_bg_color; }"
                "%1$s scale trough { background-color: alpha(@theme_fg_color, 0.16); background-image: none; border: none; }"
                "%1$s scale highlight { background-color: @theme_selected_bg_color; background-image: none; border: none; }"
                "%1$s scale slider { background-color: @ult_bg_solid; background-image: none; border: 1px solid @ult_border; box-shadow: 0 1px 2px alpha(black, 0.25); }"
                "%1$s switch { background-color: alpha(@theme_fg_color, 0.16); background-image: none; border-color: @ult_border; color: @theme_fg_color; }"
                "%1$s switch:checked { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; }"
                "%1$s switch slider { background-color: #FFFFFF; background-image: none; border-color: @ult_border; }"
                "%1$s scrollbar, %1$s scrollbar trough { background-color: transparent; background-image: none; border: none; }"
                "%1$s scrollbar slider { background-color: alpha(@theme_fg_color, 0.30); border: none; }"
                "%1$s scrollbar slider:hover { background-color: alpha(@theme_fg_color, 0.45); }"
                "%1$s label:disabled, %1$s *:disabled { color: alpha(@theme_fg_color, 0.45); }"
                "%1$s separator { background-color: @ult_border; }",
                S);
        }
        /* the settings window itself */
        g_string_append(css,
            ".ultimate-settings-win, .ultimate-settings-win.background,"
            " .ultimate-settings-win > *, .ultimate-settings-win scrolledwindow,"
            " .ultimate-settings-win viewport, .ultimate-settings-win stack, .ultimate-settings-win box"
            " { background-color: @ult_bg_solid; background-image: none; color: @theme_fg_color; }"
            ".ultimate-settings-win label { color: @theme_fg_color; }"
            ".ultimate-settings-win headerbar, .ultimate-settings-win .titlebar"
            " { background-color: @ult_bg_solid; background-image: none; color: @theme_fg_color; border-color: @ult_border; box-shadow: none; }"
            ".ultimate-settings-win list, .ultimate-settings-win row,"
            " .ultimate-settings-win stacksidebar, .ultimate-settings-win stacksidebar list"
            " { background-color: transparent; color: @theme_fg_color; border-color: @ult_border; }"
            ".ultimate-settings-win row:hover { background-color: @ult_hover; }"
            ".ultimate-settings-win row:selected, .ultimate-settings-win stacksidebar row:selected"
            " { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; }"
            ".ultimate-settings-win row:selected label { color: @theme_selected_fg_color; }"
            ".ultimate-settings-win frame > border, .ultimate-settings-win .frame { border-color: @ult_border; }");
        /* drop-downs, tooltips and pop-overs that open from the shell */
        g_string_append(css,
            "menu, .menu, .context-menu, popover, popover.background"
            " { background-color: @ult_bg_solid; background-image: none; color: @theme_fg_color; border-color: @ult_border; }"
            "menu menuitem, popover modelbutton { color: @theme_fg_color; }"
            /* sub-menus (e.g. Terminal > Set Encoding > ...) carry no shell class: make sure
               their labels, arrows and checks use the palette, not the GTK theme's colours */
            "menu menuitem label, .menu menuitem label, menu menuitem accelerator,"
            " menu menuitem arrow, menu menuitem check, menu menuitem radio { color: @theme_fg_color; }"
            "menu menuitem accelerator { color: alpha(@theme_fg_color, 0.55); }"
            "menu menuitem:hover > arrow, menu menuitem:hover > label > accelerator { color: @theme_selected_fg_color; }"
            "menu menuitem:disabled, menu menuitem:disabled > label, menu menuitem:disabled > arrow"
            " { color: alpha(@theme_fg_color, 0.40); }"
            "menu menuitem:hover, popover modelbutton:hover { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; }"
            "menu menuitem:hover > label { color: @theme_selected_fg_color; }"
            "menu menuitem:disabled > label { color: alpha(@theme_fg_color, 0.45); }"
            "tooltip, tooltip.background { background-color: @ult_bg_solid; background-image: none;"
            " color: @theme_fg_color; border: 1px solid @ult_border; box-shadow: none; text-shadow: none; }"
            "tooltip label, tooltip * { color: @theme_fg_color; }");
    }
    gtk_css_provider_load_from_data(prov, css->str, -1, NULL);
    g_string_free(css, TRUE);
}

/* ---- xsettings: the system GTK theme and icon theme ---- */
static XfconfChannel *xsettings_channel(void) {
    return xfconf_init(NULL) ? xfconf_channel_get("xsettings") : NULL;
}

/* kind: 0 GTK theme, 1 icon theme, 2 xfwm4 title bar theme */
static GList *list_theme_dirs(gint kind) {
    gboolean icons = (kind == 1);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *out = NULL;
    gchar *dirs[4];
    if (icons) {
        dirs[0] = g_build_filename(g_get_home_dir(), ".icons", NULL);
        dirs[1] = g_build_filename(g_get_user_data_dir(), "icons", NULL);
        dirs[2] = g_strdup("/usr/share/icons");
    } else {
        dirs[0] = g_build_filename(g_get_home_dir(), ".themes", NULL);
        dirs[1] = g_build_filename(g_get_user_data_dir(), "themes", NULL);
        dirs[2] = g_strdup("/usr/share/themes");
    }
    dirs[3] = NULL;
    for (gint d = 0; dirs[d]; d++) {
        GDir *dir = g_dir_open(dirs[d], 0, NULL);
        if (!dir) continue;
        const gchar *n;
        while ((n = g_dir_read_name(dir))) {
            if (g_hash_table_contains(seen, n)) continue;
            gchar *probe = icons ? g_build_filename(dirs[d], n, "index.theme", NULL)
                                 : g_build_filename(dirs[d], n, kind == 2 ? "xfwm4" : "gtk-3.0", NULL);
            gboolean ok = g_file_test(probe, icons ? G_FILE_TEST_IS_REGULAR : G_FILE_TEST_IS_DIR);
            if (ok && icons) {                     /* skip cursor-only themes */
                gchar *cur = g_build_filename(dirs[d], n, "cursors", NULL);
                GDir *sub = g_dir_open(g_build_filename(dirs[d], n, NULL), 0, NULL);
                gint entries = 0;
                if (sub) { while (g_dir_read_name(sub)) entries++; g_dir_close(sub); }
                if (g_file_test(cur, G_FILE_TEST_IS_DIR) && entries <= 2) ok = FALSE;
                g_free(cur);
            }
            if (ok) { g_hash_table_add(seen, g_strdup(n)); out = g_list_insert_sorted(out, g_strdup(n), (GCompareFunc)g_ascii_strcasecmp); }
            g_free(probe);
        }
        g_dir_close(dir);
    }
    for (gint d = 0; dirs[d]; d++) g_free(dirs[d]);
    g_hash_table_destroy(seen);
    if (kind == 0 && !g_list_find_custom(out, "Adwaita", (GCompareFunc)g_strcmp0))
        out = g_list_prepend(out, g_strdup("Adwaita"));   /* built into GTK */
    return out;
}

static XfconfChannel *xfwm4_channel(void) {
    return xfconf_init(NULL) ? xfconf_channel_get("xfwm4") : NULL;
}

static void on_sys_theme_combo_changed(GtkComboBox *c, gpointer prop) {
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c));
    XfconfChannel *xs = g_str_has_prefix((const gchar *)prop, "/general/") ? xfwm4_channel() : xsettings_channel();
    if (name && xs) xfconf_channel_set_string(xs, (const gchar *)prop, name);
    g_free(name);
}

static GtkWidget *make_sys_theme_combo(gint kind) {
    const gchar *prop = kind == 1 ? "/Net/IconThemeName" : kind == 2 ? "/general/theme" : "/Net/ThemeName";
    XfconfChannel *xs = kind == 2 ? xfwm4_channel() : xsettings_channel();
    gchar *cur = xs ? xfconf_channel_get_string(xs, prop, "") : g_strdup("");
    GtkWidget *c = gtk_combo_box_text_new();
    gint i = 0, active = -1;
    GList *names = list_theme_dirs(kind);
    for (GList *l = names; l; l = l->next, i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), l->data);
        if (g_strcmp0(l->data, cur) == 0) active = i;
    }
    g_list_free_full(names, g_free);
    if (active >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(c), active);
    g_signal_connect(c, "changed", G_CALLBACK(on_sys_theme_combo_changed), (gpointer)prop);
    g_free(cur);
    return c;
}

/* ---- General-tab appearance handlers ---- */
/* Re-apply the whole look at once: CSS, palette, dock and taskbar drawing */
static void shell_refresh_look(UltimateShell *shell) {
    shell_apply_theme(shell);                 /* also re-applies the appearance overrides */
    taskbar_update_icon_sizes(shell);
    if (shell->dock_window) { dock_window_apply_settings(shell); gtk_widget_queue_draw(shell->dock_window); }
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
}

static void appearance_changed(UltimateShell *shell) {
    shell_refresh_look(shell);
    save_settings_to_xfconf(shell);
}

static gboolean theme_apply_default_for(UltimateShell *shell, gboolean dark);
void shell_set_dark_mode(UltimateShell *shell, gboolean dark) {
    if (!shell) return;
    if (theme_apply_default_for(shell, dark)) return;   /* a default theme is set for this mode */
    shell->color_mode = dark ? COLOR_DARK : COLOR_LIGHT;
    appearance_changed(shell);
}

/* ---- Colours: mode + palette (with transparency) ---- */
static GtkWidget *g_color_combo = NULL;      /* holds the colour rows, for the swatch refresh */

static void swatch_set(GtkWidget *row, const GdkRGBA *c) {
    GtkWidget *cb = row ? g_object_get_data(G_OBJECT(row), "cb") : NULL;
    if (GTK_IS_COLOR_CHOOSER(cb)) gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(cb), c);
}

/* Greyed-out / automatic colour boxes show what is really used, not stale values */
static void refresh_color_swatches(UltimateShell *shell) {
    if (!GTK_IS_WIDGET(g_color_combo)) return;
    ShellPalette pl;
    shell_palette_get(shell, &pl);
    if (shell->color_mode != COLOR_CUSTOM) {
        swatch_set(g_object_get_data(G_OBJECT(g_color_combo), "row-bg"), &pl.bg);
        swatch_set(g_object_get_data(G_OBJECT(g_color_combo), "row-fg"), &pl.fg);
    }
    if (shell->color_mode == COLOR_FOLLOW_GTK)
        swatch_set(g_object_get_data(G_OBJECT(g_color_combo), "row-ac"), &pl.accent);
    if (!(shell->ui_hover_color && shell->ui_hover_color[0]))
        swatch_set(g_object_get_data(G_OBJECT(g_color_combo), "row-hover"), &pl.hover);
    if (!(shell->ui_border_color && shell->ui_border_color[0]))
        swatch_set(g_object_get_data(G_OBJECT(g_color_combo), "row-border"), &pl.border);
}

static void colors_update_sensitivity(GtkWidget *combo, UltimateShell *shell) {
    GtkWidget *bg = g_object_get_data(G_OBJECT(combo), "row-bg");
    GtkWidget *fg = g_object_get_data(G_OBJECT(combo), "row-fg");
    GtkWidget *ac = g_object_get_data(G_OBJECT(combo), "row-ac");
    gboolean custom = (shell->color_mode == COLOR_CUSTOM);
    if (bg) gtk_widget_set_sensitive(bg, custom);
    if (fg) gtk_widget_set_sensitive(fg, custom);
    if (ac) gtk_widget_set_sensitive(ac, shell->color_mode != COLOR_FOLLOW_GTK);
    refresh_color_swatches(shell);
}
static void on_color_mode_changed(GtkComboBox *c, UltimateShell *shell) {
    shell->color_mode = CLAMP(gtk_combo_box_get_active(c), 0, 3);
    appearance_changed(shell);
    colors_update_sensitivity(GTK_WIDGET(c), shell);
}
static void on_palette_color_set(GtkColorButton *btn, UltimateShell *shell) {
    gchar **field = g_object_get_data(G_OBJECT(btn), "field");
    if (!field) return;
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    g_free(*field);
    *field = (c.alpha < 0.999) ? gdk_rgba_to_string(&c) : rgba_to_hex(&c);
    appearance_changed(shell);
}
static GtkWidget *add_palette_row(GtkWidget *box, const gchar *label, gchar **field, const gchar *fallback, UltimateShell *shell) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GdkRGBA c;
    if (!(*field && (*field)[0] && gdk_rgba_parse(&c, *field))) gdk_rgba_parse(&c, fallback);
    GtkWidget *cb = gtk_color_button_new_with_rgba(&c);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(cb), TRUE);     /* transparency */
    g_object_set_data(G_OBJECT(cb), "field", field);
    g_signal_connect(cb, "color-set", G_CALLBACK(on_palette_color_set), shell);
    g_object_set_data(G_OBJECT(row), "cb", cb);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), cb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return row;
}
static void on_bar_opacity_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->dock_opacity = gtk_spin_button_get_value_as_int(s);
    appearance_changed(shell);
}
static void on_popup_opacity_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->start_opacity = gtk_spin_button_get_value_as_int(s);
    appearance_changed(shell);
}

static void on_ui_font_set(GtkFontButton *b, UltimateShell *shell) {
    g_free(shell->ui_font);
    shell->ui_font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(b));
    appearance_changed(shell);
}
static void on_ui_font_default(GtkButton *b, UltimateShell *shell) {
    g_free(shell->ui_font);
    shell->ui_font = g_strdup("");
    GtkWidget *fb = g_object_get_data(G_OBJECT(b), "font-btn");
    if (GTK_IS_FONT_BUTTON(fb)) gtk_font_chooser_set_font(GTK_FONT_CHOOSER(fb), "Sans 10");
    appearance_changed(shell);
}
static void on_ui_radius_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->ui_radius = gtk_spin_button_get_value_as_int(s);
    appearance_changed(shell);
}
static void on_ui_border_w_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->ui_border_w = gtk_spin_button_get_value_as_int(s);
    appearance_changed(shell);
}
static void on_ui_shadow_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ui_popup_shadow = gtk_toggle_button_get_active(b);
    appearance_changed(shell);
}
/* colour buttons with a "default" tick box; "field" = gchar** in UltimateShell */
static void on_ui_color_set(GtkColorButton *btn, UltimateShell *shell) {
    gchar **field = g_object_get_data(G_OBJECT(btn), "field");
    GdkRGBA c;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &c);
    g_free(*field);
    *field = (c.alpha < 0.999) ? gdk_rgba_to_string(&c) : rgba_to_hex(&c);
    GtkWidget *chk = g_object_get_data(G_OBJECT(btn), "default-chk");
    if (GTK_IS_TOGGLE_BUTTON(chk)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), FALSE);
    appearance_changed(shell);
}
static void on_ui_color_default(GtkToggleButton *chk, UltimateShell *shell) {
    GtkWidget *cb = g_object_get_data(G_OBJECT(chk), "color-btn");
    gchar **field = g_object_get_data(G_OBJECT(cb), "field");
    if (gtk_toggle_button_get_active(chk)) { g_free(*field); *field = g_strdup(""); }
    else if (!*field || !(*field)[0]) {
        GdkRGBA c; gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cb), &c);
        g_free(*field); *field = rgba_to_hex(&c);
    }
    gtk_widget_set_sensitive(cb, !gtk_toggle_button_get_active(chk));
    appearance_changed(shell);
    refresh_color_swatches(shell);
}
static GtkWidget *add_ui_color_row(GtkWidget *box, const gchar *label, gchar **field, const gchar *fallback, UltimateShell *shell) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GdkRGBA c;
    if (!(*field && (*field)[0] && gdk_rgba_parse(&c, *field))) gdk_rgba_parse(&c, fallback);
    GtkWidget *chk = gtk_check_button_new_with_label("Automatic");
    GtkWidget *cb = gtk_color_button_new_with_rgba(&c);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(cb), TRUE);
    gboolean def = !(*field && (*field)[0]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), def);
    gtk_widget_set_sensitive(cb, !def);
    g_object_set_data(G_OBJECT(cb), "field", field);
    g_object_set_data(G_OBJECT(cb), "default-chk", chk);
    g_object_set_data(G_OBJECT(chk), "color-btn", cb);
    g_signal_connect(cb, "color-set", G_CALLBACK(on_ui_color_set), shell);
    g_signal_connect(chk, "toggled", G_CALLBACK(on_ui_color_default), shell);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), cb, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(row), "cb", cb);
    return row;
}

/* The panel window is a plain rectangle with its own background and edge line.
   Frameless: make that window fully transparent (Style = solid colour, alpha 0),
   so only the shell's own background, with its rounded corners, is seen. */
/* ---- Panel shadow: a thin, click-through window drawn just outside the panel ----
   (a shadow drawn inside the panel window is clipped by it and barely shows) */
#define PANEL_SHADOW_PX 20
static GtkWidget *g_pshadow = NULL;
static gboolean   g_pshadow_down = TRUE;     /* top bar: shadow falls down */

static gboolean on_pshadow_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    gint W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, g_pshadow_down ? 0 : H, 0, g_pshadow_down ? H : 0);
    /* soft fall-off like xfwm4's window shadows: light, long, smooth */
    for (gint k = 0; k <= 10; k++) {
        gdouble t = k / 10.0;
        gdouble a = 0.17 * pow(1.0 - t, 2.2);
        cairo_pattern_add_color_stop_rgba(g, t, 0, 0, 0, a);
    }
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    return TRUE;
}

/* Clicks pass straight through. Set through GTK (not on the GdkWindow directly):
   GTK re-applies its own input shape when the window is mapped, which undid ours
   and left the strip catching clicks on title bars snapped to the panel. */
static void pshadow_make_clickthrough(GtkWidget *w) {
    cairo_region_t *empty = cairo_region_create();
    gtk_widget_input_shape_combine_region(w, empty);
    if (gtk_widget_get_window(w)) gdk_window_input_shape_combine_region(gtk_widget_get_window(w), empty, 0, 0);
    cairo_region_destroy(empty);
}
static void on_pshadow_realize(GtkWidget *w, gpointer d) { pshadow_make_clickthrough(w); }
static void on_pshadow_map(GtkWidget *w, gpointer d)     { pshadow_make_clickthrough(w); }

static gboolean shell_compositing_active(void);
static void panel_shadow_update(UltimateShell *shell) {
    gboolean want = shell && shell->ui_panel_shadow && shell->ui_panel_frameless && shell_compositing_active()
                    && shell->root_box && gtk_widget_get_mapped(shell->root_box);
    if (!want) { if (g_pshadow) gtk_widget_hide(g_pshadow); return; }

    GtkWidget *top = gtk_widget_get_toplevel(shell->root_box);
    GdkWindow *pw = top ? gtk_widget_get_window(top) : NULL;
    if (!pw) return;
    gint px, py;
    gdk_window_get_origin(pw, &px, &py);
    gint pwid = gdk_window_get_width(pw), phei = gdk_window_get_height(pw);
    g_pshadow_down = (shell->mode == LAYOUT_MACOS);
    /* start where the panel's visible background ends (inside the border-width gap) */
    gint gap = 0;
    gint y = g_pshadow_down ? (py + phei - gap) : (py + gap - PANEL_SHADOW_PX);

    if (!g_pshadow) {
        g_pshadow = gtk_window_new(GTK_WINDOW_POPUP);
        GdkVisual *v = gdk_screen_get_rgba_visual(gtk_widget_get_screen(g_pshadow));
        if (v) gtk_widget_set_visual(g_pshadow, v);
        gtk_widget_set_app_paintable(g_pshadow, TRUE);
        g_signal_connect(g_pshadow, "realize", G_CALLBACK(on_pshadow_realize), NULL);
        g_signal_connect_after(g_pshadow, "map", G_CALLBACK(on_pshadow_map), NULL);
        gtk_window_set_accept_focus(GTK_WINDOW(g_pshadow), FALSE);
        g_signal_connect(g_pshadow, "draw", G_CALLBACK(on_pshadow_draw), NULL);
    }
    gint inset = 0;
    gtk_widget_set_size_request(g_pshadow, MAX(1, pwid - 2 * inset), PANEL_SHADOW_PX);
    gtk_window_resize(GTK_WINDOW(g_pshadow), MAX(1, pwid - 2 * inset), PANEL_SHADOW_PX);
    gtk_window_move(GTK_WINDOW(g_pshadow), px + inset, y);
    gtk_widget_show(g_pshadow);
    gtk_widget_queue_draw(g_pshadow);
}

static guint g_pshadow_idle = 0;
static gboolean panel_shadow_idle(gpointer d) { g_pshadow_idle = 0; panel_shadow_update(d); return G_SOURCE_REMOVE; }
static void on_root_box_size_allocate(GtkWidget *w, GdkRectangle *a, UltimateShell *shell) {
    if (!g_pshadow_idle) g_pshadow_idle = g_idle_add(panel_shadow_idle, shell);
}

static void set_dock_shadow(gboolean on);
static void apply_panel_frame(UltimateShell *shell) {
    if (!shell || !shell->channel || !shell->ui_panel_frameless) return;
    if (!shell_compositing_active()) return;      /* without compositing, alpha 0 would paint black */
    set_dock_shadow(FALSE);
    const gchar *panels[] = { "/panels/panel-1", "/panels/panel-0" };
    for (gsize i = 0; i < G_N_ELEMENTS(panels); i++) {
        gchar *pos = g_strdup_printf("%s/position", panels[i]);
        if (xfconf_channel_has_property(shell->channel, pos)) {
            gchar *st = g_strdup_printf("%s/background-style", panels[i]);
            gchar *rg = g_strdup_printf("%s/background-rgba", panels[i]);
            gchar *al = g_strdup_printf("%s/background-alpha", panels[i]);
            if (xfconf_channel_get_uint(shell->channel, st, 0) != 1) xfconf_channel_set_uint(shell->channel, st, 1);
            gdouble r = 0, g = 0, b = 0, a = 0;
            xfconf_channel_set_array(shell->channel, rg, G_TYPE_DOUBLE, &r, G_TYPE_DOUBLE, &g,
                                     G_TYPE_DOUBLE, &b, G_TYPE_DOUBLE, &a, G_TYPE_INVALID);
            if (xfconf_channel_has_property(shell->channel, al)) xfconf_channel_set_uint(shell->channel, al, 0);
            g_free(st); g_free(rg); g_free(al);
        }
        g_free(pos);
    }
}
/* xfwm4's compositor draws a rectangular shadow under "dock" windows such as the
   panel - that is the sharp line around it, so it goes with the frame */
static void set_dock_shadow(gboolean on) {
    XfconfChannel *wm = xfconf_init(NULL) ? xfconf_channel_get("xfwm4") : NULL;
    if (wm && xfconf_channel_get_bool(wm, "/general/show_dock_shadow", TRUE) != on)
        xfconf_channel_set_bool(wm, "/general/show_dock_shadow", on);
}

static void on_panel_frameless_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ui_panel_frameless = gtk_toggle_button_get_active(b);
    save_settings_to_xfconf(shell);
    if (shell->ui_panel_frameless) {
        apply_panel_frame(shell);
    } else if (shell->channel) {                  /* back to the panel's normal (theme) background */
        set_dock_shadow(TRUE);
        const gchar *panels[] = { "/panels/panel-1/background-style", "/panels/panel-0/background-style" };
        for (gsize i = 0; i < G_N_ELEMENTS(panels); i++)
            if (xfconf_channel_has_property(shell->channel, panels[i])) xfconf_channel_set_uint(shell->channel, panels[i], 0);
    }
    panel_shadow_update(shell);
}


static void on_wm_fill_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->wm_fill_maximized = gtk_toggle_button_get_active(b);
    save_settings_to_xfconf(shell);
    win_fill_apply_settings(shell);
}

static void on_panel_shadow_toggled(GtkToggleButton *b, UltimateShell *shell) {
    shell->ui_panel_shadow = gtk_toggle_button_get_active(b);
    save_settings_to_xfconf(shell);
    panel_shadow_update(shell);
}

static void on_topbar_height_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->mac_panel_size = gtk_spin_button_get_value_as_int(s);
    save_settings_to_xfconf(shell);
    if (shell->mode == LAYOUT_MACOS) sync_xfce_panel_edge_and_size(shell, TRUE);
}
static void on_taskbar_height_spin(GtkSpinButton *s, UltimateShell *shell) {
    shell->win_panel_size = gtk_spin_button_get_value_as_int(s);
    save_settings_to_xfconf(shell);
    if (shell->mode != LAYOUT_MACOS) sync_xfce_panel_edge_and_size(shell, FALSE);
}

/* ---- Shell themes: a key file with the look settings + GTK / icon theme ----
   Saved in ~/.local/share/xfce4-ultimate-shell/themes/<name>.theme */
static const gchar *const THEME_KEYS[] = {
    "color_mode", "custom_bg", "custom_fg", "custom_accent",
    "ui_", "ind_", "dock_ind_style", "dock_opacity", "dock_border", "dock_radius_val", "dock_icon_shadow",
    "start_opacity", "tb_show_inactive_ind", NULL
};

static gboolean is_theme_key(const gchar *base) {
    for (gint i = 0; THEME_KEYS[i]; i++) if (g_str_has_prefix(base, THEME_KEYS[i])) return TRUE;
    return FALSE;
}

static gchar *themes_dir(void) {
    gchar *d = g_build_filename(g_get_user_data_dir(), "xfce4-ultimate-shell", "themes", NULL);
    g_mkdir_with_parents(d, 0755);
    return d;
}

static gchar *encode_gvalue(const GValue *val) {
    if (G_VALUE_HOLDS_BOOLEAN(val)) return g_strdup_printf("b:%s", g_value_get_boolean(val) ? "true" : "false");
    if (G_VALUE_HOLDS_INT(val))     return g_strdup_printf("i:%d", g_value_get_int(val));
    if (G_VALUE_HOLDS_UINT(val))    return g_strdup_printf("u:%u", g_value_get_uint(val));
    if (G_VALUE_HOLDS_DOUBLE(val))  { gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
                                      return g_strdup_printf("d:%s", g_ascii_dtostr(buf, sizeof buf, g_value_get_double(val))); }
    if (G_VALUE_HOLDS_STRING(val))  return g_strdup_printf("s:%s", g_value_get_string(val) ? g_value_get_string(val) : "");
    return NULL;
}

static void set_encoded_prop(XfconfChannel *ch, const gchar *base, const gchar *enc) {
    if (!enc || strlen(enc) < 2 || enc[1] != ':' || strchr(base, '/') || strstr(base, "..")) return;
    gchar *full = g_strdup_printf(ULT_PROP_ROOT "/%s", base);
    const gchar *v = enc + 2;
    switch (enc[0]) {
        case 'b': xfconf_channel_set_bool(ch, full, g_strcmp0(v, "true") == 0); break;
        case 'i': xfconf_channel_set_int(ch, full, (gint)g_ascii_strtoll(v, NULL, 10)); break;
        case 'u': xfconf_channel_set_uint(ch, full, (guint)g_ascii_strtoull(v, NULL, 10)); break;
        case 'd': xfconf_channel_set_double(ch, full, g_ascii_strtod(v, NULL)); break;
        case 's': xfconf_channel_set_string(ch, full, v); break;
        default: break;
    }
    g_free(full);
}

/* Current look -> key file */
static GKeyFile *theme_capture(UltimateShell *shell, const gchar *name) {
    save_settings_to_xfconf(shell);
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Theme", "Name", name);
    XfconfChannel *xs = xsettings_channel();
    if (xs) {
        gchar *g = xfconf_channel_get_string(xs, "/Net/ThemeName", "");
        gchar *i = xfconf_channel_get_string(xs, "/Net/IconThemeName", "");
        g_key_file_set_string(kf, "Theme", "GtkTheme", g);
        g_key_file_set_string(kf, "Theme", "IconTheme", i);
        g_free(g); g_free(i);
    }
    XfconfChannel *wm = xfwm4_channel();
    if (wm) {
        gchar *t = xfconf_channel_get_string(wm, "/general/theme", "");
        g_key_file_set_string(kf, "Theme", "WmTheme", t);
        g_free(t);
    }
    GHashTable *props = xfconf_channel_get_properties(shell->channel, ULT_PROP_ROOT);
    if (props) {
        GHashTableIter it; gpointer k, v;
        g_hash_table_iter_init(&it, props);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            const gchar *full = k;
            if (!g_str_has_prefix(full, ULT_PROP_ROOT "/")) continue;
            const gchar *base = full + strlen(ULT_PROP_ROOT "/");
            if (!is_theme_key(base)) continue;
            gchar *enc = encode_gvalue(v);
            if (enc) { g_key_file_set_string(kf, "ultimateshell", base, enc); g_free(enc); }
        }
        g_hash_table_destroy(props);
    }
    return kf;
}

/* Key file -> settings */
static void theme_apply(UltimateShell *shell, GKeyFile *kf) {
    if (!shell->channel) return;
    /* only the settings in the theme change; everything else stays as you set it */
    gsize n = 0;
    gchar **keys = g_key_file_get_keys(kf, "ultimateshell", &n, NULL);
    for (gsize i = 0; keys && i < n; i++) {
        if (!is_theme_key(keys[i])) continue;           /* a theme only changes the look */
        gchar *enc = g_key_file_get_string(kf, "ultimateshell", keys[i], NULL);
        set_encoded_prop(shell->channel, keys[i], enc);
        g_free(enc);
    }
    g_strfreev(keys);

    gchar *name = g_key_file_get_string(kf, "Theme", "Name", NULL);
    xfconf_channel_set_string(shell->channel, ULT_PROP_ROOT "/shell_theme_current", name ? name : "");
    xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", FALSE);
    g_free(name);

    XfconfChannel *xs = xsettings_channel();
    gchar *g = g_key_file_get_string(kf, "Theme", "GtkTheme", NULL);
    gchar *ic = g_key_file_get_string(kf, "Theme", "IconTheme", NULL);
    if (xs && g && g[0])  xfconf_channel_set_string(xs, "/Net/ThemeName", g);
    if (xs && ic && ic[0]) xfconf_channel_set_string(xs, "/Net/IconThemeName", ic);
    g_free(g); g_free(ic);
    gchar *wmt = g_key_file_get_string(kf, "Theme", "WmTheme", NULL);
    XfconfChannel *wm = xfwm4_channel();
    if (wm && wmt && wmt[0]) xfconf_channel_set_string(wm, "/general/theme", wmt);
    g_free(wmt);
    settings_sync_memory_then_restart(shell);
}

/* Built-in themes: colours, accent and GTK theme only - they never touch your
   shapes, transparency or indicators (your own saved themes restore everything) */
typedef struct { const gchar *name, *gtk, *kv; } BuiltinTheme;
static const BuiltinTheme BUILTIN_THEMES[] = {
    { "macOS Light", "Adwaita",
      "color_mode=i:1;custom_accent=s:#007AFF;"
      "" },
    { "macOS Dark", "Adwaita-dark",
      "color_mode=i:2;custom_accent=s:#0A84FF;"
      "" },
    { "Windows 11 Light", "Adwaita",
      "color_mode=i:1;custom_accent=s:#0067C0;"
      "" },
    { "Windows 11 Dark", "Adwaita-dark",
      "color_mode=i:2;custom_accent=s:#4CC2FF;"
      "" },
};

static GKeyFile *builtin_theme_keyfile(const BuiltinTheme *t) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Theme", "Name", t->name);
    g_key_file_set_string(kf, "Theme", "GtkTheme", t->gtk);
    gchar **pairs = g_strsplit(t->kv, ";", -1);
    for (gint i = 0; pairs[i]; i++) {
        gchar *eq = strchr(pairs[i], '=');
        if (!eq) continue;
        *eq = '\0';
        g_key_file_set_string(kf, "ultimateshell", pairs[i], eq + 1);
    }
    g_strfreev(pairs);
    return kf;
}

/* Theme picker: id = "builtin:<n>" or a file path */
static void theme_combo_fill(GtkComboBoxText *c, const gchar *current) {
    gtk_combo_box_text_remove_all(c);
    gint idx = 0, active = -1;
    for (guint i = 0; i < G_N_ELEMENTS(BUILTIN_THEMES); i++, idx++) {
        gchar *id = g_strdup_printf("builtin:%u", i);
        gtk_combo_box_text_append(c, id, BUILTIN_THEMES[i].name);
        if (g_strcmp0(current, BUILTIN_THEMES[i].name) == 0) active = idx;
        g_free(id);
    }
    gchar *dir = themes_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d))) {
            if (!g_str_has_suffix(n, ".theme")) continue;
            gchar *path = g_build_filename(dir, n, NULL);
            GKeyFile *kf = g_key_file_new();
            gchar *name = NULL;
            if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
                name = g_key_file_get_string(kf, "Theme", "Name", NULL);
            if (!name) name = g_strndup(n, strlen(n) - 6);
            gtk_combo_box_text_append(c, path, name);
            if (g_strcmp0(current, name) == 0) active = idx;
            idx++;
            g_free(name); g_key_file_free(kf); g_free(path);
        }
        g_dir_close(d);
    }
    g_free(dir);
    if (active >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(c), active);
}

static GKeyFile *theme_load_selected(GtkComboBox *c) {
    const gchar *id = gtk_combo_box_get_active_id(c);
    if (!id) return NULL;
    if (g_str_has_prefix(id, "builtin:")) {
        guint i = (guint)g_ascii_strtoull(id + 8, NULL, 10);
        return i < G_N_ELEMENTS(BUILTIN_THEMES) ? builtin_theme_keyfile(&BUILTIN_THEMES[i]) : NULL;
    }
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, id, G_KEY_FILE_NONE, NULL)) { g_key_file_free(kf); return NULL; }
    return kf;
}

static void on_theme_apply_clicked(GtkButton *b, UltimateShell *shell) {
    GtkComboBox *c = g_object_get_data(G_OBJECT(b), "theme-combo");
    GKeyFile *kf = theme_load_selected(c);
    if (!kf) return;
    if (settings_confirm(GTK_WIDGET(b), "Apply this shell theme?",
            "Colours, fonts, shapes, indicators and the GTK, icon and title bar themes change. "
            "Layout and behaviour stay as they are. The panel restarts to apply it.", "_Apply"))
        theme_apply(shell, kf);
    g_key_file_free(kf);
}

static gchar *ask_theme_name(GtkWidget *any) {
    GtkWidget *top = gtk_widget_get_toplevel(any);
    GtkWidget *d = gtk_dialog_new_with_buttons("Save Shell Theme", GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL,
        GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "Theme name");
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_pack_start(GTK_BOX(area), gtk_label_new("Saves the current look, including the GTK, icon and title bar themes."), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(area), e, FALSE, FALSE, 6);
    gtk_widget_show_all(d);
    gchar *name = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
        if (!name[0]) { g_free(name); name = NULL; }
    }
    gtk_widget_destroy(d);
    return name;
}

static gchar *theme_file_for(const gchar *name) {
    gchar *safe = g_strdup(name);
    for (gchar *p = safe; *p; p++) if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') *p = '_';
    gchar *dir = themes_dir();
    gchar *fn = g_strdup_printf("%s.theme", safe);
    gchar *path = g_build_filename(dir, fn, NULL);
    g_free(safe); g_free(dir); g_free(fn);
    return path;
}

static void look_snapshot_reset(UltimateShell *shell);
static void theme_status_update(UltimateShell *shell);
static void on_theme_save_clicked(GtkButton *b, UltimateShell *shell) {
    gchar *name = ask_theme_name(GTK_WIDGET(b));
    if (!name) return;
    GKeyFile *kf = theme_capture(shell, name);
    gchar *path = theme_file_for(name);
    g_key_file_save_to_file(kf, path, NULL);
    xfconf_channel_set_string(shell->channel, ULT_PROP_ROOT "/shell_theme_current", name);
    xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", FALSE);
    theme_combo_fill(GTK_COMBO_BOX_TEXT(g_object_get_data(G_OBJECT(b), "theme-combo")), name);
    look_snapshot_reset(shell);
    theme_status_update(shell);
    g_key_file_free(kf); g_free(path); g_free(name);
}

/* ---- default dark / light theme: the Dark mode toggle switches between them ---- */
static GKeyFile *theme_load_id(const gchar *id) {
    if (!id || !id[0]) return NULL;
    if (g_str_has_prefix(id, "builtin:")) {
        guint i = (guint)g_ascii_strtoull(id + 8, NULL, 10);
        return i < G_N_ELEMENTS(BUILTIN_THEMES) ? builtin_theme_keyfile(&BUILTIN_THEMES[i]) : NULL;
    }
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, id, G_KEY_FILE_NONE, NULL)) { g_key_file_free(kf); return NULL; }
    return kf;
}
static gboolean theme_apply_default_for(UltimateShell *shell, gboolean dark) {
    if (!shell || !shell->channel) return FALSE;
    gchar *id = xfconf_channel_get_string(shell->channel,
        dark ? ULT_PROP_ROOT "/shell_theme_default_dark" : ULT_PROP_ROOT "/shell_theme_default_light", "");
    GKeyFile *kf = theme_load_id(id);
    g_free(id);
    if (!kf) return FALSE;                  /* none set, or the theme was deleted: plain Light / Dark */
    theme_apply(shell, kf);
    g_key_file_free(kf);
    return TRUE;
}
static GtkWidget *g_theme_defaults_lbl = NULL;
static void theme_defaults_update(UltimateShell *shell) {
    if (!GTK_IS_LABEL(g_theme_defaults_lbl) || !shell->channel) return;
    gchar *dn = xfconf_channel_get_string(shell->channel, ULT_PROP_ROOT "/shell_theme_default_dark_name", "");
    gchar *ln = xfconf_channel_get_string(shell->channel, ULT_PROP_ROOT "/shell_theme_default_light_name", "");
    gchar *t = g_markup_printf_escaped("Dark mode toggle - Dark: <b>%s</b>   Light: <b>%s</b>",
                                       dn[0] ? dn : "plain Dark colours", ln[0] ? ln : "plain Light colours");
    gtk_label_set_markup(GTK_LABEL(g_theme_defaults_lbl), t);
    g_free(t); g_free(dn); g_free(ln);
}
static void theme_set_default(GtkButton *b, UltimateShell *shell, gboolean dark) {
    GtkComboBox *c = g_object_get_data(G_OBJECT(b), "theme-combo");
    const gchar *id = c ? gtk_combo_box_get_active_id(c) : NULL;
    if (!id || !shell->channel) return;
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c));
    xfconf_channel_set_string(shell->channel, dark ? ULT_PROP_ROOT "/shell_theme_default_dark" : ULT_PROP_ROOT "/shell_theme_default_light", id);
    xfconf_channel_set_string(shell->channel, dark ? ULT_PROP_ROOT "/shell_theme_default_dark_name" : ULT_PROP_ROOT "/shell_theme_default_light_name", name ? name : "");
    g_free(name);
    theme_defaults_update(shell);
}
static void on_theme_default_dark_clicked(GtkButton *b, UltimateShell *shell) { theme_set_default(b, shell, TRUE); }
static void on_theme_default_light_clicked(GtkButton *b, UltimateShell *shell) { theme_set_default(b, shell, FALSE); }
static void on_theme_default_clear_clicked(GtkButton *b, UltimateShell *shell) {
    if (!shell->channel) return;
    const gchar *keys[] = { "/shell_theme_default_dark", "/shell_theme_default_light",
                            "/shell_theme_default_dark_name", "/shell_theme_default_light_name" };
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        gchar *k = g_strconcat(ULT_PROP_ROOT, keys[i], NULL);
        xfconf_channel_reset_property(shell->channel, k, FALSE);
        g_free(k);
    }
    theme_defaults_update(shell);
}

static void on_theme_delete_clicked(GtkButton *b, UltimateShell *shell) {
    GtkComboBox *c = g_object_get_data(G_OBJECT(b), "theme-combo");
    const gchar *id = gtk_combo_box_get_active_id(c);
    if (!id || g_str_has_prefix(id, "builtin:")) return;          /* built-ins can't be deleted */
    if (!settings_confirm(GTK_WIDGET(b), "Delete this theme?", "The theme file is removed. Your current look does not change.", "_Delete")) return;
    g_unlink(id);
    theme_combo_fill(GTK_COMBO_BOX_TEXT(c), NULL);
}

static void on_theme_export_clicked(GtkButton *b, UltimateShell *shell) {
    GtkComboBox *c = g_object_get_data(G_OBJECT(b), "theme-combo");
    GKeyFile *kf = theme_load_selected(c);
    if (!kf) return;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Export Shell Theme",
        GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "_Export", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
    gchar *name = g_key_file_get_string(kf, "Theme", "Name", NULL);
    gchar *fn = g_strdup_printf("%s.theme", name ? name : "shell");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), fn);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        g_key_file_save_to_file(kf, path, NULL);
        g_free(path);
    }
    g_object_unref(fc); g_free(fn); g_free(name); g_key_file_free(kf);
}

static void on_theme_import_clicked(GtkButton *b, UltimateShell *shell) {
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Import Shell Theme",
        GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "_Import", "_Cancel");
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        gchar *src = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, src, G_KEY_FILE_NONE, NULL) && g_key_file_has_group(kf, "Theme")) {
            gchar *name = g_key_file_get_string(kf, "Theme", "Name", NULL);
            if (!name) name = g_path_get_basename(src);
            gchar *dst = theme_file_for(name);
            g_key_file_save_to_file(kf, dst, NULL);     /* copy into the themes folder */
            theme_combo_fill(GTK_COMBO_BOX_TEXT(g_object_get_data(G_OBJECT(b), "theme-combo")), name);
            g_free(dst); g_free(name);
        }
        g_key_file_free(kf); g_free(src);
    }
    g_object_unref(fc);
}

/* ---- Unsaved look changes: offer to save them to the current shell theme ---- */
static gchar     *g_look_snapshot = NULL;     /* the look when the settings opened / were last saved */
static GtkWidget *g_theme_combo = NULL;
static GtkWidget *g_theme_status = NULL;

static gchar *look_fingerprint(UltimateShell *shell) {
    GKeyFile *kf = theme_capture(shell, "");
    gchar *data = g_key_file_to_data(kf, NULL, NULL);
    g_key_file_free(kf);
    return data;
}

static gboolean is_builtin_theme_name(const gchar *name) {
    for (guint i = 0; i < G_N_ELEMENTS(BUILTIN_THEMES); i++)
        if (g_strcmp0(name, BUILTIN_THEMES[i].name) == 0) return TRUE;
    return FALSE;
}

static void theme_status_update(UltimateShell *shell) {
    if (!GTK_IS_LABEL(g_theme_status) || !shell->channel) return;
    gchar *cur = xfconf_channel_get_string(shell->channel, ULT_PROP_ROOT "/shell_theme_current", "");
    gboolean mod = xfconf_channel_get_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", FALSE);
    gchar *t = cur[0] ? g_markup_printf_escaped("Current look: <b>%s</b>%s", cur, mod ? " (modified)" : "")
                      : g_strdup("Current look: <b>your own settings</b>");
    gtk_label_set_markup(GTK_LABEL(g_theme_status), t);
    g_free(t); g_free(cur);
}

static void look_snapshot_reset(UltimateShell *shell) {
    g_free(g_look_snapshot);
    g_look_snapshot = look_fingerprint(shell);
}

/* Called when leaving the General page and when the settings window closes */
static void theme_check_unsaved(GtkWidget *any, UltimateShell *shell) {
    if (!shell || !shell->channel || !g_look_snapshot) return;
    gchar *now = look_fingerprint(shell);
    gboolean changed = g_strcmp0(now, g_look_snapshot) != 0;
    g_free(now);
    if (!changed) return;

    gchar *name = xfconf_channel_get_string(shell->channel, ULT_PROP_ROOT "/shell_theme_current", "");
    gchar *path = name[0] ? theme_file_for(name) : NULL;
    gboolean own = name[0] && !is_builtin_theme_name(name) && path && g_file_test(path, G_FILE_TEST_EXISTS);

    GtkWidget *top = any ? gtk_widget_get_toplevel(any) : NULL;
    GtkWidget *d = gtk_message_dialog_new(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                          own ? "Save your look changes to the theme?" : "Save your look changes as a theme?");
    if (own)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
            "You changed the look while “%s” is the current theme.", name);
    else if (name[0])
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
            "“%s” is a built-in theme and can’t be changed, but you can save your look as a new theme.", name);
    else
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Your changes are kept either way.");
    gtk_dialog_add_button(GTK_DIALOG(d), "_Keep Without Saving", 3);
    gtk_dialog_add_button(GTK_DIALOG(d), "Save as _New Theme…", 2);
    if (own) gtk_dialog_add_button(GTK_DIALOG(d), "_Save to Theme", 1);
    gtk_dialog_set_default_response(GTK_DIALOG(d), own ? 1 : 2);
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);

    if (r == 1 && own) {
        GKeyFile *kf = theme_capture(shell, name);
        g_key_file_save_to_file(kf, path, NULL);
        g_key_file_free(kf);
        xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", FALSE);
    } else if (r == 2) {
        gchar *nn = ask_theme_name(any);
        if (nn) {
            GKeyFile *kf = theme_capture(shell, nn);
            gchar *np = theme_file_for(nn);
            g_key_file_save_to_file(kf, np, NULL);
            xfconf_channel_set_string(shell->channel, ULT_PROP_ROOT "/shell_theme_current", nn);
            xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", FALSE);
            if (GTK_IS_COMBO_BOX_TEXT(g_theme_combo)) theme_combo_fill(GTK_COMBO_BOX_TEXT(g_theme_combo), nn);
            g_key_file_free(kf); g_free(np); g_free(nn);
        } else if (name[0]) {
            xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", TRUE);
        }
    } else if (name[0]) {
        xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/shell_theme_modified", TRUE);   /* kept */
    }
    look_snapshot_reset(shell);
    theme_status_update(shell);
    g_free(path); g_free(name);
}

static void on_settings_page_switched(GObject *stack, GParamSpec *ps, UltimateShell *shell) {
    const gchar *now = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    const gchar *prev = g_object_get_data(stack, "prev-page");
    if (g_strcmp0(prev, "general") == 0 && g_strcmp0(now, "general") != 0)
        theme_check_unsaved(GTK_WIDGET(stack), shell);
    g_object_set_data_full(stack, "prev-page", g_strdup(now), g_free);
}

static void on_settings_closing(GtkDialog *dlg, gint resp, UltimateShell *shell) {
    theme_check_unsaved(GTK_WIDGET(dlg), shell);
}

static void build_general_appearance(GtkWidget *vbox, UltimateShell *shell) {
    GtkWidget *hd = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hd), "<b>Shell Theme</b>");
    gtk_widget_set_halign(hd, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), hd, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *combo = gtk_combo_box_text_new();
    gchar *cur = shell->channel ? xfconf_channel_get_string(shell->channel, ULT_PROP_ROOT "/shell_theme_current", "") : g_strdup("");
    theme_combo_fill(GTK_COMBO_BOX_TEXT(combo), cur);
    g_free(cur);
    gtk_box_pack_start(GTK_BOX(row), combo, TRUE, TRUE, 0);
    struct { const gchar *label; GCallback cb; } btns[] = {
        { "Apply", G_CALLBACK(on_theme_apply_clicked) }, { "Save as…", G_CALLBACK(on_theme_save_clicked) },
        { "Delete", G_CALLBACK(on_theme_delete_clicked) }, { "Import…", G_CALLBACK(on_theme_import_clicked) },
        { "Export…", G_CALLBACK(on_theme_export_clicked) },
    };
    for (guint i = 0; i < G_N_ELEMENTS(btns); i++) {
        GtkWidget *bt = gtk_button_new_with_label(btns[i].label);
        g_object_set_data(G_OBJECT(bt), "theme-combo", combo);
        g_signal_connect(bt, "clicked", btns[i].cb, shell);
        gtk_box_pack_start(GTK_BOX(row), bt, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
    {
        GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        struct { const gchar *label, *tip; GCallback cb; } b2[] = {
            { "Set as Default Dark Theme", "The Dark mode toggle in Quick Settings switches to the selected theme", G_CALLBACK(on_theme_default_dark_clicked) },
            { "Set as Default Light Theme", "Turning Dark mode off switches to the selected theme", G_CALLBACK(on_theme_default_light_clicked) },
            { "Clear Defaults", "The Dark mode toggle only switches the Light / Dark colours", G_CALLBACK(on_theme_default_clear_clicked) },
        };
        for (guint i = 0; i < G_N_ELEMENTS(b2); i++) {
            GtkWidget *bt = gtk_button_new_with_label(b2[i].label);
            gtk_widget_set_tooltip_text(bt, b2[i].tip);
            g_object_set_data(G_OBJECT(bt), "theme-combo", combo);
            g_signal_connect(bt, "clicked", b2[i].cb, shell);
            gtk_box_pack_start(GTK_BOX(row2), bt, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(vbox), row2, FALSE, FALSE, 0);
        g_theme_defaults_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(g_theme_defaults_lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(g_theme_defaults_lbl), PANGO_ELLIPSIZE_END);
        g_signal_connect(g_theme_defaults_lbl, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_theme_defaults_lbl);
        gtk_box_pack_start(GTK_BOX(vbox), g_theme_defaults_lbl, FALSE, FALSE, 0);
        theme_defaults_update(shell);
    }
    g_theme_combo = combo;
    g_signal_connect(combo, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_theme_combo);
    g_theme_status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(g_theme_status), 0.0);
    g_signal_connect(g_theme_status, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_theme_status);
    gtk_box_pack_start(GTK_BOX(vbox), g_theme_status, FALSE, FALSE, 0);
    theme_status_update(shell);
    GtkWidget *tn = gtk_label_new("A theme holds the look only: colours, transparency, fonts, shapes, indicators, and the GTK, icon and title bar themes. Panel heights, layout and behaviour are not changed.");
    gtk_label_set_line_wrap(GTK_LABEL(tn), TRUE);
    gtk_label_set_xalign(GTK_LABEL(tn), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(tn), "dim-label");
    gtk_box_pack_start(GTK_BOX(vbox), tn, FALSE, FALSE, 0);

    /* ---- Colours ---- */
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    hd = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hd), "<b>Colours</b>");
    gtk_widget_set_halign(hd, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), hd, FALSE, FALSE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Colours:"), FALSE, FALSE, 0);
    GtkWidget *cm = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cm), "Follow GTK theme");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cm), "Light");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cm), "Dark");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cm), "Custom");
    gtk_combo_box_set_active(GTK_COMBO_BOX(cm), CLAMP(shell->color_mode, 0, 3));
    gtk_box_pack_end(GTK_BOX(row), cm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(cm), "row-bg", add_palette_row(vbox, "Background:", &shell->custom_bg, "#F5F5F7", shell));
    g_object_set_data(G_OBJECT(cm), "row-fg", add_palette_row(vbox, "Text:",       &shell->custom_fg, "#1D1D1F", shell));
    g_object_set_data(G_OBJECT(cm), "row-ac", add_palette_row(vbox, "Accent:",     &shell->custom_accent, "#0A84FF", shell));
    g_object_set_data(G_OBJECT(cm), "row-hover", add_ui_color_row(vbox, "Hover & highlight:", &shell->ui_hover_color, "#3894FF", shell));
    g_object_set_data(G_OBJECT(cm), "row-border", add_ui_color_row(vbox, "Borders:", &shell->ui_border_color, "#888888", shell));
    g_color_combo = cm;
    g_signal_connect(cm, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_color_combo);
    colors_update_sensitivity(cm, shell);
    g_signal_connect(cm, "changed", G_CALLBACK(on_color_mode_changed), shell);
    GtkWidget *cn = gtk_label_new("Follow GTK theme uses your GTK theme's colours. Light, Dark and Custom give the panel, dock, popups, menus and this window one palette. Every colour picker has a transparency slider.");
    gtk_label_set_line_wrap(GTK_LABEL(cn), TRUE);
    gtk_label_set_xalign(GTK_LABEL(cn), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cn), "dim-label");
    gtk_box_pack_start(GTK_BOX(vbox), cn, FALSE, FALSE, 0);

    add_spin_setting_row(vbox, "Panel & dock opacity (10 – 100 %):", 10, 100, 5,
                         CLAMP(shell->dock_opacity, 10, 100), G_CALLBACK(on_bar_opacity_spin), shell);
    add_spin_setting_row(vbox, "Popups & menus opacity (20 – 100 %):", 20, 100, 5,
                         shell->start_opacity > 0 ? shell->start_opacity : 95, G_CALLBACK(on_popup_opacity_spin), shell);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    hd = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hd), "<b>Look</b>");
    gtk_widget_set_halign(hd, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), hd, FALSE, FALSE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("GTK theme:"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), make_sys_theme_combo(0), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Icon theme:"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), make_sys_theme_combo(1), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Window title bar style:"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), make_sys_theme_combo(2), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Shell font:"), FALSE, FALSE, 0);
    GtkWidget *fb = gtk_font_button_new_with_font((shell->ui_font && shell->ui_font[0]) ? shell->ui_font : "Sans 10");
    g_signal_connect(fb, "font-set", G_CALLBACK(on_ui_font_set), shell);
    GtkWidget *fdef = gtk_button_new_with_label("Default");
    g_object_set_data(G_OBJECT(fdef), "font-btn", fb);
    g_signal_connect(fdef, "clicked", G_CALLBACK(on_ui_font_default), shell);
    gtk_box_pack_end(GTK_BOX(row), fb, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), fdef, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    add_spin_setting_row(vbox, "Corner radius of panel buttons and popups (-1 = theme default, 0 – 24 px):", -1, 24, 1,
                         shell->ui_radius, G_CALLBACK(on_ui_radius_spin), shell);
    GtkWidget *sh = gtk_check_button_new_with_label("Shadow under popups and menus");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sh), shell->ui_popup_shadow);
    g_signal_connect(sh, "toggled", G_CALLBACK(on_ui_shadow_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox), sh, FALSE, FALSE, 0);
    add_spin_setting_row(vbox, "Popup border width (-1 = theme default, 0 – 4 px):", -1, 4, 1,
                         shell->ui_border_w, G_CALLBACK(on_ui_border_w_spin), shell);

    GtkWidget *fl = gtk_check_button_new_with_label("Hide the panel's own frame (removes the edge line, needed for rounded corners)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fl), shell->ui_panel_frameless);
    g_signal_connect(fl, "toggled", G_CALLBACK(on_panel_frameless_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox), fl, FALSE, FALSE, 0);
    GtkWidget *psh = gtk_check_button_new_with_label("Shadow under the panel (needs the compositor)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(psh), shell->ui_panel_shadow);
    g_signal_connect(psh, "toggled", G_CALLBACK(on_panel_shadow_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox), psh, FALSE, FALSE, 0);
    GtkWidget *wfill = gtk_check_button_new_with_label("Let maximized windows fill the screen (terminals no longer stop short of the panel)");
    gtk_widget_set_tooltip_text(wfill, "Terminals ask to be sized in whole text rows, which leaves a gap when maximized. "
                                       "The shell lifts that, so they fill the space; the rest becomes a few pixels of padding.");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wfill), shell->wm_fill_maximized);
    g_signal_connect(wfill, "toggled", G_CALLBACK(on_wm_fill_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox), wfill, FALSE, FALSE, 0);
    add_spin_setting_row(vbox, "Top bar height, macOS layout (22 – 48 px):", 22, 48, 1,
                         shell->mac_panel_size, G_CALLBACK(on_topbar_height_spin), shell);
    add_spin_setting_row(vbox, "Taskbar height, Windows layout (32 – 64 px):", 32, 64, 1,
                         shell->win_panel_size, G_CALLBACK(on_taskbar_height_spin), shell);

}

/* ---- Page builder ---- */
static void settings_add_page(GtkStack *stack, GtkWidget *content, const gchar *id, const gchar *title,
                              gint only_mode, UltimateShell *shell, const gchar *const *reset_prefixes) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *hdr = gtk_label_new(NULL);
    gchar *hm = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", title);
    gtk_label_set_markup(GTK_LABEL(hdr), hm);
    g_free(hm);
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(outer), hdr, FALSE, FALSE, 0);

    if (only_mode != PAGE_ANY && shell && (gint)shell->mode != only_mode) {
        GtkWidget *note = gtk_label_new(only_mode == PAGE_MAC_ONLY
            ? "These settings apply to the macOS layout. You can switch layouts on the General page."
            : "These settings apply to the Windows layout. You can switch layouts on the General page.");
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
        gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(content, FALSE);
    }

    gtk_box_pack_start(GTK_BOX(outer), content, FALSE, FALSE, 0);

    if (reset_prefixes) {
        GtkWidget *reset = gtk_button_new_with_label("Reset This Page");
        gtk_widget_set_halign(reset, GTK_ALIGN_END);
        gtk_widget_set_margin_top(reset, 12);
        g_object_set_data(G_OBJECT(reset), "reset-prefixes", (gpointer)reset_prefixes);
        g_object_set_data_full(G_OBJECT(reset), "page-title", g_strdup(title), g_free);
        g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_page_clicked), shell);
        gtk_box_pack_end(GTK_BOX(outer), reset, FALSE, FALSE, 0);
    }

    gtk_stack_add_titled(stack, wrap_tab_in_scrolled(outer), id, title);
}

/* ---- Search: jump to the first page/setting whose text matches ---- */
static GtkWidget *g_settings_hit = NULL;

static const gchar *widget_search_text(GtkWidget *w) {
    if (GTK_IS_LABEL(w)) return gtk_label_get_text(GTK_LABEL(w));
    if (GTK_IS_BUTTON(w) && !GTK_IS_COMBO_BOX(w)) return gtk_button_get_label(GTK_BUTTON(w));
    return NULL;
}

static GtkWidget *find_text_widget(GtkWidget *w, const gchar *needle_fold) {
    const gchar *t = widget_search_text(w);
    if (t && t[0]) {
        gchar *f = g_utf8_casefold(t, -1);
        gboolean hit = (strstr(f, needle_fold) != NULL);
        g_free(f);
        if (hit) return w;
    }
    if (GTK_IS_CONTAINER(w) && !GTK_IS_COMBO_BOX(w)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        GtkWidget *found = NULL;
        for (GList *l = kids; l && !found; l = l->next) found = find_text_widget(GTK_WIDGET(l->data), needle_fold);
        g_list_free(kids);
        return found;
    }
    return NULL;
}

static void on_settings_search_changed(GtkSearchEntry *entry, GtkStack *stack) {
    if (g_settings_hit) {
        gtk_style_context_remove_class(gtk_widget_get_style_context(g_settings_hit), "ult-settings-hit");
        g_object_remove_weak_pointer(G_OBJECT(g_settings_hit), (gpointer *)&g_settings_hit);
        g_settings_hit = NULL;
    }
    const gchar *q = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!q || strlen(q) < 2) return;
    gchar *nf = g_utf8_casefold(q, -1);
    GList *pages = gtk_container_get_children(GTK_CONTAINER(stack));
    for (GList *l = pages; l; l = l->next) {
        GtkWidget *page = GTK_WIDGET(l->data);
        GtkWidget *hit = find_text_widget(page, nf);
        if (!hit) continue;
        gtk_stack_set_visible_child(stack, page);
        gtk_style_context_add_class(gtk_widget_get_style_context(hit), "ult-settings-hit");
        g_settings_hit = hit;
        g_object_add_weak_pointer(G_OBJECT(hit), (gpointer *)&g_settings_hit);
        /* scroll the page so the match is visible */
        if (GTK_IS_SCROLLED_WINDOW(page)) {
            GtkWidget *child = gtk_bin_get_child(GTK_BIN(page));
            if (GTK_IS_VIEWPORT(child)) child = gtk_bin_get_child(GTK_BIN(child));
            gint x = 0, y = 0;
            if (child && gtk_widget_translate_coordinates(hit, child, 0, 0, &x, &y)) {
                GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page));
                gtk_adjustment_set_value(adj, MAX(0, y - 60));
            }
        }
        break;
    }
    g_list_free(pages);
    g_free(nf);
}

static void settings_ensure_css(void) {
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".ult-settings-hit { background-color: alpha(@theme_selected_bg_color, 0.35); border-radius: 4px; }"
        " stacksidebar row { padding: 6px 10px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(css);
}

/* ---- Notifications page callbacks ---- */
static void on_notif_dnd_set_toggled(GObject *sw, GParamSpec *ps, UltimateShell *shell) {
    if (shell->channel)
        xfconf_channel_set_bool(shell->channel, ULT_PROP_ROOT "/notif_dnd", gtk_switch_get_active(GTK_SWITCH(sw)));
}
static void on_notif_timeout_spin(GtkSpinButton *spin, UltimateShell *shell) {
    if (shell->channel)
        xfconf_channel_set_int(shell->channel, ULT_PROP_ROOT "/notif_timeout_s", gtk_spin_button_get_value_as_int(spin));
}

/* ---- per-page reset prefixes (keys under /plugins/ultimateshell) ---- */
static const gchar *const RESET_GENERAL[] = { "color_mode", "theme_mode", "custom_", "ui_", "anim_", "start_opacity", "dock_opacity", NULL };
static const gchar *const RESET_TOPBAR[]  = { "mac_apple_icon", "mac_panel_size", "winck_", "titlebar_btn_side", NULL };
static const gchar *const RESET_DOCK[]    = { "dock_", NULL };
static const gchar *const RESET_IND[]     = { "ind_", "tb_show_inactive_ind", "dock_ind_", NULL };
static const gchar *const RESET_TASKBAR[] = { "tb_", "show_thumbnails", "show_desktop_enabled", "ind_style", "win_panel_size", "win_layout", NULL };
static const gchar *const RESET_START[]   = { "start_", "win7_", NULL };
static const gchar *const RESET_CLOCK[]   = { "clock_", "cal_style", NULL };
static const gchar *const RESET_TRAY[]    = { "tray_", NULL };
static const gchar *const RESET_SOUND[]   = { "audio_", NULL };
static const gchar *const RESET_NOTIF[]   = { "show_notif_icon", "notif_", NULL };
static const gchar *const RESET_WS[]      = { "ws_", NULL };

/* Let the settings window get narrow: every page only scrolls up/down, so its widest widget used
   to set the window's minimum width. Long drop-down texts shrink with "…" (the open list still
   shows them in full), long labels wrap. */
static void settings_make_shrinkable(GtkWidget *w) {
    if (GTK_IS_COMBO_BOX(w)) {
        GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(w));
        for (GList *l = cells; l; l = l->next)
            if (GTK_IS_CELL_RENDERER_TEXT(l->data))
                g_object_set(l->data, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 8, NULL);
        g_list_free(cells);
        gtk_combo_box_set_popup_fixed_width(GTK_COMBO_BOX(w), FALSE);
        /* a one-column "grid" makes GTK open the list below the button (above if there is no
           room) instead of on top of it - same look, same items */
        if (gtk_combo_box_get_wrap_width(GTK_COMBO_BOX(w)) == 0) gtk_combo_box_set_wrap_width(GTK_COMBO_BOX(w), 1);
    } else if (GTK_IS_LABEL(w)) {
        const gchar *t = gtk_label_get_text(GTK_LABEL(w));
        GtkWidget *p = gtk_widget_get_parent(w);
        gboolean in_combo = p && (GTK_IS_COMBO_BOX(p) || GTK_IS_COMBO_BOX(gtk_widget_get_parent(p)));
        if (!in_combo && t && g_utf8_strlen(t, -1) > 28 &&
            gtk_label_get_ellipsize(GTK_LABEL(w)) == PANGO_ELLIPSIZE_NONE && !gtk_label_get_line_wrap(GTK_LABEL(w))) {
            gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(w), PANGO_WRAP_WORD_CHAR);
            if (gtk_label_get_max_width_chars(GTK_LABEL(w)) < 0) gtk_label_set_max_width_chars(GTK_LABEL(w), 60);
            gtk_label_set_width_chars(GTK_LABEL(w), 12);   /* it may wrap down to this */
        }
    } else if (GTK_IS_SCALE(w)) {
        gint rw = -1, rh = -1;
        gtk_widget_get_size_request(w, &rw, &rh);
        if (rw > 140) gtk_widget_set_size_request(w, 140, rh);   /* sliders a bit narrower at minimum */
    }
    if (GTK_IS_CONTAINER(w) && !GTK_IS_COMBO_BOX(w)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = kids; l; l = l->next) settings_make_shrinkable(l->data);
        g_list_free(kids);
    }
}

static gboolean settings_shrink_later(gpointer stack) {
    if (GTK_IS_WIDGET(stack)) settings_make_shrinkable(GTK_WIDGET(stack));
    return G_SOURCE_REMOVE;
}

static void on_configure_plugin(XfcePanelPlugin *plugin, UltimateShell *shell) {
    if (g_active_prefs_dlg && GTK_IS_WIDGET(g_active_prefs_dlg)) {
        gtk_window_present(GTK_WINDOW(g_active_prefs_dlg));
        return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Ultimate Shell — Control & Customization",
        NULL,
        0,
        "_Close", GTK_RESPONSE_OK,
        NULL
    );
    gtk_window_set_type_hint(GTK_WINDOW(dlg), GDK_WINDOW_TYPE_HINT_NORMAL);
    gtk_window_set_keep_above(GTK_WINDOW(dlg), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dlg), "ultimate-settings-win");
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    g_active_prefs_dlg = dlg;
    g_object_add_weak_pointer(G_OBJECT(dlg), (gpointer *)&g_active_prefs_dlg);

    gint init_w = CLAMP(shell->prefs_w, 460, 1400);
    gint init_h = CLAMP(shell->prefs_h, 440, 1000);
    gtk_window_set_default_size(GTK_WINDOW(dlg), init_w, init_h);
    gtk_window_resize(GTK_WINDOW(dlg), init_w, init_h);
    g_signal_connect(dlg, "configure-event", G_CALLBACK(on_prefs_configure_event), shell);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    settings_ensure_css();

    /* Sidebar (search + page list) on the left, pages on the right */
    GtkWidget *root_h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(side, 200, -1);
    gtk_widget_set_margin_start(side, 8);
    gtk_widget_set_margin_top(side, 8);
    gtk_widget_set_margin_bottom(side, 8);
    gtk_widget_set_margin_end(side, 8);
    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search settings…");
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(stack, TRUE);
    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_widget_set_vexpand(sidebar, TRUE);
    g_signal_connect(search, "search-changed", G_CALLBACK(on_settings_search_changed), stack);
    gtk_box_pack_start(GTK_BOX(side), search, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side), sidebar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root_h), side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_h), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_h), stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), root_h, TRUE, TRUE, 0);

    /* New pages that collect settings from the old tabs */
    GtkWidget *vbox_general = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *vbox_top     = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *vbox_notif   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *vbox_sound   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *vbox_about   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *vbox_ind     = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    /* ---- Indicators: one page for the taskbar and the dock ---- */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Taskbar style (Windows layout):"), FALSE, FALSE, 0);
        GtkWidget *combo_ind = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_ind), "Windows 11 (Rounded Card + Bottom Pill)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_ind), "Windows 10 (Full-Width Bottom Accent Line)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_ind), "KDE Plasma (Top Edge Accent Line + Glow)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_ind), "None");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_ind), CLAMP((gint)shell->ind_style, 0, 3));
        g_signal_connect(combo_ind, "changed", G_CALLBACK(on_ind_style_changed), shell);
        gtk_box_pack_end(GTK_BOX(row), combo_ind, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_ind), row, FALSE, FALSE, 0);

        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Dock style (macOS layout):"), FALSE, FALSE, 0);
        GtkWidget *combo_dk = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dk), "macOS Dot");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dk), "Pill (wider when active)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dk), "One Dot per Window (up to 3)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dk), "Line");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dk), "None");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dk), CLAMP(shell->dock_ind_style, 0, 4));
        g_signal_connect(combo_dk, "changed", G_CALLBACK(on_dock_ind_style_changed), shell);
        gtk_box_pack_end(GTK_BOX(row), combo_dk, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_ind), row, FALSE, FALSE, 0);

        GtkWidget *inact_chk = gtk_check_button_new_with_label("Show the indicator on open apps that are not focused");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inact_chk), shell->tb_show_inactive_ind);
        g_signal_connect(inact_chk, "toggled", G_CALLBACK(on_inactive_ind_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_ind), inact_chk, FALSE, FALSE, 0);

        GtkWidget *urg_chk = gtk_check_button_new_with_label("Pulse orange on apps that need attention");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(urg_chk), shell->ind_urgent_pulse);
        g_signal_connect(urg_chk, "toggled", G_CALLBACK(on_ind_urgent_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_ind), urg_chk, FALSE, FALSE, 0);

        GdkRGBA c_act, c_ina, c_urg;
        shell_indicator_colors(shell, &c_act, &c_ina, &c_urg);
        add_ind_color_row(vbox_ind, "Active app:",      "Use the accent colour",   &shell->ind_color,          &c_act, shell);
        add_ind_color_row(vbox_ind, "Inactive app:",    "Same as active",          &shell->ind_inactive_color, &c_ina, shell);
        add_ind_color_row(vbox_ind, "Needs attention:", "Orange",                  &shell->ind_urgent_color,   &c_urg, shell);
    }

    /* =====================================================================
     * TAB 1: SHELL LAYOUT, TASKBAR & WINDOWCK
     * ===================================================================== */
    GtkWidget *vbox_layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *lbl_preset = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_preset), "<b>Desktop Shell Paradigm</b>");
    gtk_widget_set_halign(lbl_preset, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_general), lbl_preset, FALSE, FALSE, 0);

    GtkWidget *combo_layout = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_layout), "macOS Menu Bar + Bottom Dock Layout");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_layout), "Windows 11 / 10 / Plasma Taskbar Layout");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_layout), "GNOME Top Bar + Bottom Dock Layout");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_layout), shell_layout_combo_index(shell));
    g_signal_connect(combo_layout, "changed", G_CALLBACK(on_layout_combo_changed), shell);
    gtk_box_pack_start(GTK_BOX(vbox_general), combo_layout, FALSE, FALSE, 0);

    GtkWidget *lbl_wtb = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_wtb), "<b>Windows Taskbar Customization</b>");
    gtk_widget_set_halign(lbl_wtb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_layout), lbl_wtb, FALSE, FALSE, 0);

    GtkWidget *align_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(align_hbox), gtk_label_new("Panel Layout:"), FALSE, FALSE, 0);
    GtkWidget *combo_align = gtk_combo_box_text_new();
    /* order = WIN_LAYOUT_* */
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_align), "Classic: Start + Taskbar left · Workspaces + Tray + Clock right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_align), "Workspaces left · Start + Taskbar center · Tray + Clock right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_align), "Start + Workspaces left · Taskbar center · Tray + Clock right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_align), "Windows 11: Start + Taskbar center · Workspaces + Tray + Clock right");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_align), CLAMP(shell->win_layout, 0, 3));
    g_signal_connect(combo_align, "changed", G_CALLBACK(on_win_layout_changed), shell);
    gtk_box_pack_end(GTK_BOX(align_hbox), combo_align, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_layout), align_hbox, FALSE, FALSE, 0);

    GtkWidget *thumb_chk = gtk_check_button_new_with_label("Show live window thumbnails on taskbar hover (Windows layout)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(thumb_chk), shell->show_thumbnails);
    g_signal_connect(thumb_chk, "toggled", G_CALLBACK(on_thumbnails_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_layout), thumb_chk, FALSE, FALSE, 0);

    GtkWidget *mid_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(mid_hbox), gtk_label_new("Middle-Click on Taskbar/Dock Icon:"), FALSE, FALSE, 0);
    GtkWidget *combo_mid = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_mid), "Launch New App Instance");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_mid), "Close App Window");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_mid), shell->tb_middle_click_close ? 1 : 0);
    g_signal_connect(combo_mid, "changed", G_CALLBACK(on_tb_mid_click_changed), shell);
    gtk_box_pack_end(GTK_BOX(mid_hbox), combo_mid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_layout), mid_hbox, FALSE, FALSE, 0);

    add_spin_setting_row(vbox_layout, "Taskbar Icon Size (16 – 48 px):", 16, 48, 2,
                         shell->tb_icon_size, G_CALLBACK(on_tb_icon_size_spin), shell);
    add_spin_setting_row(vbox_layout, "Taskbar Icon Spacing (0 – 20 px):", 0, 20, 1,
                         shell->tb_icon_spacing, G_CALLBACK(on_tb_spacing_spin), shell);
    add_spin_setting_row(vbox_layout, "Taskbar Icon Padding (0 – 14 px):", 0, 14, 1,
                         shell->tb_icon_padding, G_CALLBACK(on_tb_padding_spin), shell);


    GtkWidget *chk_n = gtk_check_button_new_with_label("Show Notification Bell & History Center Icon");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_n), shell->show_notif_icon);
    g_signal_connect(chk_n, "toggled", G_CALLBACK(on_show_notif_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_notif), chk_n, FALSE, FALSE, 0);

    GtkWidget *chk_sd = gtk_check_button_new_with_label("Show 'Show Desktop' Corner Strip (Windows Mode)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_sd), shell->show_desktop_enabled);
    g_signal_connect(chk_sd, "toggled", G_CALLBACK(on_show_desktop_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_layout), chk_sd, FALSE, FALSE, 0);


    /* =====================================================================
     * TAB 2: MACOS DOCK CUSTOMIZATION
     * ===================================================================== */
    GtkWidget *vbox_dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *lbl_dock = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_dock), "<b>macOS Dock Customization</b>");
    gtk_widget_set_halign(lbl_dock, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_dock), lbl_dock, FALSE, FALSE, 0);

    GtkWidget *dstyle_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(dstyle_hbox), gtk_label_new("Dock Style:"), FALSE, FALSE, 0);
    GtkWidget *combo_dstyle = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dstyle), "macOS Dock (floating, magnification)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dstyle), "Ubuntu Dock (left edge, full height, no zoom)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dstyle), shell->dock_style == 1 ? 1 : 0);
    gtk_box_pack_end(GTK_BOX(dstyle_hbox), combo_dstyle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_dock), dstyle_hbox, FALSE, FALSE, 0);

    GtkWidget *dpos_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(dpos_hbox), gtk_label_new("Dock Screen Position:"), FALSE, FALSE, 0);
    GtkWidget *combo_dpos = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dpos), "Bottom of Screen");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dpos), "Left of Screen");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dpos), "Right of Screen");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dpos), (gint)shell->dock_pos);
    g_signal_connect(combo_dpos, "changed", G_CALLBACK(on_dock_pos_changed), shell);
    gtk_box_pack_end(GTK_BOX(dpos_hbox), combo_dpos, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_dock), dpos_hbox, FALSE, FALSE, 0);

    GtkWidget *dhide_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(dhide_hbox), gtk_label_new("Dock Auto-Hide Behavior:"), FALSE, FALSE, 0);
    GtkWidget *combo_dhide = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dhide), "Always Visible (Reserves Screen Space / Never Covered)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dhide), "Intelligent Auto-Hide (Hide When Overlapped)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dhide), "Always Auto-Hide (Show on Edge Hover)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dhide), (gint)shell->dock_autohide);
    g_signal_connect(combo_dhide, "changed", G_CALLBACK(on_dock_hide_changed), shell);
    gtk_box_pack_end(GTK_BOX(dhide_hbox), combo_dhide, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_dock), dhide_hbox, FALSE, FALSE, 0);

    add_spin_setting_row(vbox_dock, "Dock Auto-Hide Delay (1 – 60 sec):", 1, 60, 1,
                         shell->dock_hide_delay_sec, G_CALLBACK(on_dock_hide_delay_spin), shell);

    GtkWidget *dtip_chk = gtk_check_button_new_with_label("Show App Title Pill on Dock Hover");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dtip_chk), shell->dock_show_tooltips);
    g_signal_connect(dtip_chk, "toggled", G_CALLBACK(on_dock_tooltips_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_dock), dtip_chk, FALSE, FALSE, 0);

    add_spin_setting_row(vbox_dock, "Dock Icon Size (24 – 64 px):", 24, 64, 2,
                         shell->dock_icon_size, G_CALLBACK(on_dock_size_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Background Opacity (15 – 100 %):", 15, 100, 5,
                         shell->dock_opacity, G_CALLBACK(on_dock_opacity_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Border Thickness (0 – 8 px):", 0, 8, 1,
                         shell->dock_border_w, G_CALLBACK(on_dock_border_w_spin), shell);

    GtkWidget *dbc_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *dbc_lbl = gtk_label_new("Dock Border Color:");
    gtk_widget_set_halign(dbc_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(dbc_hbox), dbc_lbl, TRUE, TRUE, 0);
    GdkRGBA init_dbc = { 0.22, 0.58, 1.0, 1.0 };
    if (shell->dock_border_color) gdk_rgba_parse(&init_dbc, shell->dock_border_color);
    GtkWidget *dbc_btn = gtk_color_button_new_with_rgba(&init_dbc);
    g_signal_connect(dbc_btn, "color-set", G_CALLBACK(on_dock_border_color_set), shell);
    gtk_box_pack_end(GTK_BOX(dbc_hbox), dbc_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_dock), dbc_hbox, FALSE, FALSE, 0);

    add_spin_setting_row(vbox_dock, "Dock Border Opacity (0 – 100 %):", 0, 100, 5,
                         shell->dock_border_opacity, G_CALLBACK(on_dock_border_opacity_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Rounded Border Radius (0 – 32 px):", 0, 32, 2,
                         shell->dock_radius_val, G_CALLBACK(on_dock_radius_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Horizontal Icon Padding (0 – 32 px):", 0, 32, 2,
                         shell->dock_pad_h, G_CALLBACK(on_dock_pad_h_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Vertical Icon Padding (0 – 20 px):", 0, 20, 1,
                         shell->dock_pad_v, G_CALLBACK(on_dock_pad_v_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Icon Peak Zoom Scale (115 – 200 %):", 115, 200, 5,
                         shell->dock_zoom_scale, G_CALLBACK(on_dock_zoom_scale_spin), shell);
    add_spin_setting_row(vbox_dock, "Dock Distance from Screen Edge (0 – 64 px):", 0, 64, 1,
                         shell->dock_edge_dist, G_CALLBACK(on_dock_edge_dist_spin), shell);

    GtkWidget *dsh_chk = gtk_check_button_new_with_label("Enable Icon & Dock Drop Shadow");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dsh_chk), shell->dock_icon_shadow);
    g_signal_connect(dsh_chk, "toggled", G_CALLBACK(on_dock_icon_shadow_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_dock), dsh_chk, FALSE, FALSE, 0);

    GtkWidget *mag_chk = gtk_check_button_new_with_label("Enable Dock Icon Magnification on Hover");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mag_chk), shell->dock_magnify);
    g_signal_connect(mag_chk, "toggled", G_CALLBACK(on_dock_mag_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_dock), mag_chk, FALSE, FALSE, 0);

    GtkWidget *full_chk = gtk_check_button_new_with_label("Expand Dock to Full Screen Width (Panel Mode)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(full_chk), shell->dock_full_width);
    g_signal_connect(full_chk, "toggled", G_CALLBACK(on_dock_full_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_dock), full_chk, FALSE, FALSE, 0);

    GtkWidget *fwl_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(fwl_hbox), gtk_label_new("Full-Width Dock Icon Layout:"), FALSE, FALSE, 0);
    GtkWidget *combo_fwl = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_fwl), "1. Launchpad + Icons + Trash Centered");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_fwl), "2. Launchpad + Icons Centered, Trash on Right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_fwl), "3. Launchpad on Left, Icons Centered, Trash on Right");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_fwl), "4. Launchpad + Icons on Left, Trash on Right");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_fwl), CLAMP(shell->dock_full_width_layout, 0, 3));
    g_signal_connect(combo_fwl, "changed", G_CALLBACK(on_dock_fw_layout_changed), shell);
    gtk_box_pack_end(GTK_BOX(fwl_hbox), combo_fwl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_dock), fwl_hbox, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(combo_dstyle), "w-pos", combo_dpos);
    g_object_set_data(G_OBJECT(combo_dstyle), "w-mag", mag_chk);
    g_object_set_data(G_OBJECT(combo_dstyle), "w-full", full_chk);
    g_object_set_data(G_OBJECT(combo_dstyle), "w-fwl", fwl_hbox);
    dock_style_sync_controls(combo_dstyle, shell);
    g_signal_connect(combo_dstyle, "changed", G_CALLBACK(on_dock_style_changed), shell);

    create_icon_picker_row(vbox_top,
                           "Custom Top-Bar System Menu Icon:",
                           "Default: your distro's logo",
                           shell->mac_apple_icon,
                           "distributor-logo",
                           apply_mac_apple_icon_cb,
                           shell);
    add_spin_setting_row(vbox_top, "Top-Bar System Menu Icon Size (12 – 36 px):", 12, 36, 1, shell->mac_apple_icon_size > 0 ? shell->mac_apple_icon_size : 18, G_CALLBACK(on_mac_apple_icon_size_spin), shell);

    create_icon_picker_row(vbox_dock,
                           "Custom Launchpad Dock Icon:",
                           "applications-other",
                           shell->dock_launchpad_icon,
                           "applications-other",
                           apply_dock_launchpad_icon_cb,
                           shell);

    GtkWidget *trash_chk = gtk_check_button_new_with_label("Show Trash Bin Icon on Dock");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(trash_chk), shell->dock_show_trash);
    g_signal_connect(trash_chk, "toggled", G_CALLBACK(on_dock_trash_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_dock), trash_chk, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox_top), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);

    GtkWidget *lbl_wck = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_wck), "<b>Window Buttons (– + ×) on the Top Bar</b>");
    gtk_widget_set_halign(lbl_wck, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_top), lbl_wck, FALSE, FALSE, 0);

    GtkWidget *chk_wck = gtk_check_button_new_with_label("Enable Windowck Buttons on Top Bar (macOS Mode)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_wck), shell->winck_enabled);
    g_signal_connect(chk_wck, "toggled", G_CALLBACK(on_winck_enabled_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_top), chk_wck, FALSE, FALSE, 0);

    GtkWidget *wpos_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(wpos_hbox), gtk_label_new("Window Buttons Side:"), FALSE, FALSE, 0);
    GtkWidget *combo_wpos = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_wpos), "Left (macOS) · after the Apple menu");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_wpos), "Right (Windows) · far right of the top bar");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_wpos), (shell->winck_pos == 1) ? 1 : 0);
    g_signal_connect(combo_wpos, "changed", G_CALLBACK(on_winck_pos_changed), shell);
    gtk_box_pack_end(GTK_BOX(wpos_hbox), combo_wpos, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_top), wpos_hbox, FALSE, FALSE, 0);

    GtkWidget *chk_tb = gtk_check_button_new_with_label("Also move every window's title-bar buttons to the same side");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_tb), shell->titlebar_btn_side != 3);
    gtk_widget_set_tooltip_text(chk_tb, "Off: your own XFCE window-button setting is restored and left alone.");
    g_signal_connect(chk_tb, "toggled", G_CALLBACK(on_titlebar_follow_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_top), chk_tb, FALSE, FALSE, 0);

    add_spin_setting_row(vbox_top, "Windowck Button Size (12 – 32 px):", 12, 32, 1,
                         shell->winck_btn_size, G_CALLBACK(on_winck_size_spin), shell);
    add_spin_setting_row(vbox_top, "Windowck Horizontal Button Padding (0 – 16 px):", 0, 16, 1,
                         shell->winck_btn_pad, G_CALLBACK(on_winck_pad_spin), shell);


    /* =====================================================================
     * TAB 3: START MENU & LAUNCHPAD CUSTOMIZATION (DYNAMIC PER-STYLE)
     * ===================================================================== */
    GtkWidget *vbox_sm = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    StartTabUI *sm_ui = g_new0(StartTabUI, 1);
    sm_ui->shell = shell;
    g_start_tab_ui = sm_ui;
    g_object_set_data_full(G_OBJECT(vbox_sm), "sm-tab-ui", sm_ui, g_free);
    g_object_add_weak_pointer(G_OBJECT(vbox_sm), (gpointer *)&g_start_tab_ui);

    GtkWidget *lbl_sm = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_sm), "<b>Start Menu / Application Launcher Style</b>");
    gtk_widget_set_halign(lbl_sm, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_sm), lbl_sm, FALSE, FALSE, 0);

    {
        GtkWidget *sk = gtk_check_button_new_with_label("Super (Windows) key opens the Start menu");
        gtk_widget_set_tooltip_text(sk, "Binds Super to the start menu in Keyboard Shortcuts. A previous Super binding is restored when this is off.");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sk), shell->super_key_start);
        g_signal_connect(sk, "toggled", G_CALLBACK(on_super_key_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_sm), sk, FALSE, FALSE, 0);
    }

    GtkWidget *combo_sm = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "List");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "Classic");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "Modern");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "Categories");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "Launchpad");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "Tiles");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sm), "App Grid");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_sm), (gint)shell->start_style);
    g_signal_connect(combo_sm, "changed", G_CALLBACK(on_start_style_changed), shell);
    gtk_box_pack_start(GTK_BOX(vbox_sm), combo_sm, FALSE, FALSE, 0);
    {
        GtkWidget *blurb = gtk_label_new(start_style_blurb(shell->start_style));
        gtk_label_set_xalign(GTK_LABEL(blurb), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(blurb), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(blurb), 60);
        gtk_style_context_add_class(gtk_widget_get_style_context(blurb), "dim-label");
        g_object_set_data(G_OBJECT(combo_sm), "style-blurb", blurb);
        gtk_box_pack_start(GTK_BOX(vbox_sm), blurb, FALSE, FALSE, 0);
    }

    GtkWidget *sbm_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(sbm_hbox), gtk_label_new("Start Button Display (Windows Mode):"), FALSE, FALSE, 0);
    GtkWidget *combo_sbm = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sbm), "Icon + Label");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sbm), "Icon Only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sbm), "Label Only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_sbm), (gint)shell->start_btn_mode);
    g_signal_connect(combo_sbm, "changed", G_CALLBACK(on_start_btn_mode_changed), shell);
    gtk_box_pack_end(GTK_BOX(sbm_hbox), combo_sbm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_sm), sbm_hbox, FALSE, FALSE, 0);

    GtkWidget *sbl_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(sbl_hbox), gtk_label_new("Start Button Text Label:"), FALSE, FALSE, 0);
    GtkWidget *entry_sbl = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_sbl), shell->start_btn_label ? shell->start_btn_label : "Start");
    g_signal_connect(entry_sbl, "changed", G_CALLBACK(on_start_btn_label_changed), shell);
    gtk_box_pack_end(GTK_BOX(sbl_hbox), entry_sbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_sm), sbl_hbox, FALSE, FALSE, 0);

    create_icon_picker_row(vbox_sm,
                           "Custom Start Button Icon (Windows Mode):",
                           "open-menu-symbolic",
                           shell->start_btn_icon,
                           "open-menu-symbolic",
                           apply_win_start_icon_cb,
                           shell);
    add_spin_setting_row(vbox_sm, "Start Button Icon Size (Windows Mode, 14 – 48 px):", 14, 48, 1, shell->start_btn_icon_size > 0 ? shell->start_btn_icon_size : 24, G_CALLBACK(on_start_btn_icon_size_spin), shell);

    add_spin_setting_row(vbox_sm, "Start Menu Background Opacity (20 – 100 %):", 20, 100, 5,
                         shell->start_opacity, G_CALLBACK(on_start_opacity_spin), shell);

    gtk_box_pack_start(GTK_BOX(vbox_sm), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    GtkWidget *lbl_st_opts = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_st_opts), "<b>Selected Style Layout &amp; Sizing Options</b>");
    gtk_widget_set_halign(lbl_st_opts, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_sm), lbl_st_opts, FALSE, FALSE, 0);
    GtkWidget *size_note = gtk_label_new(
        "Resize the menu by dragging any of its edges or the ╬ corner; the size is saved for each style.\n"
        "The Fullscreen style fills the screen but leaves your panel visible, so it can't be resized.");
    gtk_label_set_line_wrap(GTK_LABEL(size_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(size_note), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(size_note), "dim-label");
    gtk_box_pack_start(GTK_BOX(vbox_sm), size_note, FALSE, FALSE, 0);

    gint cur_st = CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);

    sm_ui->row_list_icon = create_tracked_spin_row(
        vbox_sm, "Application List Icon Size (16 – 48 px):", 16, 48, 2,
        shell->start_style_list_icon[cur_st], G_CALLBACK(on_start_list_icon_spin), shell, &sm_ui->spin_list_icon);

    sm_ui->row_grid_icon = create_tracked_spin_row(
        vbox_sm, "Pinned / Grid Icon Size (24 – 72 px):", 24, 72, 2,
        shell->start_style_grid_icon[cur_st], G_CALLBACK(on_start_grid_icon_spin), shell, &sm_ui->spin_grid_icon);

    sm_ui->row_grid_cols = create_tracked_spin_row(
        vbox_sm, "Grid Columns (3 – 10 columns):", 3, 10, 1,
        shell->start_style_grid_cols[cur_st], G_CALLBACK(on_start_grid_cols_spin), shell, &sm_ui->spin_grid_cols);

    sm_ui->row_fs_rows = create_tracked_spin_row(
        vbox_sm, "Launchpad Fullscreen Rows (2 – 8 rows):", 2, 8, 1,
        shell->start_style_fs_rows, G_CALLBACK(on_start_fs_rows_spin), shell, &sm_ui->spin_fs_rows);

    sm_ui->row_label_pt = create_tracked_spin_row(
        vbox_sm, "Application Label Font Size (8 – 16 pt):", 8, 16, 1,
        shell->start_style_label_pt[cur_st], G_CALLBACK(on_start_label_pt_spin), shell, &sm_ui->spin_label_pt);

    sm_ui->row_search_pos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(sm_ui->row_search_pos), gtk_label_new("Search Bar Position:"), FALSE, FALSE, 0);
    sm_ui->combo_search_pos = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_search_pos), "Default for Style");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_search_pos), "Top of Menu");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_search_pos), "Bottom of Menu");
    gtk_combo_box_set_active(GTK_COMBO_BOX(sm_ui->combo_search_pos), (gint)shell->start_style_search_pos[cur_st]);
    g_signal_connect(sm_ui->combo_search_pos, "changed", G_CALLBACK(on_start_search_pos_changed), shell);
    gtk_box_pack_end(GTK_BOX(sm_ui->row_search_pos), sm_ui->combo_search_pos, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_sm), sm_ui->row_search_pos, FALSE, FALSE, 0);

    sm_ui->chk_show_desc = gtk_check_button_new_with_label("Show Application Descriptions Under App Names");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sm_ui->chk_show_desc), shell->start_style_show_desc[cur_st]);
    g_signal_connect(sm_ui->chk_show_desc, "toggled", G_CALLBACK(on_start_show_desc_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_sm), sm_ui->chk_show_desc, FALSE, FALSE, 0);

    sm_ui->chk_win10_tiles = gtk_check_button_new_with_label("Show pinned tiles (Windows 10 menu)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sm_ui->chk_win10_tiles), shell->win10_show_tiles);
    gtk_widget_set_tooltip_text(sm_ui->chk_win10_tiles, "Off: just the A–Z list, a narrow menu like Windows 10 with no tiles pinned.");
    g_signal_connect(sm_ui->chk_win10_tiles, "toggled", G_CALLBACK(on_win10_tiles_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_sm), sm_ui->chk_win10_tiles, FALSE, FALSE, 0);

    /* Category Sidebar / Filter Bar Group (Only for Whisker & Fullscreen Launchpad) */
    sm_ui->box_cat_group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_cat_group), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    GtkWidget *lbl_cat_grp = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_cat_grp), "<b>Category Sidebar / Filter Bar Options</b>");
    gtk_widget_set_halign(lbl_cat_grp, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_cat_group), lbl_cat_grp, FALSE, FALSE, 0);

    GtkWidget *cat_m_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cat_m_hbox), gtk_label_new("Category Button Display Mode:"), FALSE, FALSE, 0);
    sm_ui->combo_cat_mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_cat_mode), "Icon + Label");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_cat_mode), "Icon Only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_cat_mode), "Label Only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(sm_ui->combo_cat_mode), (gint)shell->start_style_cat_mode[cur_st]);
    g_signal_connect(sm_ui->combo_cat_mode, "changed", G_CALLBACK(on_start_cat_mode_changed), shell);
    gtk_box_pack_end(GTK_BOX(cat_m_hbox), sm_ui->combo_cat_mode, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_cat_group), cat_m_hbox, FALSE, FALSE, 0);

    create_tracked_spin_row(sm_ui->box_cat_group, "Category Icon Size (14 – 36 px):", 14, 36, 2,
                            shell->start_style_cat_icon[cur_st], G_CALLBACK(on_start_cat_icon_spin), shell, &sm_ui->spin_cat_icon);
    create_tracked_spin_row(sm_ui->box_cat_group, "Category Font Size (8 – 16 pt):", 8, 16, 1,
                            shell->start_style_cat_pt[cur_st], G_CALLBACK(on_start_cat_pt_spin), shell, &sm_ui->spin_cat_pt);
    GtkWidget *cat_w_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cat_w_hbox), gtk_label_new("Category Font Weight:"), FALSE, FALSE, 0);
    sm_ui->combo_cat_weight = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_cat_weight), "Regular");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_cat_weight), "Medium");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sm_ui->combo_cat_weight), "Bold");
    gint init_cw = shell->start_style_cat_weight[cur_st];
    gtk_combo_box_set_active(GTK_COMBO_BOX(sm_ui->combo_cat_weight), (init_cw >= 700) ? 2 : ((init_cw >= 500) ? 1 : 0));
    g_signal_connect(sm_ui->combo_cat_weight, "changed", G_CALLBACK(on_start_cat_weight_changed), shell);
    gtk_box_pack_end(GTK_BOX(cat_w_hbox), sm_ui->combo_cat_weight, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_cat_group), cat_w_hbox, FALSE, FALSE, 0);
    create_tracked_spin_row(sm_ui->box_cat_group, "Category Item Spacing (0 – 14 px):", 0, 14, 1,
                            shell->start_style_cat_spacing[cur_st], G_CALLBACK(on_start_cat_spacing_spin), shell, &sm_ui->spin_cat_spacing);

    sm_ui->chk_hover_cat = gtk_check_button_new_with_label("Switch Categories Automatically on Mouse Hover");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sm_ui->chk_hover_cat), shell->start_style_hover_cat[cur_st]);
    g_signal_connect(sm_ui->chk_hover_cat, "toggled", G_CALLBACK(on_start_hover_cat_toggled), shell);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_cat_group), sm_ui->chk_hover_cat, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_sm), sm_ui->box_cat_group, FALSE, FALSE, 0);

    /* Windows 7 Right-Pane Shortcuts Group (Only for Windows 7 Style) */
    sm_ui->box_win7_group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_win7_group), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    GtkWidget *lbl_w7 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_w7), "<b>Windows 7 Right-Pane Places &amp; System Shortcuts</b>");
    gtk_widget_set_halign(lbl_w7, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_win7_group), lbl_w7, FALSE, FALSE, 0);

    GtkWidget *w7_wt_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(w7_wt_hbox), gtk_label_new("Shortcuts / Places Font Weight:"), FALSE, FALSE, 0);
    GtkWidget *combo_w7_wt = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_w7_wt), "Regular");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_w7_wt), "Medium");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_w7_wt), "Bold");
    gint init_pw = shell->win7_places_weight;
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_w7_wt), (init_pw >= 700) ? 2 : ((init_pw >= 500) ? 1 : 0));
    g_signal_connect(combo_w7_wt, "changed", G_CALLBACK(on_win7_places_weight_changed), shell);
    gtk_box_pack_end(GTK_BOX(w7_wt_hbox), combo_w7_wt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_win7_group), w7_wt_hbox, FALSE, FALSE, 2);

    struct { const gchar *id; const gchar *lbl; gboolean val; } w7_items[] = {
        { "pic",  "Show Pictures Folder Shortcut", shell->win7_show_pictures },
        { "mus",  "Show Music Folder Shortcut",    shell->win7_show_music },
        { "vid",  "Show Videos Folder Shortcut",   shell->win7_show_videos },
        { "trsh", "Show Trash Bin Shortcut",       shell->win7_show_trash },
        { "term", "Show Terminal Shortcut",        shell->win7_show_terminal }
    };
    for (gsize i = 0; i < G_N_ELEMENTS(w7_items); i++) {
        GtkWidget *c = gtk_check_button_new_with_label(w7_items[i].lbl);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), w7_items[i].val);
        g_object_set_data(G_OBJECT(c), "win7-id", (gpointer)w7_items[i].id);
        g_signal_connect(c, "toggled", G_CALLBACK(on_win7_shortcut_toggled), shell);
        gtk_box_pack_start(GTK_BOX(sm_ui->box_win7_group), c, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox_sm), sm_ui->box_win7_group, FALSE, FALSE, 0);

    /* Shared Behavior & Footer Power Buttons */
    gtk_box_pack_start(GTK_BOX(vbox_sm), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    GtkWidget *gn_chk = gtk_check_button_new_with_label("Show Generic Application Names Instead of Package Names");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gn_chk), shell->start_generic_names);
    g_signal_connect(gn_chk, "toggled", G_CALLBACK(on_start_generic_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_sm), gn_chk, FALSE, FALSE, 0);

    {   /* Recent & Recommended (optional) */
        GtkWidget *wrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(wrow), gtk_label_new("Whisker Menu: show applications as"), FALSE, FALSE, 0);
        GtkWidget *wcb = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wcb), "List");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wcb), "Icons (grid)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(wcb), shell->start_whisker_grid ? 1 : 0);
        g_signal_connect(wcb, "changed", G_CALLBACK(on_start_whisker_grid_changed), shell);
        gtk_box_pack_end(GTK_BOX(wrow), wcb, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_sm), wrow, FALSE, FALSE, 0);

        GtkWidget *rec_chk = gtk_check_button_new_with_label("Show Recent & Recommended (most used apps; new apps are always shown)");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rec_chk), shell->start_show_recommended);
        GtkWidget *files_chk = gtk_check_button_new_with_label("Include recent files");
        gtk_widget_set_margin_start(files_chk, 24);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(files_chk), shell->start_recommend_files);
        gtk_widget_set_sensitive(files_chk, shell->start_show_recommended);
        g_object_set_data(G_OBJECT(rec_chk), "files-chk", files_chk);
        g_signal_connect(rec_chk, "toggled", G_CALLBACK(on_start_recommended_toggled), shell);
        g_signal_connect(files_chk, "toggled", G_CALLBACK(on_start_rec_files_toggled), shell);
        GtkWidget *clr = gtk_button_new_with_label("Clear App History");
        gtk_widget_set_tooltip_text(clr, "Forget how often apps were opened (used for \"Most used\")");
        gtk_widget_set_halign(clr, GTK_ALIGN_START);
        gtk_widget_set_margin_start(clr, 24);
        g_signal_connect(clr, "clicked", G_CALLBACK(on_start_clear_history_clicked), shell);
        gtk_box_pack_start(GTK_BOX(vbox_sm), rec_chk, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_sm), files_chk, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_sm), clr, FALSE, FALSE, 0);
    }

    GtkWidget *hfx_chk = gtk_check_button_new_with_label("Enable Highlight Hover Effect on Menu Items");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hfx_chk), shell->start_hover_effect);
    g_signal_connect(hfx_chk, "toggled", G_CALLBACK(on_start_hover_fx_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_sm), hfx_chk, FALSE, FALSE, 0);

    sm_ui->box_footer_group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *uh_chk = gtk_check_button_new_with_label("Show User Avatar & Profile Header in Start Menu");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(uh_chk), shell->start_show_user_header);
    g_signal_connect(uh_chk, "toggled", G_CALLBACK(on_start_user_hdr_toggled), shell);
    gtk_box_pack_start(GTK_BOX(sm_ui->box_footer_group), uh_chk, FALSE, FALSE, 0);

    struct { const gchar *id; const gchar *lbl; gboolean val; } pwr_items[] = {
        { "lock", "Show Lock Screen Button in Footer", shell->start_show_lock },
        { "out",  "Show Log Out Button in Footer",     shell->start_show_logout },
        { "reb",  "Show Restart Button in Footer",     shell->start_show_restart },
        { "shut", "Show Shut Down Button in Footer",   shell->start_show_shutdown }
    };
    for (gsize i = 0; i < G_N_ELEMENTS(pwr_items); i++) {
        GtkWidget *c = gtk_check_button_new_with_label(pwr_items[i].lbl);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), pwr_items[i].val);
        g_object_set_data(G_OBJECT(c), "pwr-id", (gpointer)pwr_items[i].id);
        g_signal_connect(c, "toggled", G_CALLBACK(on_start_footer_btn_toggled), shell);
        gtk_box_pack_start(GTK_BOX(sm_ui->box_footer_group), c, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox_sm), sm_ui->box_footer_group, FALSE, FALSE, 0);


    /* =====================================================================
     * TAB 4: CLOCK & CALENDAR CUSTOMIZATION
     * ===================================================================== */
    GtkWidget *vbox_clk = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *lbl_clk = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_clk), "<b>Clock &amp; Calendar Flyout Customization</b>");
    gtk_widget_set_halign(lbl_clk, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_clk), lbl_clk, FALSE, FALSE, 0);

    GtkWidget *cal_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cal_hbox), gtk_label_new("Calendar Flyout Style:"), FALSE, FALSE, 0);
    GtkWidget *combo_cal = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cal), "Windows 7 (calendar + analog clock)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cal), "Windows 10 (large digital clock + calendar)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cal), "Windows 11 (compact calendar card)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cal), "GNOME / macOS (notifications + calendar)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_cal), (gint)shell->cal_style);
    g_signal_connect(combo_cal, "changed", G_CALLBACK(on_cal_style_changed), shell);
    gtk_box_pack_end(GTK_BOX(cal_hbox), combo_cal, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_clk), cal_hbox, FALSE, FALSE, 0);

    GtkWidget *cfmt_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cfmt_hbox), gtk_label_new("Panel Clock Layout:"), FALSE, FALSE, 0);
    GtkWidget *combo_cfmt = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cfmt), "Time on Top, Date Below");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cfmt), "Date on Top, Time Below");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cfmt), "Single Line: Time · Date");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_cfmt), (gint)shell->clock_format);
    g_signal_connect(combo_cfmt, "changed", G_CALLBACK(on_clock_fmt_changed), shell);
    gtk_box_pack_end(GTK_BOX(cfmt_hbox), combo_cfmt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_clk), cfmt_hbox, FALSE, FALSE, 0);

    GtkWidget *c24_chk = gtk_check_button_new_with_label("Use 24-Hour Military Time Format");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c24_chk), shell->clock_24h);
    g_signal_connect(c24_chk, "toggled", G_CALLBACK(on_clock_24h_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_clk), c24_chk, FALSE, FALSE, 0);

    GtkWidget *csec_chk = gtk_check_button_new_with_label("Show Seconds in Panel Clock");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(csec_chk), shell->clock_show_seconds);
    g_signal_connect(csec_chk, "toggled", G_CALLBACK(on_clock_sec_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_clk), csec_chk, FALSE, FALSE, 0);

    {
        GtkWidget *dr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(dr), gtk_label_new("Date Format:"), FALSE, FALSE, 0);
        GtkWidget *dc = gtk_combo_box_text_new();
        /* order = clock-widget.c date_fmt_idx_to_strftime */
        const gchar *names[] = { "2026-11-16", "11/16/2026", "16/11/2026", "Mon, Nov 16", "Nov 16, 2026",
                                 "Your language's default", "16 Nov 2026", "Custom…", NULL };
        for (gint i = 0; names[i]; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dc), names[i]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(dc), CLAMP(shell->clock_date_fmt, 0, 7));
        GtkWidget *ce = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(ce), shell->clock_date_custom ? shell->clock_date_custom : "");
        gtk_entry_set_width_chars(GTK_ENTRY(ce), 12);
        gtk_widget_set_tooltip_text(ce, "strftime codes, e.g. %a %-d %b  →  Mon 16 Nov");
        gtk_widget_set_sensitive(ce, shell->clock_date_fmt == 7);
        g_object_set_data(G_OBJECT(dc), "custom-entry", ce);
        g_signal_connect(dc, "changed", G_CALLBACK(on_clock_date_fmt_changed), shell);
        g_signal_connect(ce, "changed", G_CALLBACK(on_clock_date_custom_changed), shell);
        gtk_box_pack_end(GTK_BOX(dr), ce, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(dr), dc, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_clk), dr, FALSE, FALSE, 0);

        add_spin_setting_row(vbox_clk, "Space Between Time and Date (-8 – 12 px):", -8, 12, 1,
                             shell->clock_line_spacing, G_CALLBACK(on_clock_spacing_spin), shell);

        GtkWidget *md = gtk_check_button_new_with_label("macOS layout: show the weekday (Mon)");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(md), shell->clock_mac_show_day);
        g_object_set_data(G_OBJECT(md), "clk-prop", "day");
        g_signal_connect(md, "toggled", G_CALLBACK(on_clock_bool_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_clk), md, FALSE, FALSE, 0);
        GtkWidget *mdt = gtk_check_button_new_with_label("macOS layout: show the date (16 Nov)");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mdt), shell->clock_mac_show_date);
        g_object_set_data(G_OBJECT(mdt), "clk-prop", "date");
        g_signal_connect(mdt, "toggled", G_CALLBACK(on_clock_bool_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_clk), mdt, FALSE, FALSE, 0);
        GtkWidget *wk = gtk_check_button_new_with_label("Show week numbers in the calendar");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wk), shell->cal_week_numbers);
        g_object_set_data(G_OBJECT(wk), "clk-prop", "weeks");
        g_signal_connect(wk, "toggled", G_CALLBACK(on_clock_bool_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_clk), wk, FALSE, FALSE, 0);
    }

    add_spin_setting_row(vbox_clk, "Clock Time Font Size (8 – 18 pt):", 8, 18, 1,
                         shell->clock_time_pt, G_CALLBACK(on_clock_time_pt_spin), shell);
    add_spin_setting_row(vbox_clk, "Clock Date Font Size (7 – 16 pt):", 7, 16, 1,
                         shell->clock_date_pt, G_CALLBACK(on_clock_date_pt_spin), shell);

    GtkWidget *tw_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(tw_hbox), gtk_label_new("Clock Time Font Weight:"), FALSE, FALSE, 0);
    GtkWidget *combo_tw = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_tw), "Regular");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_tw), "Medium");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_tw), "Bold");
    gint init_tw = shell->clock_time_weight;
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_tw), (init_tw >= 700) ? 2 : ((init_tw >= 500) ? 1 : 0));
    g_signal_connect(combo_tw, "changed", G_CALLBACK(on_clock_time_weight_changed), shell);
    gtk_box_pack_end(GTK_BOX(tw_hbox), combo_tw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_clk), tw_hbox, FALSE, FALSE, 0);

    GtkWidget *dw_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(dw_hbox), gtk_label_new("Clock Date Font Weight:"), FALSE, FALSE, 0);
    GtkWidget *combo_dw = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dw), "Regular");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dw), "Medium");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dw), "Bold");
    gint init_dw = shell->clock_date_weight;
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dw), (init_dw >= 700) ? 2 : ((init_dw >= 500) ? 1 : 0));
    g_signal_connect(combo_dw, "changed", G_CALLBACK(on_clock_date_weight_changed), shell);
    gtk_box_pack_end(GTK_BOX(dw_hbox), combo_dw, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_clk), dw_hbox, FALSE, FALSE, 0);


    /* =====================================================================
     * TAB 5: SYSTEM TRAY & AUDIO
     * ===================================================================== */
    GtkWidget *vbox_tray = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *lbl_tray = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_tray), "<b>System Tray Items</b>");
    gtk_widget_set_halign(lbl_tray, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_tray), lbl_tray, FALSE, FALSE, 0);

    GtkWidget *t_all = gtk_check_button_new_with_label("Always Show All System Tray Icons (Disable Overflow Chevron)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_all), shell->tray_show_all);
    g_signal_connect(t_all, "toggled", G_CALLBACK(on_tray_show_all_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_all, FALSE, FALSE, 0);

    GtkWidget *t_net = gtk_check_button_new_with_label("Enable Built-in Network Status Indicator");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_net), shell->tray_enable_network);
    g_signal_connect(t_net, "toggled", G_CALLBACK(on_tray_net_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_net, FALSE, FALSE, 0);

    GtkWidget *t_aud = gtk_check_button_new_with_label("Enable Built-in PulseAudio Volume & Media Flyout");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_aud), shell->tray_enable_audio);
    g_signal_connect(t_aud, "toggled", G_CALLBACK(on_tray_aud_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_aud, FALSE, FALSE, 0);

    GtkWidget *t_bat = gtk_check_button_new_with_label("Enable Built-in Battery & Power Indicator");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_bat), shell->tray_enable_battery);
    g_signal_connect(t_bat, "toggled", G_CALLBACK(on_tray_bat_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_bat, FALSE, FALSE, 0);

    GtkWidget *t_bat_always = gtk_check_button_new_with_label("Show Battery Icon Even Without a Battery (desktops)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_bat_always),
        shell->channel ? xfconf_channel_get_bool(shell->channel, "/plugins/ultimateshell/tray_battery_always_show", FALSE) : FALSE);
    g_signal_connect(t_bat_always, "toggled", G_CALLBACK(on_tray_bat_always_toggled), shell);
    gtk_widget_set_margin_start(t_bat_always, 24);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_bat_always, FALSE, FALSE, 0);

    GtkWidget *t_clip = gtk_check_button_new_with_label("Enable Built-in Clipboard History Indicator");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_clip), shell->tray_enable_clipboard);
    g_signal_connect(t_clip, "toggled", G_CALLBACK(on_tray_clip_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_clip, FALSE, FALSE, 0);

    GtkWidget *t_rem = gtk_check_button_new_with_label("Enable Built-in Removable Drives & USB Eject Indicator");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_rem), shell->tray_enable_removable);
    g_signal_connect(t_rem, "toggled", G_CALLBACK(on_tray_rem_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_rem, FALSE, FALSE, 0);

    GtkWidget *t_bt = gtk_check_button_new_with_label("Enable Built-in Bluetooth Manager Indicator");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_bt), shell->tray_enable_bluetooth);
    g_signal_connect(t_bt, "toggled", G_CALLBACK(on_tray_bt_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_bt, FALSE, FALSE, 0);

    add_spin_setting_row(vbox_tray, "System Tray Icon Size (14 – 32 px):", 14, 32, 2,
                         shell->tray_icon_size, G_CALLBACK(on_tray_icon_size_spin), shell);
    add_spin_setting_row(vbox_tray, "System Tray Icon Spacing (0 – 16 px):", 0, 16, 1,
                         shell->tray_icon_spacing, G_CALLBACK(on_tray_spacing_spin), shell);

    GtkWidget *t_hide_def = gtk_check_button_new_with_label("Hide new status tray items by default (Place in Overflow Menu)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_hide_def), shell->tray_hide_new_default);
    g_signal_connect(t_hide_def, "toggled", G_CALLBACK(on_tray_hide_new_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_hide_def, FALSE, FALSE, 0);

    GtkWidget *t_ibus = gtk_check_button_new_with_label("Use the modern tray icon for IBus (can be moved to the overflow menu)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_ibus), shell->tray_ibus_modern);
    gtk_widget_set_tooltip_text(t_ibus, "Restarts the IBus input-method service once. Off: IBus uses the old embedded "
                                        "tray icon, which has to stay on the panel.");
    g_signal_connect(t_ibus, "toggled", G_CALLBACK(on_tray_ibus_modern_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_ibus, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox_tray), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    GtkWidget *lbl_feat = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_feat), "<b>Features</b>");
    gtk_widget_set_halign(lbl_feat, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox_tray), lbl_feat, FALSE, FALSE, 0);

    {
        GtkWidget *qs = gtk_check_button_new_with_label("Quick Settings (Sound, Network, Bluetooth and Power open one panel)");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(qs), shell->tray_quick_settings);
        g_signal_connect(qs, "toggled", G_CALLBACK(on_tray_quick_settings_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_tray), qs, FALSE, FALSE, 0);
    }

    GtkWidget *t_mp = gtk_check_button_new_with_label("Menu is primary action (Left-click opens menu, Right-click hides/moves icon)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_mp), shell->tray_menu_is_primary);
    g_signal_connect(t_mp, "toggled", G_CALLBACK(on_tray_menu_primary_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_mp, FALSE, FALSE, 0);

    GtkWidget *t_sym = gtk_check_button_new_with_label("Request symbolic icons (Use monochrome symbolic icons when available)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t_sym), shell->tray_request_symbolic);
    g_signal_connect(t_sym, "toggled", G_CALLBACK(on_tray_symbolic_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_tray), t_sym, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox_tray), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    systray_populate_settings_box(shell, vbox_tray);


    /* =====================================================================
     * TAB 6: APPEARANCE & THEME ENGINE
     * ===================================================================== */


    /* ---------- General = layout + appearance ---------- */
    gtk_box_pack_start(GTK_BOX(vbox_general), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    build_general_appearance(vbox_general, shell);

    /* ---------- Top Bar: global menu status ---------- */
    {
        gtk_box_pack_start(GTK_BOX(vbox_top), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
        GtkWidget *gm_hdr = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(gm_hdr), "<b>Global Menu</b>");
        gtk_widget_set_halign(gm_hdr, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vbox_top), gm_hdr, FALSE, FALSE, 0);
        gboolean helper_ok = appmenu_module_installed();
        GtkWidget *gm_info = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(gm_info), helper_ok
            ? "✔ Menu helper for GTK apps is installed.\n"
              "Keyboard: <b>Ctrl+Alt+M</b> opens the menu bar · <b>Help ▸ Search Menus…</b> finds any item."
            : "✘ Menu helper for GTK apps is <b>not installed</b>, so GTK apps (Thunar, Mousepad…) can't show their menus.\n"
              "Install it, then log out and back in:  <tt>sudo apt install appmenu-gtk3-module appmenu-gtk2-module</tt>");
        gtk_label_set_line_wrap(GTK_LABEL(gm_info), TRUE);
        gtk_label_set_selectable(GTK_LABEL(gm_info), TRUE);
        gtk_label_set_xalign(GTK_LABEL(gm_info), 0.0);
        gtk_box_pack_start(GTK_BOX(vbox_top), gm_info, FALSE, FALSE, 0);
    }

    /* ---------- Notifications ---------- */
    {
        GtkWidget *dnd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *dnd_lbl = gtk_label_new("Do Not Disturb (hide pop-ups, keep them in the list):");
        gtk_widget_set_halign(dnd_lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(dnd_row), dnd_lbl, TRUE, TRUE, 0);
        GtkWidget *dnd_sw = gtk_switch_new();
        gtk_widget_set_valign(dnd_sw, GTK_ALIGN_CENTER);
        gtk_switch_set_active(GTK_SWITCH(dnd_sw), shell->channel &&
            xfconf_channel_get_bool(shell->channel, ULT_PROP_ROOT "/notif_dnd", FALSE));
        g_signal_connect(dnd_sw, "notify::active", G_CALLBACK(on_notif_dnd_set_toggled), shell);
        gtk_box_pack_end(GTK_BOX(dnd_row), dnd_sw, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_notif), dnd_row, FALSE, FALSE, 0);
        add_spin_setting_row(vbox_notif, "Pop-up Duration (2 – 60 sec):", 2, 60, 1,
            shell->channel ? xfconf_channel_get_int(shell->channel, ULT_PROP_ROOT "/notif_timeout_s", 5) : 5,
            G_CALLBACK(on_notif_timeout_spin), shell);
        GtkWidget *nnote = gtk_label_new("Urgent notifications always stay until you close them.");
        gtk_style_context_add_class(gtk_widget_get_style_context(nnote), "dim-label");
        gtk_widget_set_halign(nnote, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vbox_notif), nnote, FALSE, FALSE, 0);
    }

    /* ---------- Sound (was the separate PulseAudio dialog) ---------- */
    gtk_box_pack_start(GTK_BOX(vbox_sound), systray_build_audio_settings(shell), FALSE, FALSE, 0);

    /* ---------- About ---------- */
    {
        GtkWidget *ab = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(ab), "<b>Ultimate Shell</b> for XFCE\nmacOS- and Windows-style desktop shell");
        gtk_label_set_xalign(GTK_LABEL(ab), 0.0);
        gtk_box_pack_start(GTK_BOX(vbox_about), ab, FALSE, FALSE, 0);

        GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *exp_btn = gtk_button_new_with_label("Export Settings…");
        GtkWidget *imp_btn = gtk_button_new_with_label("Import Settings…");
        GtkWidget *rst_btn = gtk_button_new_with_label("Restart Panel");
        gtk_widget_set_tooltip_text(exp_btn, "Save all Ultimate Shell settings to a file (backup or another computer)");
        gtk_widget_set_tooltip_text(imp_btn, "Load settings from a file and restart the panel");
        gtk_widget_set_tooltip_text(rst_btn, "Same as running xfce4-panel -r");
        g_signal_connect(exp_btn, "clicked", G_CALLBACK(on_export_clicked), shell);
        g_signal_connect(imp_btn, "clicked", G_CALLBACK(on_import_clicked), shell);
        g_signal_connect(rst_btn, "clicked", G_CALLBACK(on_restart_panel_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(bb), exp_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bb), imp_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bb), rst_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_about), bb, FALSE, FALSE, 6);
    }

    /* ---------- Pages, in sidebar order ---------- */
    /* ---------- Effects (General) ---------- */
    {
        gtk_box_pack_start(GTK_BOX(vbox_general), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
        GtkWidget *hd = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hd), "<b>Effects</b>");
        gtk_widget_set_halign(hd, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vbox_general), hd, FALSE, FALSE, 0);

        GtkWidget *comp = gtk_check_button_new_with_label("Desktop compositing (transparency, shadows, smooth fades)");
        gboolean comp_on = xfconf_init(NULL) ? xfconf_channel_get_bool(xfconf_channel_get("xfwm4"), "/general/use_compositing", TRUE) : TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(comp), comp_on);
        gtk_widget_set_tooltip_text(comp, "Same switch as Window Manager Tweaks ▸ Compositor. Needed for fading popups.");
        g_signal_connect(comp, "toggled", G_CALLBACK(on_compositing_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_general), comp, FALSE, FALSE, 0);

        GtkWidget *anim_chk = gtk_check_button_new_with_label("Animations");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(anim_chk), shell->anim_mode != 0);
        gtk_widget_set_tooltip_text(anim_chk, "Untick for popups and menus that open and close instantly.");
        gtk_box_pack_start(GTK_BOX(vbox_general), anim_chk, FALSE, FALSE, 0);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, 24);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Style:"), FALSE, FALSE, 0);
        GtkWidget *am = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(am), "Fade (fade in and out)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(am), "Slide (slide out of and back into the bar)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(am), shell->anim_style == 2 ? 1 : 0);
        gtk_box_pack_end(GTK_BOX(row), am, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_general), row, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(row, shell->anim_mode != 0);

        GtkWidget *srow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(srow), gtk_label_new("Animation speed:"), FALSE, FALSE, 0);
        GtkWidget *sp = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 50, 200, 5);
        gtk_range_set_value(GTK_RANGE(sp), CLAMP(shell->anim_speed, 50, 200));
        gtk_scale_add_mark(GTK_SCALE(sp), 100, GTK_POS_BOTTOM, NULL);
        gtk_widget_set_size_request(sp, 200, -1);
        g_signal_connect(sp, "format-value", G_CALLBACK(on_anim_speed_format), NULL);
        g_signal_connect(sp, "value-changed", G_CALLBACK(on_anim_speed_changed), shell);
        gtk_box_pack_end(GTK_BOX(srow), sp, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(srow, shell->anim_mode != 0);
        gtk_widget_set_margin_start(srow, 24);
        gtk_box_pack_start(GTK_BOX(vbox_general), srow, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(anim_chk), "style-row", row);
        g_object_set_data(G_OBJECT(anim_chk), "speed-row", srow);
        g_signal_connect(anim_chk, "toggled", G_CALLBACK(on_anim_enabled_toggled), shell);
        g_signal_connect(am, "changed", G_CALLBACK(on_anim_style_changed), shell);

        GtkWidget *note = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(note), "<small>Popups animate only while compositing is on. "
                                              "Untick Animations for instant popups (good for slow machines and VMs).</small>");
        gtk_label_set_xalign(GTK_LABEL(note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
        gtk_box_pack_start(GTK_BOX(vbox_general), note, FALSE, FALSE, 0);
    }

    /* ---------- Workspaces ---------- */
    GtkWidget *vbox_ws = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    {
        GtkWidget *en = gtk_check_button_new_with_label("Show workspace indicator");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(en), shell->ws_enabled);
        g_signal_connect(en, "toggled", G_CALLBACK(on_ws_enabled_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_ws), en, FALSE, FALSE, 0);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Style:"), FALSE, FALSE, 0);
        GtkWidget *st = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st), "GNOME pills (active one wider)");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st), "Dots");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(st), "Numbers");
        gtk_combo_box_set_active(GTK_COMBO_BOX(st), CLAMP(shell->ws_style, 0, 2));
        g_signal_connect(st, "changed", G_CALLBACK(on_ws_style_changed), shell);
        gtk_box_pack_end(GTK_BOX(row), st, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_ws), row, FALSE, FALSE, 0);

        add_spin_setting_row(vbox_ws, "Indicator Size (4 – 14 px):", 4, 14, 1,
                             shell->ws_size, G_CALLBACK(on_ws_size_spin), shell);
        add_spin_setting_row(vbox_ws, "Number of Workspaces (1 – 16):", 1, 16, 1,
                             workspace_indicator_get_count(), G_CALLBACK(on_ws_count_spin), shell);

        GtkWidget *sc = gtk_check_button_new_with_label("Scroll over the indicator to switch workspaces");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sc), shell->ws_scroll);
        g_signal_connect(sc, "toggled", G_CALLBACK(on_ws_scroll_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_ws), sc, FALSE, FALSE, 0);

        GtkWidget *wr = gtk_check_button_new_with_label("Wrap around (last → first)");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wr), shell->ws_wrap);
        g_signal_connect(wr, "toggled", G_CALLBACK(on_ws_wrap_toggled), shell);

        gtk_box_pack_start(GTK_BOX(vbox_ws), wr, FALSE, FALSE, 0);

        GtkWidget *hs = gtk_check_button_new_with_label("Hide when there is only one workspace");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hs), shell->ws_hide_single);
        g_signal_connect(hs, "toggled", G_CALLBACK(on_ws_single_toggled), shell);
        gtk_box_pack_start(GTK_BOX(vbox_ws), hs, FALSE, FALSE, 0);

        GtkWidget *nb = gtk_button_new_with_label("Rename Workspaces…");
        gtk_widget_set_halign(nb, GTK_ALIGN_START);
        g_signal_connect(nb, "clicked", G_CALLBACK(on_ws_names_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(vbox_ws), nb, FALSE, FALSE, 4);

        GtkWidget *note = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(note),
            "<small>Click a dot to switch · right-click for Add / Remove.\n"
            "macOS layout: sits between the Apple menu and the global menu, or takes the far-right spot "
            "when the window buttons are on the left.\nWindows layout: placed by Taskbar ▸ Panel Layout.</small>");
        gtk_label_set_xalign(GTK_LABEL(note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
        gtk_box_pack_start(GTK_BOX(vbox_ws), note, FALSE, FALSE, 0);
    }
    settings_add_page(GTK_STACK(stack), vbox_general, "general", "General",          PAGE_ANY,      shell, RESET_GENERAL);
    settings_add_page(GTK_STACK(stack), vbox_top,     "topbar",  "Top Bar",          PAGE_MAC_ONLY, shell, RESET_TOPBAR);
    settings_add_page(GTK_STACK(stack), vbox_dock,    "dock",    "Dock",             PAGE_MAC_ONLY, shell, RESET_DOCK);
    settings_add_page(GTK_STACK(stack), vbox_layout,  "taskbar", "Taskbar",          PAGE_WIN_ONLY, shell, RESET_TASKBAR);
    settings_add_page(GTK_STACK(stack), vbox_ind,     "indicators", "Indicators",    PAGE_ANY,      shell, RESET_IND);
    settings_add_page(GTK_STACK(stack), vbox_ws,      "workspaces", "Workspaces",    PAGE_ANY,      shell, RESET_WS);
    settings_add_page(GTK_STACK(stack), vbox_sm,      "start",   "Start Menu",       PAGE_ANY,      shell, RESET_START);
    settings_add_page(GTK_STACK(stack), vbox_clk,     "clock",   "Clock & Calendar", PAGE_ANY,      shell, RESET_CLOCK);
    settings_add_page(GTK_STACK(stack), vbox_tray,    "tray",    "System Tray",      PAGE_ANY,      shell, RESET_TRAY);
    settings_add_page(GTK_STACK(stack), vbox_sound,   "sound",   "Sound",            PAGE_ANY,      shell, RESET_SOUND);
    settings_add_page(GTK_STACK(stack), vbox_notif,   "notif",   "Notifications",    PAGE_ANY,      shell, RESET_NOTIF);
    settings_add_page(GTK_STACK(stack), vbox_about,   "about",   "About",            PAGE_ANY,      shell, NULL);

    settings_make_shrinkable(stack);                  /* the window can now be made narrow */
    gtk_widget_show_all(dlg);
    g_timeout_add(250, settings_shrink_later, stack);   /* again for rows that pages add after opening */
    sync_start_tab_controls(sm_ui);
    look_snapshot_reset(shell);                       /* the look as the window opened */
    g_object_set_data_full(G_OBJECT(stack), "prev-page", g_strdup(gtk_stack_get_visible_child_name(GTK_STACK(stack))), g_free);
    g_signal_connect(stack, "notify::visible-child", G_CALLBACK(on_settings_page_switched), shell);
    g_signal_connect(dlg, "response", G_CALLBACK(on_settings_closing), shell);   /* runs first */
    g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy), NULL);
}

static void on_xsettings_theme_changed(XfconfChannel *channel, const gchar *property, const GValue *value, UltimateShell *shell) {
if (!shell) return;
if (g_strcmp0(property, "/Net/ThemeName") == 0 || g_strcmp0(property, "/Net/IconThemeName") == 0) {
    shell_apply_theme(shell);
    if (shell->dock_window) dock_window_apply_settings(shell);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
}
}

static void on_plugin_free_data(XfcePanelPlugin *plugin, UltimateShell *shell) {
    dock_window_shutdown();
    windowck_shutdown();
}

static void ultimate_shell_construct(XfcePanelPlugin *plugin) {
    UltimateShell *shell = g_new0(UltimateShell, 1);
    shell->plugin = plugin;
    shell->tracker = window_tracker_new();

    load_settings_from_xfconf(shell);
    shell->size_guard_until = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;   /* startup */
    shell_apply_theme(shell);
    if (xfconf_init(NULL)) {
    XfconfChannel *xset_ch = xfconf_channel_get("xsettings");
    if (xset_ch) {
        g_signal_connect(xset_ch, "property-changed", G_CALLBACK(on_xsettings_theme_changed), shell);
    }
}

    xfce_panel_plugin_set_expand(plugin, TRUE);
    g_spawn_command_line_async(
        "sh -c \"pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 || "
        "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1 >/dev/null 2>&1 & "
        "pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 || "
        "lxpolkit >/dev/null 2>&1 & "
        "true\"",
        NULL);
    xfce_panel_plugin_menu_show_configure(plugin);

    g_signal_connect(plugin, "remote-event", G_CALLBACK(on_plugin_remote_event), shell);
    super_key_apply(shell);

    /* before any widget is built: the start menu, popups and stacks read their animation
       times from it while they are created (set later, the first build got no animation) */
    g_anim_shell = shell;

    shell->root_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(shell->root_box, "ultimate-root-box");

    shell->left_box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    shell->center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    shell->right_box  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_valign(shell->root_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(shell->left_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(shell->center_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(shell->right_box, GTK_ALIGN_FILL);

    gtk_box_pack_start(GTK_BOX(shell->root_box), shell->left_box, FALSE, FALSE, 0);
    /* True panel center: GtkBox centers its center widget on the whole panel,
       whatever the widths of the left and right zones (it only shifts when they would overlap). */
    gtk_box_set_center_widget(GTK_BOX(shell->root_box), shell->center_box);
    gtk_box_pack_end(GTK_BOX(shell->root_box), shell->right_box, FALSE, FALSE, 0);

    /* 1. Start Menu Button */
    shell->start_btn = start_menu_button_new(shell);
    gtk_box_pack_start(GTK_BOX(shell->left_box), shell->start_btn, FALSE, FALSE, 0);

    /* 2. macOS Windowck + Global Menu Container */
    shell->mac_top_Controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    shell->windowck_widget = windowck_widget_new(shell->tracker);
    shell->global_menu_widget = global_menu_widget_new(shell->tracker);
    gtk_box_pack_start(GTK_BOX(shell->mac_top_Controls_box), shell->windowck_widget, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(shell->mac_top_Controls_box), shell->global_menu_widget, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(shell->left_box), shell->mac_top_Controls_box, FALSE, FALSE, 0);
    shell->app_title_widget = app_title_new(shell);      /* GNOME mode only */
    gtk_box_pack_start(GTK_BOX(shell->left_box), shell->app_title_widget, FALSE, FALSE, 0);

    /* Centered group (Start + taskbar in the centered Windows layouts) */
    shell->center_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(shell->center_group, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(shell->center_group, GTK_ALIGN_FILL);
    gtk_box_pack_start(GTK_BOX(shell->center_box), shell->center_group, TRUE, FALSE, 0);

    /* Workspace indicator (placed by shell_apply_layout) */
    shell->ws_widget = workspace_indicator_new(shell);
    gtk_box_pack_start(GTK_BOX(shell->right_box), shell->ws_widget, FALSE, FALSE, 4);

    /* 3. Live Grouped & Custom Taskbar Widget */
    shell->taskbar_box = taskbar_widget_new(shell);
    gtk_box_pack_start(GTK_BOX(shell->center_box), shell->taskbar_box, TRUE, FALSE, 0);

    /* 4. Right Zone: [Systray] -> [Clock] -> [Notifications] -> [Show Desktop] */
    shell->systray_widget = systray_widget_new(shell);
    shell->clock_widget   = clock_widget_new(shell);
    shell->notif_widget   = notifications_widget_new();

    shell->show_desktop_btn = gtk_button_new();
    gtk_widget_set_name(shell->show_desktop_btn, "ultimate-show-desktop-btn");
    gtk_button_set_relief(GTK_BUTTON(shell->show_desktop_btn), GTK_RELIEF_NONE);
    gtk_widget_set_size_request(shell->show_desktop_btn, 10, -1);
    gtk_widget_set_valign(shell->show_desktop_btn, GTK_ALIGN_FILL);
    gtk_widget_set_tooltip_text(shell->show_desktop_btn, "Show Desktop");
    g_signal_connect(shell->show_desktop_btn, "clicked", G_CALLBACK(on_show_desktop_clicked), shell);

    gtk_box_pack_start(GTK_BOX(shell->right_box), shell->systray_widget, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(shell->right_box), shell->clock_widget, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(shell->right_box), shell->notif_widget, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(shell->right_box), shell->show_desktop_btn, FALSE, FALSE, 0);   /* flush in the corner */

    /* 5. Initialize standalone macOS Dock Window */
    dock_window_init(shell);

    gtk_container_add(GTK_CONTAINER(plugin), shell->root_box);
    gtk_widget_show_all(shell->root_box);

    g_anim_shell = shell;
    apply_panel_frame(shell);
    win_fill_apply_settings(shell);
    g_signal_connect(shell->root_box, "size-allocate", G_CALLBACK(on_root_box_size_allocate), shell);
    shell_apply_animation_css(shell);
    shell_apply_layout(shell, shell->mode);

    g_signal_connect(plugin, "configure-plugin", G_CALLBACK(on_configure_plugin), shell);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed), shell);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(on_plugin_size_changed), shell);
    g_signal_connect(plugin, "free-data", G_CALLBACK(on_plugin_free_data), shell);
}

XFCE_PANEL_PLUGIN_REGISTER(ultimate_shell_construct);


typedef struct {
    GAppInfo *app;
} DeferredLaunchData;

static void on_launched_child_exited(GPid pid, gint status, gpointer user_data) {
    g_spawn_close_pid(pid);
}

/* Same as the Start menu: make sure a Polkit authentication agent is running so the
   password prompt can appear (e.g. Synaptic, GParted, apps that use pkexec). */
static void ensure_polkit_agent_for_launch(void) {
    gchar *argv[] = { "sh", "-c",
        "pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 && exit 0; "
        "for a in /usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1 "
        "/usr/libexec/polkit-gnome-authentication-agent-1 /usr/lib/xfce-polkit/xfce-polkit "
        "/usr/libexec/xfce-polkit lxpolkit; do "
        "  if command -v \"$a\" >/dev/null 2>&1; then \"$a\" >/dev/null 2>&1 & exit 0; fi; "
        "done", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static gboolean on_deferred_launch_idle(gpointer user_data) {
    DeferredLaunchData *d = (DeferredLaunchData *)user_data;
    if (!d || !d->app) { g_free(d); return G_SOURCE_REMOVE; }

    GdkDisplay *display = gdk_display_get_default();
    if (display) gdk_display_sync(display);

    const gchar *cmdline = g_app_info_get_commandline(d->app);
    gboolean launched = FALSE;

    if (cmdline && cmdline[0] != '\0') {
        /* Strip desktop entry field codes (%f, %F, %u, %U, %i, %c, %k, %d, %D, %n, %N, %v, %m) like Whisker Menu */
        GString *clean = g_string_new("");
        for (const gchar *p = cmdline; *p != '\0'; p++) {
            if (*p == '%' && *(p + 1) != '\0') {
                p++;
                if (*p == '%') g_string_append_c(clean, '%');
                continue;
            }
            g_string_append_c(clean, *p);
        }
        g_strstrip(clean->str);

        gboolean needs_term = FALSE;
        if (G_IS_DESKTOP_APP_INFO(d->app)) {
            needs_term = g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(d->app), "Terminal");
        }

        gchar *final_cmd = NULL;
        if (needs_term) {
            final_cmd = g_strdup_printf("exo-open --launch TerminalEmulator %s", clean->str);
        } else {
            final_cmd = g_strdup(clean->str);
        }

        gint argc = 0;
        gchar **argv = NULL;
        if (g_shell_parse_argv(final_cmd, &argc, &argv, NULL) && argc > 0) {
            gchar **envp = g_get_environ();
            if (display) {
                const gchar *disp_name = gdk_display_get_name(display);
                if (disp_name) envp = g_environ_setenv(envp, "DISPLAY", disp_name, TRUE);
            }
            /* G_SPAWN_DO_NOT_REAP_CHILD keeps the panel as the app's live parent. Without it GLib
               double-forks and the app is re-parented to PID 1, which pkexec refuses
               ("Refusing to render service to dead parents") — so privileged apps pinned to the
               dock/taskbar never opened, while the Start menu (which already did this) worked. */
            GPid child_pid = 0;
            gchar *wd = G_IS_DESKTOP_APP_INFO(d->app)
                        ? g_desktop_app_info_get_string(G_DESKTOP_APP_INFO(d->app), "Path") : NULL;
            if (wd && (!wd[0] || !g_file_test(wd, G_FILE_TEST_IS_DIR))) { g_free(wd); wd = NULL; }
            launched = g_spawn_async(
                wd ? wd : g_get_home_dir(),
                argv,
                envp,
                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                NULL,
                NULL,
                &child_pid,
                NULL
            );
            g_free(wd);
            if (launched && child_pid > 0) g_child_watch_add(child_pid, on_launched_child_exited, NULL);
            g_strfreev(envp);
            g_strfreev(argv);
        }
        g_free(final_cmd);
        g_string_free(clean, TRUE);
    }

    if (!launched) {
        GdkAppLaunchContext *ctx = display ? gdk_display_get_app_launch_context(display) : NULL;
        if (ctx) {
            gdk_app_launch_context_set_screen(ctx, gdk_screen_get_default());
            gdk_app_launch_context_set_timestamp(ctx, GDK_CURRENT_TIME);
        }
        g_app_info_launch(d->app, NULL, G_APP_LAUNCH_CONTEXT(ctx), NULL);
        if (ctx) g_object_unref(ctx);
    }

    g_object_unref(d->app);
    g_free(d);
    return G_SOURCE_REMOVE;
}

void shell_launch_app_info(GAppInfo *app) {
    if (!app) return;
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkSeat *seat = gdk_display_get_default_seat(display);
        if (seat) gdk_seat_ungrab(seat);
        gdk_display_sync(display);
    }
    ensure_polkit_agent_for_launch();
    DeferredLaunchData *d = g_new0(DeferredLaunchData, 1);
    d->app = g_object_ref(app);
    /* Wait 60ms after popup hide & seat ungrab so Polkit authentication agent can grab X11 keyboard/pointer cleanly */
    g_timeout_add(60, on_deferred_launch_idle, d);
}
