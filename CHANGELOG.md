# Changelog

## 0.4.1 — 2026-11-22

### GNOME layout
- New Desktop Shell Paradigm: GNOME Top Bar + Bottom Dock. The top bar has the workspace pill and the focused app's name on the left, the clock in the centre, and the tray with Quick Settings on the right.
- No start button: the workspace pill and the dock's Show Apps button open the start menu you picked.
- The app name has a small menu: New Window and Quit.
- Shell themes can be set as the default dark or light theme; the Dark mode toggle switches between them.

### Dock
- Two dock styles: the macOS dock, and the Ubuntu dock (left edge, full height under the top bar, no zoom, apps and Trash from the top, Show Apps at the bottom, hover tiles and window previews stacked top to bottom).

### Start menu
- The Super (Windows) key opens the start menu (Settings ▸ Start Menu, on by default). A Super binding that was there before is put back when it is turned off.
- Shorter style names - List, Classic, Modern, Categories, Launchpad, Tiles, App Grid - with a
  one-line description under the list
- Keyboard everywhere: ↓ from search into the apps, ↑ back, Enter launches, Esc clears the search
  first; App Grid / Launchpad: arrows cross pages, Ctrl+Page Up/Down switch pages
- List: right-click ▸ Pin to top - pinned apps above A–Z, in your order
- Classic: Windows 7 power button - Shut down plus ▸ for Switch user / Log off / Lock / Restart / Sleep
- Modern: drag pinned apps with a live preview; Categories: drag Favourites into your own order
- Launchpad: 250 ms page slides, one page per wheel notch; Launchpad and App Grid icons grow in
  when opening and shrink away when closing
- Tiles: named tile groups - drag a tile below the last group to start one, click a name to
  rename it, empty groups go away
- App Grid: hold a dragged app at a side (or on a peek) for a second to turn the page
- New style **Windows 10 Tiles Menu**: rail (account, Documents, Pictures, Settings, Power),
  A–Z app list with letter headers (click one to jump to a letter) and pinned tiles -
  right-click a tile ▸ Resize ▸ Small / Medium / Wide, drag a tile onto another to move it;
  Grid Columns sets how many medium tiles fit in a row (default 4) and the menu
  width follows it
- New style **GNOME App Grid**: full screen, search on top, a strip of workspace maps
  showing each workspace's own wallpaper, the current one larger (click or scroll to switch;
  drop an app on one to open it there - the grid stays open and the new window shows up), then a paged A–Z grid -
  8 × 3 with 48 px icons by default. Timing and sizes follow GNOME Shell: pages slide in 250 ms,
  one page per wheel notch, the neighbouring pages peek in at the sides, workspace maps sized
  from the screen and resized in 200 ms, and the icons grow into place when it opens

### Animations
- Simpler setting: an *Animations* on/off tick plus two styles - *Fade* (popups fade in
  and out) and *Slide* (popups slide out of the bar and back into it)
- Popups now animate when they close too, not only when they open

### Quick Settings
- New option (Settings ▸ System Tray): Sound, Network and Power - plus Bluetooth if wanted, and the
  microphone while it is in use - become one group button next to the clock that opens one panel:
  battery (click for the Power page), sound output and microphone with players and apps, brightness,
  Wi-Fi and Bluetooth (▸ shows their lists in place), Do Not Disturb, Airplane Mode, Dark Mode,
  Night Light and Caffeine, plus buttons for System Settings and Ultimate Shell Settings

### System tray
- Older (XEmbed) tray icons - Notes, IBus, Wine and Java apps - now behave like the others: they can
  be hidden into the ▲ popup like any other icon, moved with drag and
  drop or Settings, remember their state, and get the same hover highlight

### Fixes
- Esc closes every shell popup: clock & calendar, the tray popups and Quick Settings.
- Settings window can be made much narrower: long drop-down texts shorten with "…" (the open
  list shows them in full) and long labels wrap; drop-down lists open below their button instead of covering it
