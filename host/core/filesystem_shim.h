// filesystem_shim.h — filesystem folio (id 4) shim over the host FS.
#pragma once

#include <filesystem>
#include <string>

namespace m2host {

class FilesystemShim {
public:
    // root = directory that game-relative paths resolve against
    // (normally <game-data>/driving/data; see data_paths.h).
    bool init(const std::filesystem::path& root);
    void shutdown();

    const std::filesystem::path& root() const { return m_root; }
    bool root_ok() const { return m_root_ok; }

    const std::filesystem::path& microcard_root() const { return m_microcard; }
    const std::filesystem::path& screenshot_root() const { return m_screenshots; }

private:
    std::filesystem::path m_root;
    std::filesystem::path m_microcard;
    std::filesystem::path m_screenshots;
    bool m_root_ok = false;
};

}  // namespace m2host
