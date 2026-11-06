// Recycling cache the GL backends retire freed cel textures into.
//
// `cel_free` retires a texture instead of deleting it because the guest
// cycles palette and TRAM state while the same resident stays live, so a
// later upload is usually an image that was just released. A cached entry
// holds only the identity of the image and the GL texture that carries it:
// the identity is the extent, the sampler shape, and the 64-bit packed-RGBA
// identities the interpreter already computes while decoding. No CPU copy of
// the texels is retained, so a hit is a bind-and-reuse and a miss must upload.
//
// Two uploads are interchangeable exactly when their CelCacheKeys are equal.
// The content identity is a 64-bit FNV-1a over the decoded bytes -- the same
// bytes the upload carries -- so a key match is treated as the same image.
// Keeping a texel copy to re-compare would cost a full image copy per upload
// and a full read per hit to guard a 64-bit collision.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace m2host {

// What a cached texture is remembered as. `content_hash` is the FNV-1a
// identity of the uploaded level 0; `lod_chain_hash` is the identity of the
// guest-authored coarser levels and 0 for a single-level upload, so an
// authored chain is never served to a request that asked for the host's own
// reduction of level 0.
struct CelCacheKey {
    int w = 0;
    int h = 0;
    uint64_t content_hash = 0;
    uint64_t lod_chain_hash = 0;
    bool has_mips = false;
    bool wrap_repeat = false;

    bool operator==(const CelCacheKey&) const = default;
};

struct CelCacheKeyHash {
    size_t operator()(const CelCacheKey& key) const noexcept
    {
        size_t hash = mix(0, key.content_hash);
        hash = mix(hash, key.lod_chain_hash);
        hash = mix(hash, static_cast<uint64_t>(static_cast<uint32_t>(key.w)));
        hash = mix(hash, static_cast<uint64_t>(static_cast<uint32_t>(key.h)));
        hash = mix(hash, (key.has_mips ? 1u : 0u) | (key.wrap_repeat ? 2u : 0u));
        return hash;
    }

private:
    static size_t mix(size_t hash, uint64_t value)
    {
        return hash ^ (static_cast<size_t>(value) +
                       static_cast<size_t>(0x9e3779b97f4a7c15ull) +
                       (hash << 6) + (hash >> 2));
    }
};

inline CelCacheKey cel_cache_key(int w, int h, bool has_mips, bool wrap_repeat,
                                 uint64_t content_hash,
                                 uint64_t lod_chain_hash = 0)
{
    CelCacheKey key;
    key.w = w;
    key.h = h;
    key.content_hash = content_hash;
    key.lod_chain_hash = lod_chain_hash;
    key.has_mips = has_mips;
    key.wrap_repeat = wrap_repeat;
    return key;
}

// FIFO cache of retired textures, looked up by their image identity.
//
// `Record` is the backend's texture record. It is owned by the backend --
// including the GL deletion -- so nothing here touches GL: records the caller
// has to release come back through `released` instead of being destroyed,
// because a texture name is only valid in the context that created it.
template <typename Record>
class CelTextureCache {
public:
    static constexpr size_t kMaxRecords = 512u;
    // The byte budget bounds resident GPU textures, charged from the image
    // extent. Evicting early costs a re-upload, so it stays at the size
    // gameplay was tuned against rather than growing to whatever the device
    // could hold.
    static constexpr size_t kMaxBytes = 128u * 1024u * 1024u;

    struct Entry {
        CelCacheKey key;
        Record record;
    };

    // Remove and return the resident texture for `key`, if there is one.
    std::optional<Record> take(const CelCacheKey& key)
    {
        const auto entry = m_index.find(key);
        if (entry == m_index.end()) return std::nullopt;
        const auto record = entry->second;
        subtract_bytes(record->key);
        Record taken = std::move(record->record);
        m_records.erase(record);
        m_index.erase(entry);
        return taken;
    }

    // Retire `record` under `key`. A record whose image is already resident is
    // handed back through `released`, as are the oldest entries evicted to
    // stay inside the caps.
    void insert(const CelCacheKey& key, Record&& record,
                std::vector<Record>& released)
    {
        if (m_index.find(key) != m_index.end()) {
            released.push_back(std::move(record));
            return;
        }
        m_bytes += image_bytes(key);
        m_records.push_back(Entry{key, std::move(record)});
        m_index.emplace(key, std::prev(m_records.end()));
        while (m_records.size() > kMaxRecords || m_bytes > kMaxBytes) {
            Entry& oldest = m_records.front();
            subtract_bytes(oldest.key);
            released.push_back(std::move(oldest.record));
            m_index.erase(oldest.key);
            m_records.pop_front();
        }
    }

    size_t size() const { return m_records.size(); }
    size_t bytes() const { return m_bytes; }

    // Resident records, for the shutdown path that must delete every texture
    // before the context goes away.
    const std::list<Entry>& records() const { return m_records; }

    // Drop the bookkeeping without touching the textures.
    void clear()
    {
        m_records.clear();
        m_index.clear();
        m_bytes = 0;
    }

    // Bytes the retired texture occupies. An authored MIP chain is charged at
    // its level 0 extent only, matching the eviction the cache was tuned with.
    static size_t image_bytes(const CelCacheKey& key)
    {
        if (key.w <= 0 || key.h <= 0) return 0;
        return static_cast<size_t>(key.w) * static_cast<size_t>(key.h) *
               sizeof(uint32_t);
    }

private:
    void subtract_bytes(const CelCacheKey& key)
    {
        const size_t bytes = image_bytes(key);
        m_bytes = m_bytes > bytes ? m_bytes - bytes : 0;
    }

    std::list<Entry> m_records;
    std::unordered_map<CelCacheKey, typename std::list<Entry>::iterator,
                       CelCacheKeyHash>
        m_index;
    size_t m_bytes = 0;
};

}  // namespace m2host
