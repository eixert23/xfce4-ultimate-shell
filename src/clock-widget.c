/* Panel clock + calendar flyouts (Windows 7 / 10 / 11, GNOME-macOS).
 * Ticks exactly on the second / minute, follows time-zone changes, opens on today. */
#include "clock-widget.h"
#include "notifications.h"
#include <cairo.h>
#include <math.h>
#include <string.h>

typedef struct {
    UltimateShell *shell;
    GtkWidget     *btn;
    GtkWidget     *panel_box;
    GtkWidget     *panel_time_lbl;
    GtkWidget     *panel_date_lbl;
    GtkWidget     *popup_win;
    GtkWidget     *popup_root;
    GtkWidget     *header_time_lbl;
    GtkWidget     *header_date_lbl;
    GtkWidget     *header_day_lbl;     /* GNOME: weekday line */
    GtkWidget     *analog_clock_da;
    GtkWidget     *calendar;
    GtkWidget     *notif_list;         /* GNOME: notification list box */
    GtkWidget     *notif_empty;
    GtkWidget     *dnd_switch;
    guint          timer_id;
    GTimeZone     *tz;
    GFileMonitor  *tz_mon;
} ClockWidget;

static ClockWidget *g_clock_widget = NULL;

static const gchar *weight_idx_to_pango_str(gint w) {
    switch (w) {
        case 0: return "normal";
        case 1: return "medium";
        case 3: return "heavy";
        case 2:
        default: return "bold";
    }
}

/* index = Clock & Calendar ▸ Date Format (0–4 kept from before, so saved choices stay) */
static const gchar *date_fmt_idx_to_strftime(UltimateShell *sh) {
    switch (sh->clock_date_fmt) {
        case 1: return "%m/%d/%Y";
        case 2: return "%d/%m/%Y";
        case 3: return "%a, %b %-d";
        case 4: return "%b %-d, %Y";
        case 5: return "%x";                     /* your language's own short date */
        case 6: return "%-d %b %Y";
        case 7: return (sh->clock_date_custom && sh->clock_date_custom[0]) ? sh->clock_date_custom : "%x";
        case 0:
        default: return "%Y-%m-%d";
    }
}

/* ---------- time zone: re-read when /etc/localtime changes (GLib caches "local") ---------- */
static GTimeZone *read_local_tz(void) {
    if (g_getenv("TZ")) return g_time_zone_new_local();
    gchar *target = g_file_read_link("/etc/localtime", NULL);
    GTimeZone *tz = NULL;
    if (target) {
        const gchar *z = strstr(target, "zoneinfo/");
        if (z) tz = g_time_zone_new_identifier(z + strlen("zoneinfo/"));
        g_free(target);
    }
    if (!tz) {
        gchar *name = NULL;
        if (g_file_get_contents("/etc/timezone", &name, NULL, NULL) && name) {
            g_strstrip(name);
            tz = g_time_zone_new_identifier(name);
        }
        g_free(name);
    }
    return tz ? tz : g_time_zone_new_local();
}

static GDateTime *clock_now(ClockWidget *cw) {
    if (!cw->tz) cw->tz = read_local_tz();
    return g_date_time_new_now(cw->tz);
}

static gboolean clock_tick(gpointer data);
static void clock_schedule(ClockWidget *cw);

static void on_tz_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, ClockWidget *cw) {
    if (cw->tz) g_time_zone_unref(cw->tz);
    cw->tz = read_local_tz();
    clock_schedule(cw);
}

