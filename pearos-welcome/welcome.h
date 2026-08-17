// Business-logic helpers for the pearOS Welcome content pages -- ported from
// ../welcome.h/.cpp (the real Qt6 pearOS Welcome app), minus the pieces this
// shell deliberately doesn't carry: is_electron_window_visible()/
// is_online() (only used to gate the QWebEngineView "What's New" window,
// which this shell skips entirely -- "What's New?" opens the real site in
// the default browser instead, via open_url()) and the QuickUpdatePage
// webview zip-download flow (same reason).
//
// Ported from the Arch/pacman edition to Debian/apt: see welcome.cpp's
// header comment for what changed and why.

#ifndef WELCOME_H
#define WELCOME_H

#include <QString>
#include <QStringList>

QString get_base_path();
QString get_os_version();

QString get_desktop_environment();
bool check_if_live_iso();
void open_url(const QString &url);

bool autostart_file_exists();
void toggle_autostart(bool enable);

void screen_resolution(const QString &desktop);
void update_system(const QString &desktop);
void fix_liquid_gel_after_upgrade(const QString &desktop);
bool fix_layout(QStringList *failed = nullptr);
void run_bin_install();

// Reinstalls the pearOS apt archive keyring file -- a single pkexec root
// shell, for when /etc/apt/keyrings/pearos-archive-keyring.gpg has gone
// missing or corrupt (the apt equivalent of a broken pacman keyring; apt's
// trust store is just this one static file, so there's no pacman-key-style
// init/populate dance to redo -- just re-lay the key down).
void fix_apt_keys(const QString &desktop);

#endif // WELCOME_H