- Light colours: the Apple menu's *Recent Items* submenu (and nested app sub-menus such as
  Terminal → Set Encoding) showed white text while their parent item was hovered
- Light colours: hovered menu items showed white text on the pale hover tint
- Clock: a negative line spacing made GTK print a size warning on every update; tighter
  lines now use the font's line height instead
- Top bar: the » overflow item was freed while still in use ("invalid unclassed pointer in
  cast to 'GtkMenuItem'" in the log)

## 0.4.0 — first public beta

### Appearance
- Unified colours: *Follow GTK theme*, *Light*, *Dark* or *Custom* (background, text,
  accent - all with transparency); hover and border colours automatic or custom
- One palette for panel, dock, popups, menus, global menu, search fields, check boxes,
  sliders, tooltips and the settings window
- Panel & dock opacity, popups & menus opacity
- Shell themes: Apply, Save as, Delete, Import, Export; built-in macOS / Windows 11
  Light and Dark (colours only - your shapes stay); "save your look changes to the
  theme?" when leaving the General page, with a *(modified)* marker
- GTK theme, icon theme, title-bar style, shell font, corner radius, popup border and
  shadow on the General page
- Frameless panel (no square edge line) with a shell-drawn panel shadow; dock and dock
  icon shadows drawn by the shell
- Indicators page: taskbar and dock styles, active / inactive / attention colours
- xfce4-panel's dark mode follows the shell colours

### Shell & layouts
- macOS and Windows layouts; four Windows panel layouts with a true centered group
- Workspace indicator (GNOME pills / dots / numbers) with its own settings page
- Animations: Off / Subtle / Full with speed; desktop-compositing switch
- One placement rule for every popup, menu and preview (6 px from the bar, beside it on side docks)

### Dock & taskbar
- App badges and progress bars (Unity LauncherEntry; XApp progress, e.g. Firefox downloads)
- Drop files on an icon to open them with that app
- Count badges redesigned (red for app counts, neutral for open windows)
- Icons update when the icon theme changes
- Maximized windows fill the screen (terminals no longer stop a text row short)
- Live window thumbnails on taskbar hover (Windows layout); name labels on dock hover (macOS)
- Fuller right-click menus (windows, app actions, admin, edit launcher, workspaces, force quit)
- Start / Launchpad quick menu (right-click): Task Manager, settings, terminal, power, …

### Start menu
- Same launcher as the taskbar (terminal apps, working folder, admin apps)
- Rebuilds itself when apps are installed or removed
- Search: keywords, program name, ranking, Enter, ↓, calculator, web search
- "New" badges and *Recently added*; optional *Recent & Recommended*
- Whisker icon view; Windows 11 opens on Pinned; drag to reorder pinned apps
- Exact category matching, accented A–Z headers, edge resizing

### Top bar
- Global menu shortcuts for GTK / GtkApplication apps (own GTK module)
- Window buttons left or right with all title-bar buttons following
- Menus that don't fit move into a » overflow menu; the app name stays readable

### System tray
- New sound popup (devices, now playing with album art and seek, per-app volume)
- Microphone-in-use indicator as its own tray icon
- Overflow menu: 5 columns, drag to rearrange, popups open beside it
- IBus as a movable modern tray icon
- Sound settings are now saved and restored correctly

### Fixes
- Panel heights no longer reset when switching layouts or restarting the panel
- Settings applied by a theme, an import or a page reset are no longer overwritten
  when the settings window closes
- Readable text on light themes (global menu, panel buttons, search boxes)
- Sound popup no longer widens with long media titles
- Win11 Start menu always opens on Pinned, also from the dock

### Clock & calendar
- Windows 7 / 10 / 11 and GNOME-macOS flyouts sized like the real ones
- Notifications in the GNOME flyout; date formats incl. custom; week numbers
- Exact ticking; follows time-zone changes; always opens on today