static gboolean on_draw_win7_analog_clock(GtkWidget *widget, cairo_t *cr, ClockWidget *cw) {
    gint w = gtk_widget_get_allocated_width(widget);
    gint h = gtk_widget_get_allocated_height(widget);
    gdouble cx = w / 2.0;
    gdouble cy = h / 2.0;
    gdouble radius = MIN(w, h) / 2.0 - 6.0;
    if (radius < 20.0) return FALSE;

    GDateTime *now = clock_now(cw);
    gint hour   = g_date_time_get_hour(now) % 12;
    gint minute = g_date_time_get_minute(now);
    gint second = g_date_time_get_second(now);
    g_date_time_unref(now);

    /* 1. Outer Metallic / Glass Bezel Ring */
    cairo_pattern_t *bezel_pat = cairo_pattern_create_linear(cx - radius, cy - radius, cx + radius, cy + radius);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 0.0, 0.72, 0.78, 0.85, 0.95);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 0.5, 0.35, 0.42, 0.50, 0.95);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 1.0, 0.60, 0.68, 0.76, 0.95);
    cairo_arc(cr, cx, cy, radius + 3.0, 0, 2 * G_PI);
    cairo_set_source(cr, bezel_pat);
    cairo_fill(cr);
    cairo_pattern_destroy(bezel_pat);

    /* 2. Soft Radial Dial Face (Classic Windows 7 Pearl/Silver Dial) */
    cairo_pattern_t *face_pat = cairo_pattern_create_radial(
        cx, cy - radius * 0.25, radius * 0.1,
        cx, cy, radius);
    cairo_pattern_add_color_stop_rgba(face_pat, 0.0, 0.98, 0.99, 1.00, 1.0);
    cairo_pattern_add_color_stop_rgba(face_pat, 0.82, 0.90, 0.93, 0.96, 1.0);
    cairo_pattern_add_color_stop_rgba(face_pat, 1.0, 0.78, 0.83, 0.89, 1.0);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_set_source(cr, face_pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(face_pat);

    cairo_set_source_rgba(cr, 0.25, 0.32, 0.40, 0.85);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    /* 3. Minute & Hour Tick Marks */
    for (int i = 0; i < 60; i++) {
        gdouble angle = (i * 6.0 - 90.0) * (G_PI / 180.0);
        gboolean is_hour = (i % 5 == 0);
        gboolean is_quarter = (i % 15 == 0);

        gdouble inner_r = is_quarter ? (radius - 11.0) : (is_hour ? (radius - 8.5) : (radius - 5.0));
        gdouble outer_r = radius - 3.0;

        cairo_move_to(cr, cx + cos(angle) * inner_r, cy + sin(angle) * inner_r);
        cairo_line_to(cr, cx + cos(angle) * outer_r, cy + sin(angle) * outer_r);

        if (is_quarter) {
            cairo_set_source_rgba(cr, 0.10, 0.22, 0.36, 0.95);
            cairo_set_line_width(cr, 2.4);
        } else if (is_hour) {
            cairo_set_source_rgba(cr, 0.18, 0.28, 0.38, 0.85);
            cairo_set_line_width(cr, 1.8);
        } else {
            cairo_set_source_rgba(cr, 0.42, 0.48, 0.55, 0.65);
            cairo_set_line_width(cr, 1.0);
        }
        cairo_stroke(cr);
    }

    /* 4. Hour Hand */
    gdouble hour_angle = ((hour + minute / 60.0 + second / 3600.0) * 30.0 - 90.0) * (G_PI / 180.0);
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 0.14, 0.20, 0.28, 0.95);
    cairo_set_line_width(cr, 3.8);
    cairo_move_to(cr, cx - cos(hour_angle) * (radius * 0.14), cy - sin(hour_angle) * (radius * 0.14));
    cairo_line_to(cr, cx + cos(hour_angle) * (radius * 0.52), cy + sin(hour_angle) * (radius * 0.52));
    cairo_stroke(cr);
    cairo_restore(cr);

    /* 5. Minute Hand */
    gdouble min_angle = ((minute + second / 60.0) * 6.0 - 90.0) * (G_PI / 180.0);
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 0.16, 0.24, 0.34, 0.95);
    cairo_set_line_width(cr, 2.6);
    cairo_move_to(cr, cx - cos(min_angle) * (radius * 0.18), cy - sin(min_angle) * (radius * 0.18));
    cairo_line_to(cr, cx + cos(min_angle) * (radius * 0.74), cy + sin(min_angle) * (radius * 0.74));
    cairo_stroke(cr);
    cairo_restore(cr);

    /* 6. Second Hand (Classic Windows 7 Crimson Red Sweep) */
    gdouble sec_angle = (second * 6.0 - 90.0) * (G_PI / 180.0);
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 0.84, 0.16, 0.16, 0.92);
    cairo_set_line_width(cr, 1.3);
    cairo_move_to(cr, cx - cos(sec_angle) * (radius * 0.22), cy - sin(sec_angle) * (radius * 0.22));
    cairo_line_to(cr, cx + cos(sec_angle) * (radius * 0.82), cy + sin(sec_angle) * (radius * 0.82));
    cairo_stroke(cr);
    cairo_restore(cr);

    /* 7. Center Pivot Cap */
    cairo_arc(cr, cx, cy, 3.8, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, 0.14, 0.20, 0.28, 1.0);
    cairo_fill(cr);
    cairo_arc(cr, cx, cy, 1.8, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, 0.84, 0.16, 0.16, 1.0);
    cairo_fill(cr);

    return FALSE;
}

