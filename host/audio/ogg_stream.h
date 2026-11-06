// ogg_stream.h — streaming Ogg Vorbis decoder for background music.
//
// Pull model: the mixer asks for N frames, we decode on demand from the
// file via stb_vorbis's pushdata API — nothing is decoded while the track is
// stopped and no full-song PCM buffer is ever resident (the CBD2→WAV path
// needed ~70 MB per song; OGG streams need a ~64 KB ring).
// Decoder: public-domain stb_vorbis.c v1.22 (third_party/).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct stb_vorbis;

namespace m2host {

class OggStream {
public:
    ~OggStream() { close(); }

    // Opens the file and parses headers; returns false on malformed input.
    bool open(const std::filesystem::path& path);
    void close();
    bool ok() const { return m_vorbis != nullptr; }

    int channels() const { return m_channels; }
    int rate() const { return m_rate; }
    float seconds() const { return m_seconds; }

    // Interleaved s16 frames into dst (max `frames`); returns frames written.
    // Loops seamlessly at EOF when `loop` is set; returns 0 only when not
    // looping and the file is exhausted or on error.
    int read_s16(int16_t* dst, int frames, bool loop);

    // Returns false when the stream cannot be reopened; the caller must treat
    // that as EOF rather than retrying.
    bool rewind();

private:
    bool fill_pending();  // decode one vorbis frame if none pending

    std::filesystem::path m_path;
    FILE* m_file = nullptr;
    stb_vorbis* m_vorbis = nullptr;
    int m_channels = 2;
    int m_rate = 44100;
    float m_seconds = 0.f;
    bool m_eof = false;

    // Pending decoded samples (interleaved floats from the pushdata API).
    std::vector<float> m_pending;
    size_t m_pending_pos = 0;
};

}  // namespace m2host
