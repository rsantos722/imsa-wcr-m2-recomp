// audio_ogg.cpp — optional OGG music library + playback policy.
//
// See audio_ogg.h for the module split. This file deliberately contains no
// decoder: streaming lives in host/audio/ogg_stream.cpp (stb_vorbis), mixing
// in host/audio/audio_shim.cpp. Keeping resolution/policy here means callers
// never hardcode paths into the music folder.
#include "audio_ogg.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

#include "audio_shim.h"

namespace m2host::audio_ogg {

namespace {

struct Library {
    bool scanned = false;
    std::unordered_map<std::string, std::string> by_relpath;
    std::unordered_map<std::string, std::string> by_basename;
};

Library& lib() {
    static Library l;
    return l;
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

void scan_library(const std::string& converted_root) {
    Library& l = lib();
    l.scanned = true;
    l.by_relpath.clear();
    l.by_basename.clear();
    namespace fs = std::filesystem;
    size_t n = 0;
    for (const char* sub : {"audio", "sfx"}) {
        std::error_code ec;
        fs::path dir = fs::path(converted_root) / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != end(it); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() != ".ogg") continue;
            std::error_code rec;
            fs::path rel = fs::relative(it->path(), converted_root, rec);
            if (rec) continue;
            const std::string path_str = it->path().generic_string();
            // Key keeps the audio/sfx prefix ("audio/Music/ui_music") so the
            // two roots can't shadow each other; basenames stay global.
            std::string key = lower(
                (fs::path(sub) / rel).replace_extension().generic_string());
            std::string base = lower(it->path().stem().generic_string());
            if (l.by_relpath.emplace(key, path_str).second) ++n;
            l.by_basename.emplace(base, path_str);  // first basename wins
        }
    }
    std::printf("[audio-ogg] library: %zu ogg files indexed under %s\n", n,
                converted_root.c_str());
}

size_t library_size() { return lib().by_relpath.size(); }

bool exists(const std::string& name) { return !lookup_path(name).empty(); }

std::string lookup_path(const std::string& name) {
    Library& l = lib();
    if (!l.scanned || name.empty()) return {};
    const std::string key = lower(name);
    if (auto it = l.by_relpath.find(key); it != l.by_relpath.end())
        return it->second;
    if (auto it = l.by_basename.find(key); it != l.by_basename.end())
        return it->second;
    return {};
}

bool play(const std::string& name, AudioShim& shim, bool loop) {
    const std::string path = lookup_path(name);
    if (path.empty()) {
        std::fprintf(stderr, "[audio-ogg] not found: %s\n", name.c_str());
        return false;
    }
    if (!shim.music_open(std::filesystem::path(path))) return false;
    shim.music_play(loop);
    return true;
}

void stop(AudioShim& shim) { shim.music_stop(); }

}  // namespace m2host::audio_ogg