/* =========================================================================
 * Panel text
 * ========================================================================= */
static gchar *fmt_time(ClockWidget *cw, GDateTime *now, gboolean secs) {
    UltimateShell *sh = cw->shell;
    const gchar *f = sh->clock_24h ? (secs ? "%H:%M:%S" : "%H:%M")
                                   : (secs ? "%-I:%M:%S %p" : "%-I:%M %p");   /* 1:35 PM, not 01:35 PM */
    return g_date_time_format(now, f);
}

static void set_label_markup(GtkWidget *lbl, gint pt, const gchar *weight, const gchar *text) {
    gchar *esc = g_markup_escape_text(text ? text : "", -1);
    gchar *mk = g_strdup_printf("<span font_desc='%d' weight='%s'>%s</span>", pt, weight, esc);
    gtk_label_set_markup(GTK_LABEL(lbl), mk);
    g_free(mk); g_free(esc);
}

static void update_panel(ClockWidget *cw, GDateTime *now) {
    UltimateShell *sh = cw->shell;
    gint time_pt = (sh->clock_time_pt > 0) ? CLAMP(sh->clock_time_pt, 7, 24) : 10;
    gint date_pt = (sh->clock_date_pt > 0) ? CLAMP(sh->clock_date_pt, 7, 20) : 9;
    const gchar *tw = weight_idx_to_pango_str(sh->clock_time_weight);
    const gchar *dw = weight_idx_to_pango_str(sh->clock_date_weight);
    gchar *t = fmt_time(cw, now, sh->clock_show_seconds);

    if (sh->mode == LAYOUT_MACOS) {
        /* macOS menu bar: "Mon 16 Nov  1:35 PM" */
        GString *s = g_string_new("");
        if (sh->clock_mac_show_day)  { gchar *d = g_date_time_format(now, "%a"); g_string_append(s, d); g_free(d); }
        if (sh->clock_mac_show_date) {
            gchar *d = g_date_time_format(now, "%-d %b");
            if (s->len) g_string_append_c(s, ' ');
            g_string_append(s, d);
            g_free(d);
        }
        if (s->len) g_string_append(s, "  ");
        g_string_append(s, t);
        set_label_markup(cw->panel_time_lbl, time_pt, tw, s->str);
        g_string_free(s, TRUE);
        gtk_widget_show(cw->panel_time_lbl);
        gtk_widget_hide(cw->panel_date_lbl);
    } else {
        gchar *d = g_date_time_format(now, date_fmt_idx_to_strftime(sh));
        if (sh->clock_format == CLOCK_FMT_TIME_ONLY) {         /* "Single line": time · date */
            gchar *one = g_strdup_printf("%s  ·  %s", t, d ? d : "");
            set_label_markup(cw->panel_time_lbl, time_pt, tw, one);
            g_free(one);
            gtk_widget_hide(cw->panel_date_lbl);
        } else {
            gboolean date_top = (sh->clock_format == CLOCK_FMT_DATE_TOP);
            gint sp = CLAMP(sh->clock_line_spacing, -8, 12);
            if (sp < 0) {
                /* Tighter lines: one label, the second line with a smaller Pango line height.
                   (A negative widget margin made GTK warn on every update.) */
                gint second_pt = date_top ? time_pt : date_pt;
                gdouble line_px = second_pt * 96.0 / 72.0 * 1.2;
                gdouble lh = CLAMP(1.0 + 2.0 * sp / line_px, 0.4, 1.0);
                gchar *et = g_markup_escape_text(t ? t : "", -1);
                gchar *ed = g_markup_escape_text(d ? d : "", -1);
                gchar *mt = g_strdup_printf("<span font_desc='%d' weight='%s'>%s</span>", time_pt, tw, et);
                gchar *md = g_strdup_printf("<span font_desc='%d' weight='%s'>%s</span>", date_pt, dw, ed);
                gchar lhs[G_ASCII_DTOSTR_BUF_SIZE];
                g_ascii_formatd(lhs, sizeof lhs, "%.2f", lh);
                gchar *mk = g_strdup_printf("%s\n<span line_height='%s'>%s</span>",
                                            date_top ? md : mt, lhs, date_top ? mt : md);
                gtk_label_set_markup(GTK_LABEL(cw->panel_time_lbl), mk);
                g_free(mk); g_free(mt); g_free(md); g_free(et); g_free(ed);
                gtk_widget_hide(cw->panel_date_lbl);
            } else {
                set_label_markup(cw->panel_time_lbl, time_pt, tw, t);
                set_label_markup(cw->panel_date_lbl, date_pt, dw, d);
                gtk_box_reorder_child(GTK_BOX(cw->panel_box), cw->panel_date_lbl, date_top ? 0 : 1);
                gtk_widget_show(cw->panel_date_lbl);
            }
        }
        gtk_widget_show(cw->panel_time_lbl);
        g_free(d);
    }
    /* line spacing (negative pulls the two lines together) */
    gint sp = CLAMP(sh->clock_line_spacing, -8, 12);
    gtk_box_set_spacing(GTK_BOX(cw->panel_box), MAX(0, sp));

    gchar *tip = g_date_time_format(now, "%A, %-d %B %Y");
    gtk_widget_set_tooltip_text(cw->btn, tip);
    g_free(tip);
    g_free(t);
}

