#pragma once

#include <string>

// Per-user editor preferences, stored as `key=value` lines.
struct Settings {
    bool showTutorial = true;
};

// <config dir>/SkelAnim/settings.ini: %APPDATA% on Windows, ~/Library/Application Support on
// macOS, $XDG_CONFIG_HOME or ~/.config elsewhere. Empty if no home directory is known.
std::string defaultSettingsPath();

// Missing file or unknown keys leave defaults in place.
void loadSettings(const std::string& path, Settings& out);
bool saveSettings(const std::string& path, const Settings& s);
