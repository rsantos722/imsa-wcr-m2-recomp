// Optional presentation textures, keyed by the original decoded RGBA image.
// The guest still decodes and owns its original texture, palette and state.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace m2host {

inline uint64_t texture_rgba_hash(const void* pixels, size_t pixel_count)
{
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const uint8_t*>(pixels);
    for (size_t i = 0; i < pixel_count * sizeof(uint32_t); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

class TextureReplacements {
public:
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> rgba;
    };
    bool empty() const { return m_entries.empty(); }

    // Each non-comment line is: original_width original_height 0xFNV64
    // "relative/or/absolute.png". An invalid mapping is diagnosed and
    // skipped; original assets remain available for that key.
    void load(const char* value)
    {
        if (!value || !*value) return;
        const std::filesystem::path manifest(value);
        std::ifstream input(manifest);
        if (!input) {
            std::fprintf(stderr,
                         "[texture-replacement] cannot open manifest %s\n",
                         value);
            return;
        }
        std::string line;
        unsigned line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            const size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#') continue;
            std::istringstream row(line);
            int width = 0, height = 0;
            std::string hash_text, image_path, trailing;
            if (!(row >> width >> height >> hash_text >> std::quoted(image_path)) ||
                (row >> trailing) || width <= 0 || height <= 0 ||
                width > 8192 || height > 8192 || image_path.empty()) {
                report_bad(manifest, line_number, "invalid row");
                continue;
            }
            uint64_t hash = 0;
            try {
                size_t parsed = 0;
                hash = std::stoull(hash_text, &parsed, 0);
                if (parsed != hash_text.size() || hash == 0u)
                    throw std::invalid_argument("hash");
            } catch (...) {
                report_bad(manifest, line_number, "invalid RGBA hash");
                continue;
            }
            const Key key{width, height, hash};
            if (m_entries.count(key) != 0u) {
                report_bad(manifest, line_number, "duplicate source key");
                continue;
            }
            std::filesystem::path path(image_path);
            if (path.is_relative()) path = manifest.parent_path() / path;
            Entry entry;
            entry.path = std::move(path);
            m_entries.emplace(key, std::move(entry));
        }
        std::fprintf(stderr,
                     "[texture-replacement] mappings=%zu manifest=%s\n",
                     m_entries.size(), value);
    }

    const Image* find(int width, int height, uint64_t hash)
    {
        auto it = m_entries.find(Key{width, height, hash});
        if (it == m_entries.end()) return nullptr;
        Entry& entry = it->second;
        if (entry.attempted) return entry.image.rgba.empty() ? nullptr
                                                              : &entry.image;
        entry.attempted = true;
        SDL_Surface* source = SDL_LoadPNG(entry.path.string().c_str());
        if (!source) {
            std::fprintf(stderr,
                         "[texture-replacement] cannot load %s: %s\n",
                         entry.path.string().c_str(), SDL_GetError());
            return nullptr;
        }
        SDL_Surface* converted = SDL_ConvertSurface(source,
                                                    SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(source);
        if (!converted) {
            std::fprintf(stderr,
                         "[texture-replacement] cannot convert %s: %s\n",
                         entry.path.string().c_str(), SDL_GetError());
            return nullptr;
        }
        const bool valid = converted->w >= width &&
                           converted->h >= height &&
                           converted->w <= 8192 && converted->h <= 8192 &&
                           static_cast<int64_t>(converted->w) * height ==
                               static_cast<int64_t>(converted->h) * width &&
                           converted->pitch >= converted->w * 4;
        if (!valid) {
            std::fprintf(stderr,
                         "[texture-replacement] %s must keep aspect ratio "
                         "and be at least %dx%d\n",
                         entry.path.string().c_str(), width, height);
            SDL_DestroySurface(converted);
            return nullptr;
        }
        entry.image.width = converted->w;
        entry.image.height = converted->h;
        entry.image.rgba.resize(static_cast<size_t>(converted->w) *
                                converted->h);
        for (int y = 0; y < converted->h; ++y) {
            const auto* row = static_cast<const uint8_t*>(converted->pixels) +
                              static_cast<size_t>(y) * converted->pitch;
            std::memcpy(entry.image.rgba.data() +
                            static_cast<size_t>(y) * converted->w,
                        row, static_cast<size_t>(converted->w) * 4u);
        }
        SDL_DestroySurface(converted);
        std::fprintf(stderr,
                     "[texture-replacement] loaded source=%dx%d/%#llx "
                     "replacement=%dx%d path=%s\n",
                     width, height, static_cast<unsigned long long>(hash),
                     entry.image.width, entry.image.height,
                     entry.path.string().c_str());
        return &entry.image;
    }

private:
    using Key = std::tuple<int, int, uint64_t>;
    struct Entry {
        std::filesystem::path path;
        bool attempted = false;
        Image image;
    };
    static void report_bad(const std::filesystem::path& manifest,
                           unsigned line, const char* reason)
    {
        std::fprintf(stderr, "[texture-replacement] %s:%u: %s\n",
                     manifest.string().c_str(), line, reason);
    }
    std::map<Key, Entry> m_entries;
};

}  // namespace m2host