static void update_flyout(ClockWidget *cw, GDateTime *now) {
    if (!gtk_widget_get_visible(cw->popup_win)) return;
    CalendarStyle st = cw->shell->cal_style;
    if (GTK_IS_LABEL(cw->header_time_lbl)) {
        gchar *t = fmt_time(cw, now, TRUE);
        gchar *mk = g_markup_printf_escaped(st == CAL_STYLE_WIN10
            ? "<span font_desc='34' weight='300'>%s</span>" : "<span size='large'>%s</span>", t);
        gtk_label_set_markup(GTK_LABEL(cw->header_time_lbl), mk);
        g_free(mk); g_free(t);
    }
    if (GTK_IS_LABEL(cw->header_date_lbl)) {
        const gchar *f = (st == CAL_STYLE_WIN11) ? "%A, %B %-d" : (st == CAL_STYLE_GNOME) ? "%B %-d %Y" : "%A, %B %-d, %Y";
        gchar *d = g_date_time_format(now, f);
        gchar *mk = g_markup_printf_escaped(st == CAL_STYLE_GNOME ? "<span size='x-large' weight='bold'>%s</span>"
                                                                   : "<b>%s</b>", d);
        gtk_label_set_markup(GTK_LABEL(cw->header_date_lbl), mk);
        g_free(mk); g_free(d);
    }
    if (GTK_IS_LABEL(cw->header_day_lbl)) {
        gchar *d = g_date_time_format(now, "%A");
        gtk_label_set_text(GTK_LABEL(cw->header_day_lbl), d);
        g_free(d);
    }
    if (GTK_IS_WIDGET(cw->analog_clock_da)) gtk_widget_queue_draw(cw->analog_clock_da);
}

/* ---------- ticking: exactly on the second (seconds shown / flyout open) or the minute ---------- */
static gboolean clock_needs_seconds(ClockWidget *cw) {
    return cw->shell->clock_show_seconds || gtk_widget_get_visible(cw->popup_win);
}

static void clock_schedule(ClockWidget *cw) {
    if (cw->timer_id) g_source_remove(cw->timer_id);
    GDateTime *now = clock_now(cw);
    gint ms_in_sec = (gint)(g_date_time_get_microsecond(now) / 1000);
    gint wait = 1000 - ms_in_sec + 15;
    if (!clock_needs_seconds(cw)) wait += (59 - g_date_time_get_second(now)) * 1000;
    g_date_time_unref(now);
    cw->timer_id = g_timeout_add(MAX(wait, 20), clock_tick, cw);
}

static gboolean clock_tick(gpointer data) {
    ClockWidget *cw = data;
    cw->timer_id = 0;
    GDateTime *now = clock_now(cw);
    update_panel(cw, now);
    update_flyout(cw, now);
    g_date_time_unref(now);
    clock_schedule(cw);
    return G_SOURCE_REMOVE;
}

/* =========================================================================
 * Flyouts
 * ========================================================================= */
static void cal_go_today(ClockWidget *cw) {
    if (!GTK_IS_CALENDAR(cw->calendar)) return;
    GDateTime *now = clock_now(cw);
    GtkCalendar *c = GTK_CALENDAR(cw->calendar);
    gtk_calendar_clear_marks(c);
    gtk_calendar_select_month(c, g_date_time_get_month(now) - 1, g_date_time_get_year(now));
    gtk_calendar_select_day(c, g_date_time_get_day_of_month(now));
    gtk_calendar_mark_day(c, g_date_time_get_day_of_month(now));
    g_date_time_unref(now);
}
static void on_today_clicked(GtkButton *b, ClockWidget *cw) { cal_go_today(cw); }

