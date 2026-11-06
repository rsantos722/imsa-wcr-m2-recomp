#include "save_data.h"
#include "platform.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace launcher_saves {
namespace fs = std::filesystem;
namespace {
struct File { uint64_t size = 0, hash = 14695981039346656037ULL; bool operator==(const File&) const = default; };
using Inventory = std::map<std::string, File>;
// FNV-1a detects accidental backup damage; this is not an authenticity signature.
File fingerprint(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read save file: " + m2host::platform::path_to_utf8(path));
    File result;
    std::array<char, 65536> buffer;
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        result.size += input.gcount();
        for (std::streamsize i=0; i<input.gcount(); ++i) {
            result.hash ^= static_cast<unsigned char>(buffer[i]); result.hash *= 1099511628211ULL;
        }
    }
    if (!input.eof()) throw std::runtime_error("Could not finish reading save data.");
    return result;
}
Inventory inventory(const fs::path& root, bool hash = true) {
    Inventory files;
    if (fs::is_symlink(fs::symlink_status(root))) throw std::runtime_error("Save folders must not be symbolic links.");
    if (!fs::exists(root)) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto status = entry.symlink_status();
        if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status)))
            throw std::runtime_error("Save data contains a link or unsupported file: " + m2host::platform::path_to_utf8(entry.path()));
        if (fs::is_regular_file(status)) files.emplace(entry.path().lexically_relative(root).generic_string(),
            hash ? fingerprint(entry.path()) : File{entry.file_size(), 0});
    }
    return files;
}
std::string unique_name() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto local = *std::localtime(&time);
    std::ostringstream s;
    s << std::put_time(&local, "%Y-%m-%d_%H-%M-%S") << '-'
      << std::chrono::steady_clock::now().time_since_epoch().count();
    return s.str();
}
struct Temporary {
    fs::path path;
    ~Temporary() { if (!path.empty()) { std::error_code ec; fs::remove_all(path, ec); } }
};
void copy_files(const fs::path& from, const fs::path& to, const Inventory& files) {
    fs::create_directories(to);
    for (const auto& [name, file] : files) {
        (void)file;
        const auto dest = to / name;
        fs::create_directories(dest.parent_path());
        fs::copy_file(from / name, dest);
    }
    if (inventory(to) != files) throw std::runtime_error("Save data changed or could not be copied completely. Please close the game and try again.");
}
void check_locations(const fs::path& root, const fs::path& directory) {
    const auto a = fs::weakly_canonical(root), b = fs::weakly_canonical(directory);
    auto within = [](const fs::path& child, const fs::path& parent) {
        auto r=child.lexically_relative(parent);
        return !r.empty() && *r.begin() != "..";
    };
    if (a == a.root_path() || within(a,b) || within(b,a))
        throw std::runtime_error("Save data and backups must be separate folders.");
}
fs::path make_backup(const fs::path& root, const fs::path& directory) {
    check_locations(root, directory);
    const auto files = inventory(root);
    fs::create_directories(directory);
    const auto dest = directory / unique_name();
    if (!fs::create_directory(dest)) throw std::runtime_error("Could not create backup folder.");
    Temporary pending{dest};
    copy_files(root, dest / "card", files);
    std::ofstream manifest(dest / "backup.txt");
    manifest << "IMSA-CARD 1\n";
    for (const auto& [name, file] : files)
        manifest << std::quoted(name) << ' ' << file.size << ' ' << file.hash << '\n';
    manifest.close();
    if (!manifest) throw std::runtime_error("Could not save backup record.");
    pending.path.clear();
    return dest;
}
Inventory read_backup(const fs::path& selected) {
    if (fs::is_symlink(fs::symlink_status(selected)) ||
        fs::is_symlink(fs::symlink_status(selected / "backup.txt")))
        throw std::runtime_error("Backup links are not supported.");
    std::ifstream manifest(selected / "backup.txt");
    std::string header;
    std::getline(manifest, header);
    if (header != "IMSA-CARD 1") throw std::runtime_error("This is not a complete save backup.");
    Inventory files;
    std::string line;
    while (std::getline(manifest, line)) {
        std::istringstream row(line);
        std::string name;
        File file;
        if (!(row >> std::quoted(name) >> file.size >> file.hash))
            throw std::runtime_error("The backup record is invalid.");
        row >> std::ws;
        const fs::path relative(name);
        if (!row.eof() || name.empty() || relative.is_absolute() ||
            relative != relative.lexically_normal() || name.find('\\') != std::string::npos ||
            std::find(relative.begin(), relative.end(), fs::path("..")) != relative.end() ||
            !files.emplace(name, file).second)
            throw std::runtime_error("The backup record is invalid.");
    }
    if (!manifest.eof() || !fs::is_directory(selected / "card") || inventory(selected / "card") != files)
        throw std::runtime_error("The backup is incomplete or damaged. Current saves have not been changed.");
    return files;
}
Result replace(const fs::path& root, const fs::path& directory, const fs::path& selected) {
    fs::path safety;
    try {
        check_locations(root, directory);
        Inventory files;
        if (!selected.empty()) {
            if (fs::weakly_canonical(selected.parent_path()) != fs::weakly_canonical(directory))
                throw std::runtime_error("Select a backup from this launcher's backup list.");
            files = read_backup(selected);
        }
        // Stage before touching the current card. The safety backup must succeed
        // before either restore or reset can publish a replacement.
        fs::create_directories(root.parent_path());
        fs::path staging = root;
        staging += ".new-" + unique_name();
        if (!fs::create_directory(staging)) throw std::runtime_error("Cannot stage save data.");
        Temporary pending{staging};
        copy_files(selected / "card", staging, files);
        safety = make_backup(root, directory);
        fs::path retired = root;
        retired += ".old-" + unique_name();
        const bool had_root = fs::exists(root);
        if (had_root) fs::rename(root, retired);
        try { fs::rename(staging, root); }
        catch (...) {
            if (had_root) fs::rename(retired, root);
            throw;
        }
        pending.path.clear();
        std::error_code ec;
        if (had_root) fs::remove_all(retired, ec);
        return {true, std::string(selected.empty() ? "Save data reset." : "Backup restored.") +
            " Previous saves are kept in backup " + safety.filename().string() +
            (ec ? ". A save folder could not be removed: " + m2host::platform::path_to_utf8(retired) : "."), safety};
    } catch (const std::exception& e) {
        return {false, std::string(e.what()) + (safety.empty() ? "" : " Safety backup: " + m2host::platform::path_to_utf8(safety)), safety};
    }
}
}
fs::path save_root(const fs::path& host_executable) {
    // Same precedence as m2host's FilesystemShim::init, so both see one card.
    // The default follows the host executable, not the launcher's own
    // directory: standalone m2host and the launcher-managed child must resolve
    // the same card even in CMake's split build tree.
    const fs::path override = m2host::platform::get_env_path("M2_MICROCARD_ROOT");
    if (!override.empty()) return fs::absolute(override);
    return fs::absolute(m2host::platform::default_microcard_dir_for(host_executable));
}
Summary inspect(const fs::path& root) {
    Summary result;
    try { for (const auto& [name, file] : inventory(root,false)) { (void)name; ++result.files; result.bytes+=file.size; } }
    catch (const std::exception& e) { result.error=e.what(); }
    return result;
}
std::vector<fs::path> backups(const fs::path& directory) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::is_directory(directory,ec)) return result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_directory() && !entry.is_symlink() && fs::is_regular_file(entry.path()/"backup.txt"))
            result.push_back(entry.path());
    }
    std::sort(result.rbegin(),result.rend());
    return result;
}
Result backup(const fs::path& root, const fs::path& directory) {
    try { auto dest=make_backup(root,directory); return {true,"Backup created: "+dest.filename().string(),dest}; }
    catch (const std::exception& e) { return {false,e.what(),{}}; }
}
Result restore(const fs::path& root, const fs::path& directory, const fs::path& selected) {
    if (selected.empty()) return {false,"Select a backup to restore.",{}};
    return replace(root,directory,selected);
}
Result reset(const fs::path& root, const fs::path& directory) { return replace(root,directory,{}); }
}
