// config.h — persisted host settings (m2host.cfg in the launcher settings folder).
#pragma once

#include <filesystem>
#include <string>

namespace m2host {

struct AudioSettings {
    bool music_enabled = true;
    float music_volume = 0.6f;
};

class Config {
public:
    AudioSettings audio;

    // Path defaults to <cwd>/m2host.cfg. The host overrides this at startup
    // with the launcher's per-user settings folder.
    void set_path(const std::filesystem::path& p) { m_path = p; }
    const std::filesystem::path& path() const { return m_path; }

    void load();            // missing file = keep defaults
    bool save() const;

private:
    std::filesystem::path m_path = "m2host.cfg";
};

}  // namespace m2host
