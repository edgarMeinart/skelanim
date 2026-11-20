#include "app/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

std::string defaultSettingsPath() {
    fs::path dir;
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA")) dir = fs::path(appData) / "SkelAnim";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) dir = fs::path(home) / "Library" / "Application Support" / "SkelAnim";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        dir = fs::path(xdg) / "skelanim";
    else if (const char* home = std::getenv("HOME"))
        dir = fs::path(home) / ".config" / "skelanim";
#endif
    return dir.empty() ? std::string() : (dir / "settings.ini").string();
}

void loadSettings(const std::string& path, Settings& out) {
    if (path.empty()) return;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "show_tutorial") out.showTutorial = value != "0";
    }
}

bool saveSettings(const std::string& path, const Settings& s) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << "show_tutorial=" << (s.showTutorial ? 1 : 0) << "\n";
    return bool(out);
}