/* the month on screen changed: keep today marked only in today's month */
static void on_cal_month_changed(GtkCalendar *c, ClockWidget *cw) {
    guint y, m, d;
    gtk_calendar_get_date(c, &y, &m, &d);
    GDateTime *now = clock_now(cw);
    gtk_calendar_clear_marks(c);
    if ((gint)y == g_date_time_get_year(now) && (gint)m == g_date_time_get_month(now) - 1)
        gtk_calendar_mark_day(c, g_date_time_get_day_of_month(now));
    g_date_time_unref(now);
}

static void on_datetime_settings(GtkButton *b, ClockWidget *cw) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cw->btn), FALSE);
    g_spawn_command_line_async("sh -c \"time-admin || xfce4-datetime-settings || xfce4-settings-manager\"", NULL);
}

static GtkWidget *make_calendar(ClockWidget *cw, const gchar *css_class) {
    GtkWidget *cal = gtk_calendar_new();
    GtkCalendarDisplayOptions o = GTK_CALENDAR_SHOW_HEADING | GTK_CALENDAR_SHOW_DAY_NAMES;
    if (cw->shell->cal_week_numbers) o |= GTK_CALENDAR_SHOW_WEEK_NUMBERS;
    gtk_calendar_set_display_options(GTK_CALENDAR(cal), o);
    gtk_style_context_add_class(gtk_widget_get_style_context(cal), "ult-cal");
    if (css_class) gtk_style_context_add_class(gtk_widget_get_style_context(cal), css_class);
    g_signal_connect(cal, "month-changed", G_CALLBACK(on_cal_month_changed), cw);
    cw->calendar = cal;
    return cal;
}

static GtkWidget *link_button(const gchar *text, GCallback cb, ClockWidget *cw) {
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "ult-cal-link");
    g_signal_connect(b, "clicked", cb, cw);
    return b;
}

/* ---------- GNOME: notification list ---------- */
static gchar *relative_time(GDateTime *t, ClockWidget *cw) {
    if (!t) return g_strdup("");
    GDateTime *now = clock_now(cw);
    GTimeSpan d = g_date_time_difference(now, t) / G_TIME_SPAN_MINUTE;
    g_date_time_unref(now);
    if (d < 1) return g_strdup("Just now");
    if (d < 60) return g_strdup_printf("%d min ago", (gint)d);
    if (d < 1440) return g_strdup_printf("%d h ago", (gint)(d / 60));
    return g_date_time_format(t, "%-d %b");
}

static void fill_notification_list(ClockWidget *cw) {
    if (!GTK_IS_CONTAINER(cw->notif_list)) return;
    GList *old = gtk_container_get_children(GTK_CONTAINER(cw->notif_list));
    for (GList *l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);
    GList *items = notifications_get_history();
    for (GList *l = items; l; l = l->next) {
        ShellNotifEntry *e = l->data;
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "ult-notif-card");
        GtkWidget *ic = gtk_image_new_from_icon_name(e->app_icon[0] ? e->app_icon : "dialog-information-symbolic",
                                                      GTK_ICON_SIZE_DND);
        gtk_image_set_pixel_size(GTK_IMAGE(ic), 32);
        gtk_widget_set_valign(ic, GTK_ALIGN_START);
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *title = gtk_label_new(e->summary);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(title), "ult-notif-title");
        gchar *rt = relative_time(e->time, cw);
        GtkWidget *when = gtk_label_new(rt);
        g_free(rt);
        gtk_style_context_add_class(gtk_widget_get_style_context(when), "dim-label");
        gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(top), when, FALSE, FALSE, 0);
        /* body may carry simple markup; show it as plain text */
        gchar *plain = NULL;
        if (!pango_parse_markup(e->body, -1, 0, NULL, &plain, NULL, NULL)) plain = g_strdup(e->body);
        GtkWidget *body = gtk_label_new(plain);
        g_free(plain);
        gtk_label_set_xalign(GTK_LABEL(body), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
        gtk_label_set_lines(GTK_LABEL(body), 2);
        gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(body), 38);
        gtk_box_pack_start(GTK_BOX(v), top, FALSE, FALSE, 0);
        if (e->body[0]) gtk_box_pack_start(GTK_BOX(v), body, FALSE, FALSE, 0);
        else gtk_widget_destroy(body);
        gtk_box_pack_start(GTK_BOX(card), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), v, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(cw->notif_list), card, FALSE, FALSE, 0);
    }
    gtk_widget_set_visible(cw->notif_empty, items == NULL);
    g_list_free_full(items, shell_notif_entry_free);
    gtk_widget_show_all(cw->notif_list);
}

