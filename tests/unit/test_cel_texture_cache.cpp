// Contract for the retired-texture cache the GL backends share.
//
// The cache keeps no CPU copy of the texels: an upload is matched to a
// resident texture by CelCacheKey alone, so every field of the key is a
// correctness boundary. These cases fail if a field stops separating images
// (two different uploads would then share one texture), and they pin the FIFO
// eviction order and the byte budget the backends size their residency by.
//
// Expected values are derived from the source contracts, never copied from
// renderer output.

#include "cel_texture_cache.h"

#include <cstdarg>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

int g_failures = 0;

bool expect(bool condition, const char* fmt, ...)
{
    if (condition) return true;
    ++g_failures;
    std::va_list args;
    va_start(args, fmt);
    std::fputs("cel_texture_cache: FAIL: ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
    return false;
}

// Stand-in for a backend record: the payload is the texture name it carries,
// and its identity is exactly the fields a backend remembers.
struct Record {
    int tex = 0;
    m2host::CelCacheKey key;

    m2host::CelCacheKey cache_key() const { return key; }
};

m2host::CelCacheKey key_for(uint64_t content_hash, uint64_t lod_chain_hash = 0)
{
    return m2host::cel_cache_key(16, 16, true, false, content_hash,
                                 lod_chain_hash);
}

Record record_for(int tex, const m2host::CelCacheKey& key)
{
    Record record;
    record.tex = tex;
    record.key = key;
    return record;
}

// Every field of the key decides whether two uploads are the same image. The
// negative controls mutate one field at a time against a resident entry.
bool field_separation()
{
    m2host::CelTextureCache<Record> cache;
    std::vector<Record> released;
    const m2host::CelCacheKey base = key_for(0x1234u);
    cache.insert(base, record_for(1, base), released);
    if (!expect(released.empty(), "field separation: insert releases nothing"))
        return false;

    const m2host::CelCacheKey variants[] = {
        m2host::cel_cache_key(16, 16, true, false, 0x1235u),           // content
        m2host::cel_cache_key(16, 16, true, false, 0x1234u, 0x99u),    // chain
        m2host::cel_cache_key(8, 16, true, false, 0x1234u),            // width
        m2host::cel_cache_key(16, 8, true, false, 0x1234u),            // height
        m2host::cel_cache_key(16, 16, false, false, 0x1234u),          // mips
        m2host::cel_cache_key(16, 16, true, true, 0x1234u),            // wrap
    };
    for (const m2host::CelCacheKey& variant : variants) {
        if (!expect(!cache.take(variant).has_value(),
                    "field separation: a differing field must miss"))
            return false;
    }
    if (!expect(cache.size() == 1u && cache.bytes() == 16u * 16u * 4u,
                "field separation: the resident entry is untouched "
                "(size=%zu bytes=%zu)",
                cache.size(), cache.bytes()))
        return false;
    const std::optional<Record> hit = cache.take(base);
    if (!expect(hit.has_value() && hit->tex == 1,
                "field separation: the unchanged key still hits"))
        return false;
    return expect(cache.size() == 0u && cache.bytes() == 0u,
                  "field separation: a hit returns its bytes "
                  "(size=%zu bytes=%zu)",
                  cache.size(), cache.bytes());
}

// A second copy of a resident image is handed back for deletion instead of
// being filed twice: the cache holds one texture per image identity.
bool duplicate_image()
{
    m2host::CelTextureCache<Record> cache;
    std::vector<Record> released;
    const m2host::CelCacheKey key = key_for(7u);
    cache.insert(key, record_for(1, key), released);
    cache.insert(key, record_for(2, key), released);
    if (!expect(released.size() == 1u && released[0].tex == 2,
                "duplicate: the second copy is released (released=%zu)",
                released.size()))
        return false;
    if (!expect(cache.size() == 1u && cache.bytes() == 16u * 16u * 4u,
                "duplicate: the image is counted once (size=%zu bytes=%zu)",
                cache.size(), cache.bytes()))
        return false;
    const std::optional<Record> hit = cache.take(key);
    return expect(hit.has_value() && hit->tex == 1,
                  "duplicate: the retained copy is the one served");
}

// Oldest first: a cache that is full retires the earliest inserted texture,
// whether or not a later one was taken out in between.
bool fifo_eviction()
{
    m2host::CelTextureCache<Record> cache;
    std::vector<Record> released;
    const size_t cap = m2host::CelTextureCache<Record>::kMaxRecords;
    for (size_t i = 0; i < cap; ++i)
        cache.insert(key_for(1000u + i), record_for(int(i), key_for(1000u + i)),
                     released);
    if (!expect(released.empty() && cache.size() == cap,
                "fifo: the capped set fits (size=%zu)", cache.size()))
        return false;
    // Taking from the middle must not reorder the eviction queue.
    if (!expect(cache.take(key_for(1000u + cap / 2u)).has_value(),
                "fifo: the middle entry is resident"))
        return false;
    cache.insert(key_for(2000u), record_for(-1, key_for(2000u)), released);
    if (!expect(released.empty() && cache.size() == cap,
                "fifo: refilling the taken slot evicts nothing (size=%zu)",
                cache.size()))
        return false;
    cache.insert(key_for(2001u), record_for(-2, key_for(2001u)), released);
    if (!expect(released.size() == 1u && released[0].tex == 0,
                "fifo: the oldest entry is evicted first (tex=%d)",
                released.empty() ? -3 : released[0].tex))
        return false;
    if (!expect(!cache.take(key_for(1000u)).has_value(),
                "fifo: the evicted entry is gone"))
        return false;
    if (!expect(cache.take(key_for(1001u)).has_value(),
                "fifo: the next-oldest entry is still resident"))
        return false;
    return expect(cache.size() == cap - 1u,
                  "fifo: eviction and hits account for every slot (size=%zu)",
                  cache.size());
}

// The byte budget is what bounds resident textures, and it is charged from
// the extent rather than a retained copy of the texels.
bool byte_budget()
{
    m2host::CelTextureCache<Record> cache;
    std::vector<Record> released;
    const size_t cap = m2host::CelTextureCache<Record>::kMaxBytes;
    // One image is half the record cap's worth of budget spread over many
    // entries, so the byte ceiling is the cap that binds here.
    const auto large_key = [](uint64_t content_hash) {
        return m2host::cel_cache_key(512, 256, true, false, content_hash);
    };
    const m2host::CelCacheKey key = large_key(1u);
    const size_t image = m2host::CelTextureCache<Record>::image_bytes(key);
    if (!expect(image == static_cast<size_t>(512) * 256u * 4u,
                "bytes: charged from the extent (%zu)", image))
        return false;
    const size_t count = cap / image + 4u;
    if (!expect(count < m2host::CelTextureCache<Record>::kMaxRecords,
                "bytes: the budget binds before the record cap (%zu)", count))
        return false;
    for (size_t i = 0; i < count; ++i) {
        const m2host::CelCacheKey entry = large_key(3000u + i);
        cache.insert(entry, record_for(int(i), entry), released);
    }
    if (!expect(cache.bytes() <= cap,
                "bytes: residency stays inside the budget (%zu > %zu)",
                cache.bytes(), cap))
        return false;
    // The first four inserts are the ones the budget pushed out.
    return expect(released.size() == 4u && released[0].tex == 0 &&
                      released[3].tex == 3,
                  "bytes: the oldest inserts are released first "
                  "(released=%zu first=%d last=%d)",
                  released.size(), released.empty() ? -1 : released[0].tex,
                  released.empty() ? -1 : released.back().tex);
}

}  // namespace

int main()
{
    bool ok = field_separation();
    ok = duplicate_image() && ok;
    ok = fifo_eviction() && ok;
    ok = byte_budget() && ok;
    if (g_failures == 0 && ok) {
        std::puts("cel_texture_cache: OK");
        return 0;
    }
    std::fprintf(stderr, "cel_texture_cache: %d failure(s)\n", g_failures);
    return 1;
}
