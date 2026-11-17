#ifndef LAUNCHER_ENTRY_H
#define LAUNCHER_ENTRY_H

#include <glib.h>

/* Unity LauncherEntry API (com.canonical.Unity.LauncherEntry): apps such as
   Thunderbird, Telegram, Firefox downloads or Transmission send an unread
   count, a progress value and an urgent flag for their launcher icon. */

typedef struct {
    gint64   count;
    gboolean count_visible;
    gdouble  progress;          /* 0.0 - 1.0 */
    gboolean progress_visible;
    gboolean urgent;
} LauncherEntryState;

typedef void (*LauncherEntryChangedFunc)(gpointer user_data);

/* Start listening on the session bus. Safe to call more than once. */
void launcher_entry_init(LauncherEntryChangedFunc on_changed, gpointer user_data);

/* Look up the state for a group key (already normalized the same way the
   taskbar normalizes desktop ids). Returns FALSE when nothing is known. */
gboolean launcher_entry_lookup(const gchar *norm_key, LauncherEntryState *out);

/* Same normalization the taskbar uses: lower case, no ".desktop",
   reverse-DNS ids reduced to their last segment. */
gchar *launcher_entry_normalize_key(const gchar *raw);

#endif