static void on_clear_notifs(GtkButton *b, ClockWidget *cw) {
    notifications_clear_all();
    fill_notification_list(cw);
}
static void on_dnd_switch(GObject *sw, GParamSpec *ps, ClockWidget *cw) {
    notifications_set_dnd(gtk_switch_get_active(GTK_SWITCH(sw)));
}

static void ensure_clock_css(void) {
    static GtkCssProvider *p = NULL;
    if (p) return;
    p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p,
        ".ult-cal-accent { color: @theme_selected_bg_color; }"
        ".ult-cal-link { color: @theme_selected_bg_color; padding: 2px 4px; }"
        ".ult-cal-link:hover { text-decoration-line: underline; }"
        "calendar.ult-cal { background: transparent; border: none; padding: 4px; }"
        "calendar.ult-cal:selected { background-color: @theme_selected_bg_color; color: @theme_selected_fg_color; border-radius: 99px; }"
        "calendar.ult-cal.header { font-weight: bold; }"
        "calendar.ult-cal-win10 { font-size: 11pt; padding: 8px 2px; }"
        "calendar.ult-cal-win11 { font-size: 10pt; padding: 6px 2px; }"
        "calendar.ult-cal-gnome { font-size: 10pt; }"
        ".ult-notif-card { background-color: alpha(@theme_fg_color, 0.06); border-radius: 12px; padding: 10px; }"
        ".ult-notif-title { font-weight: bold; }"
        ".ult-cal-pane { background-color: alpha(@theme_fg_color, 0.04); border-radius: 12px; padding: 10px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

void clock_widget_rebuild_style(UltimateShell *shell) {
    if (!g_clock_widget) return;
    ClockWidget *cw = g_clock_widget;
    ensure_clock_css();

    if (cw->popup_root) { gtk_widget_destroy(cw->popup_root); cw->popup_root = NULL; }
    cw->header_time_lbl = cw->header_date_lbl = cw->header_day_lbl = NULL;
    cw->analog_clock_da = cw->calendar = cw->notif_list = cw->notif_empty = cw->dnd_switch = NULL;

    CalendarStyle style = shell->cal_style;
    /* sizes follow the real flyouts */
    if (style == CAL_STYLE_WIN7) {
        /* Windows 7: long date link on top, month calendar left, analog + digital clock right,
           "Change date and time settings…" at the bottom. ~ 380 × 270 */
        gtk_widget_set_size_request(cw->popup_win, 380, 270);
        cw->popup_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(cw->popup_root), 10);
        cw->header_date_lbl = gtk_label_new(NULL);
        gtk_style_context_add_class(gtk_widget_get_style_context(cw->header_date_lbl), "ult-cal-accent");
        gtk_label_set_xalign(GTK_LABEL(cw->header_date_lbl), 0.5);
        GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *cal = make_calendar(cw, NULL);
        GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
        cw->analog_clock_da = gtk_drawing_area_new();
        gtk_widget_set_size_request(cw->analog_clock_da, 130, 130);
        g_signal_connect(cw->analog_clock_da, "draw", G_CALLBACK(on_draw_win7_analog_clock), cw);
        cw->header_time_lbl = gtk_label_new(NULL);
        gtk_box_pack_start(GTK_BOX(right), cw->analog_clock_da, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(right), cw->header_time_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(body), cal, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(body), right, FALSE, FALSE, 4);
        GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(foot), link_button("Change date and time settings…", G_CALLBACK(on_datetime_settings), cw), FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(foot), link_button("Today", G_CALLBACK(on_today_clicked), cw), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), cw->header_date_lbl, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), body, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), foot, FALSE, FALSE, 0);

    } else if (style == CAL_STYLE_WIN10) {
        /* Windows 10: huge light time with seconds, accent date link, big calendar,
           "Date and time settings" at the bottom. ~ 360 × 470 */
        gtk_widget_set_size_request(cw->popup_win, 360, 470);
        cw->popup_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(cw->popup_root), 18);
        cw->header_time_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(cw->header_time_lbl), 0.0);
        GtkWidget *date_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(date_btn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(date_btn), "ult-cal-link");
        cw->header_date_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(cw->header_date_lbl), 0.0);
        gtk_container_add(GTK_CONTAINER(date_btn), cw->header_date_lbl);
        gtk_widget_set_halign(date_btn, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(date_btn, "Go to today");
        g_signal_connect(date_btn, "clicked", G_CALLBACK(on_today_clicked), cw);
        GtkWidget *cal = make_calendar(cw, "ult-cal-win10");
        GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(foot), link_button("Date and time settings", G_CALLBACK(on_datetime_settings), cw), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), cw->header_time_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), date_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), cal, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), foot, FALSE, FALSE, 0);

    } else if (style == CAL_STYLE_GNOME) {
        /* GNOME / macOS: notifications on the left (Do Not Disturb + Clear at the bottom),
           weekday + date and the month calendar on the right. ~ 700 × 440 */
        gtk_widget_set_size_request(cw->popup_win, 700, 440);
        cw->popup_root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(cw->popup_root), 12);

        GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_size_request(left, 380, -1);
        GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *stackbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        cw->notif_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        cw->notif_empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_valign(cw->notif_empty, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(cw->notif_empty, TRUE);
        GtkWidget *bell = gtk_image_new_from_icon_name("preferences-system-notifications-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(bell), 64);
        gtk_widget_set_opacity(bell, 0.4);
        GtkWidget *nolbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(nolbl), "<span size='large'>No Notifications</span>");
        gtk_widget_set_opacity(nolbl, 0.55);
        gtk_box_pack_start(GTK_BOX(cw->notif_empty), bell, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->notif_empty), nolbl, FALSE, FALSE, 0);
        gtk_widget_set_no_show_all(cw->notif_empty, TRUE);
        gtk_widget_show(bell); gtk_widget_show(nolbl);
        gtk_box_pack_start(GTK_BOX(stackbox), cw->notif_list, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(stackbox), cw->notif_empty, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(sc), stackbox);
        GtkWidget *lfoot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(lfoot), gtk_label_new("Do Not Disturb"), FALSE, FALSE, 0);
        cw->dnd_switch = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(cw->dnd_switch), notifications_get_dnd());
        gtk_widget_set_valign(cw->dnd_switch, GTK_ALIGN_CENTER);   /* a compact switch, not stretched to the footer */
        {
            GtkCssProvider *cp = gtk_css_provider_new();
            gtk_css_provider_load_from_data(cp,
                "switch { min-height: 18px; min-width: 36px; padding: 0; margin: 0; }"
                "switch slider { min-width: 16px; min-height: 16px; margin: 1px; }", -1, NULL);
            gtk_style_context_add_provider(gtk_widget_get_style_context(cw->dnd_switch),
                                           GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_USER + 140);
            g_object_unref(cp);
        }
        g_signal_connect(cw->dnd_switch, "notify::active", G_CALLBACK(on_dnd_switch), cw);
        gtk_box_pack_start(GTK_BOX(lfoot), cw->dnd_switch, FALSE, FALSE, 0);
        GtkWidget *clr = gtk_button_new_with_label("Clear");
        g_signal_connect(clr, "clicked", G_CALLBACK(on_clear_notifs), cw);
        gtk_box_pack_end(GTK_BOX(lfoot), clr, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(left), sc, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(left), lfoot, FALSE, FALSE, 0);

        GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(right), "ult-cal-pane");
        GtkWidget *hdr_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(hdr_btn), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(hdr_btn, "Go to today");
        g_signal_connect(hdr_btn, "clicked", G_CALLBACK(on_today_clicked), cw);
        GtkWidget *hv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        cw->header_day_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(cw->header_day_lbl), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(cw->header_day_lbl), "dim-label");
        cw->header_date_lbl = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(cw->header_date_lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(hv), cw->header_day_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hv), cw->header_date_lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(hdr_btn), hv);
        GtkWidget *cal = make_calendar(cw, "ult-cal-gnome");
        gtk_box_pack_start(GTK_BOX(right), hdr_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(right), cal, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(cw->popup_root), left, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), right, FALSE, FALSE, 0);

    } else {
        /* Windows 11: one rounded card — "Monday, November 16" header (click = today)
           over a compact calendar with a round accent "today". ~ 336 × 360 */
        gtk_widget_set_size_request(cw->popup_win, 336, 360);
        cw->popup_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(cw->popup_root), 14);
        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *hb = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(hb), GTK_RELIEF_NONE);
        cw->header_date_lbl = gtk_label_new(NULL);
        gtk_container_add(GTK_CONTAINER(hb), cw->header_date_lbl);
        gtk_widget_set_tooltip_text(hb, "Go to today");
        g_signal_connect(hb, "clicked", G_CALLBACK(on_today_clicked), cw);
        gtk_box_pack_start(GTK_BOX(hdr), hb, FALSE, FALSE, 0);
        GtkWidget *cal = make_calendar(cw, "ult-cal-win11");
        gtk_box_pack_start(GTK_BOX(cw->popup_root), hdr, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(cw->popup_root), cal, TRUE, TRUE, 0);
    }

    gtk_container_add(GTK_CONTAINER(cw->popup_win), cw->popup_root);
    gtk_widget_show_all(cw->popup_root);
    cal_go_today(cw);
    if (cw->notif_list) fill_notification_list(cw);
    clock_tick(cw);
}

