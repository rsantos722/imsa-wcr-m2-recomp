// config.cpp — tiny INI reader/writer (no external deps).
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace m2host {

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

using Sections = std::map<std::string, std::map<std::string, std::string>>;

Sections parse_ini(const std::filesystem::path& p)
{
    Sections out;
    std::ifstream f(p);
    if (!f) return out;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto hc = line.find('#');
        if (hc != std::string::npos) line = trim(line.substr(0, hc));
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return out;
}

}  // namespace

void Config::load()
{
    const Sections ini = parse_ini(m_path);
    const auto a = ini.find("audio");
    if (a != ini.end()) {
        if (auto it = a->second.find("music_enabled"); it != a->second.end())
            audio.music_enabled = std::atoi(it->second.c_str()) != 0;
        if (auto it = a->second.find("music_volume"); it != a->second.end())
            audio.music_volume = std::strtof(it->second.c_str(), nullptr);
    }
}

bool Config::save() const
{
    std::ofstream f(m_path);
    if (!f) return false;
    f << "# m2host settings (audio). Input bindings live in the launcher's controls.cfg\n";
    f << "[audio]\n";
    f << "music_enabled = " << (audio.music_enabled ? 1 : 0) << "\n";
    f << "music_volume = " << audio.music_volume << "\n";
    return true;
}

}  // namespace m2host
