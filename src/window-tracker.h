#ifndef WINDOW_TRACKER_H
#define WINDOW_TRACKER_H

#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <libwnck/libwnck.h>
#include <gtk/gtk.h>

typedef void (*WindowTrackerCallback)(WnckScreen *screen, WnckWindow *window, gpointer user_data);

typedef struct {
    WnckScreen *screen;
    gboolean    hide_titlebar_on_max;
} UltimateWindowTracker;

UltimateWindowTracker *window_tracker_new(void);
void window_tracker_set_hide_max_titlebar(UltimateWindowTracker *tracker, gboolean enable);

#endif