static void on_flyout_unmap(GtkWidget *w, ClockWidget *cw) { clock_schedule(cw); }

static void on_clock_toggled(GtkToggleButton *btn, ClockWidget *cw) {
    if (gtk_toggle_button_get_active(btn)) {
        cal_go_today(cw);                                  /* always opens on today */
        if (cw->notif_list) fill_notification_list(cw);
        if (cw->dnd_switch) gtk_switch_set_active(GTK_SWITCH(cw->dnd_switch), notifications_get_dnd());
    }
    /* GNOME mode: the clock sits in the middle, so its popup opens centred under it */
    g_object_set_data(G_OBJECT(cw->popup_win), "center-on-button",
                      (cw->shell && cw->shell->mode == LAYOUT_MACOS && cw->shell->gnome_style) ? GINT_TO_POINTER(1) : NULL);
    shell_toggle_popup_window(btn, cw->popup_win);
    clock_tick(cw);                                         /* header now; seconds ticking while open */
}

/* After a panel restart the label is first measured before the panel's font/CSS
   is in place, leaving the two-line clock off-centre until something re-lays it out
   (a click). Re-measure once the button is on screen and again whenever its style changes. */
static gboolean clock_relayout_idle(gpointer data) {
    ClockWidget *cw = data;
    GDateTime *now = clock_now(cw);
    update_panel(cw, now);
    g_date_time_unref(now);
    gtk_widget_queue_resize(cw->panel_time_lbl);
    gtk_widget_queue_resize(cw->btn);
    return G_SOURCE_REMOVE;
}
static void on_clock_btn_map(GtkWidget *w, gpointer data) { g_idle_add(clock_relayout_idle, data); }
static void on_clock_lbl_style(GtkWidget *w, gpointer data) { gtk_widget_queue_resize(w); }

GtkWidget *clock_widget_new(UltimateShell *shell) {
    ClockWidget *cw = g_new0(ClockWidget, 1);
    g_clock_widget = cw;
    cw->shell = shell;

    cw->btn = gtk_toggle_button_new();
    gtk_widget_set_name(cw->btn, "ultimate-clock-btn");
    gtk_button_set_relief(GTK_BUTTON(cw->btn), GTK_RELIEF_NONE);

    cw->panel_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(cw->panel_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(cw->panel_box, GTK_ALIGN_CENTER);
    cw->panel_time_lbl = gtk_label_new("");
    cw->panel_date_lbl = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(cw->panel_time_lbl), GTK_JUSTIFY_CENTER);
    gtk_label_set_justify(GTK_LABEL(cw->panel_date_lbl), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(cw->panel_box), cw->panel_time_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cw->panel_box), cw->panel_date_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(cw->btn), cw->panel_box);

    cw->popup_win = shell_create_popup_window(cw->btn, 320, 300, FALSE);
    g_signal_connect(cw->popup_win, "unmap", G_CALLBACK(on_flyout_unmap), cw);   /* back to per-minute */

    /* time-zone changes (Settings ▸ Date & Time, travelling) */
    GFile *lt = g_file_new_for_path("/etc/localtime");
    cw->tz_mon = g_file_monitor_file(lt, G_FILE_MONITOR_WATCH_HARD_LINKS, NULL, NULL);
    if (cw->tz_mon) g_signal_connect(cw->tz_mon, "changed", G_CALLBACK(on_tz_changed), cw);
    g_object_unref(lt);

    clock_widget_rebuild_style(shell);
    g_signal_connect(cw->btn, "toggled", G_CALLBACK(on_clock_toggled), cw);
    g_signal_connect(cw->btn, "map", G_CALLBACK(on_clock_btn_map), cw);
    g_signal_connect(cw->panel_time_lbl, "style-updated", G_CALLBACK(on_clock_lbl_style), NULL);
    return cw->btn;
}
