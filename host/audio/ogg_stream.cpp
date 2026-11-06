// ogg_stream.cpp — see ogg_stream.h.
//
// The pushdata API hands back one vorbis frame at a time as interleaved
// float samples; we keep the tail in m_pending across read_s16 calls.
//
// stb_vorbis itself is compiled as its own TU from third_party/stb_vorbis.c by
// CMake; this TU includes the declarations-only wrapper. (Including the .c
// here too would define every stb symbol twice and fail to link.)
#include "ogg_stream.h"

#include "platform.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "stb_vorbis.h"
}

namespace m2host {

namespace {
constexpr int kChunk = 8192;  // bytes pulled from disk per decode round
}

bool OggStream::open(const std::filesystem::path& path)
{
    close();
    m_path = path;
    m_file = platform::fopen(path, "rb");
    if (!m_file) return false;

    std::vector<unsigned char> buf(kChunk);
    const size_t got = std::fread(buf.data(), 1, buf.size(), m_file);
    int used = 0, err = 0;
    m_vorbis = stb_vorbis_open_pushdata(buf.data(), static_cast<int>(got),
                                        &used, &err, nullptr);
    if (!m_vorbis) {
        std::fprintf(stderr, "[ogg] %s: open failed (err %d)\n",
                     platform::path_to_utf8(path.filename()).c_str(), err);
        std::fclose(m_file);
        m_file = nullptr;
        return false;
    }
    // Header may not have been fully consumed in the first chunk; seek back
    // to just after whatever open_pushdata used and let the frame loop take
    // over (stb re-syncs on Ogg page boundaries).
    if (used > 0)
        std::fseek(m_file, static_cast<long>(used), SEEK_SET);

    const stb_vorbis_info info = stb_vorbis_get_info(m_vorbis);
    m_channels = info.channels;
    m_rate = static_cast<int>(info.sample_rate);
    m_seconds = stb_vorbis_stream_length_in_seconds(m_vorbis);
    std::printf("[ogg] %s: %d ch @ %d Hz, %.1fs\n",
                platform::path_to_utf8(path.filename()).c_str(), m_channels,
                m_rate, m_seconds);
    return true;
}

void OggStream::close()
{
    if (m_vorbis) stb_vorbis_close(m_vorbis);
    m_vorbis = nullptr;
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
    m_pending.clear();
    m_pending_pos = 0;
    m_eof = false;
}

bool OggStream::rewind()
{
    if (!m_file) return false;
    if (m_vorbis) stb_vorbis_close(m_vorbis);
    m_vorbis = nullptr;
    m_pending.clear();
    m_pending_pos = 0;
    m_eof = true;
    std::fseek(m_file, 0, SEEK_SET);
    std::vector<unsigned char> buf(kChunk);
    const size_t got = std::fread(buf.data(), 1, buf.size(), m_file);
    int used = 0, err = 0;
    m_vorbis = stb_vorbis_open_pushdata(buf.data(), static_cast<int>(got),
                                        &used, &err, nullptr);
    if (!m_vorbis) {
        // A stream that cannot be reopened is exhausted, not loopable. Leaving
        // m_vorbis null with m_file still open made read_s16() retry forever.
        std::fprintf(stderr, "[ogg] %s: rewind failed (err %d)\n",
                     platform::path_to_utf8(m_path.filename()).c_str(), err);
        return false;
    }
    if (used > 0)
        std::fseek(m_file, static_cast<long>(used), SEEK_SET);
    m_eof = false;
    return true;
}

bool OggStream::fill_pending()
{
    if (!m_vorbis || !m_file) return false;
    if (m_pending_pos < m_pending.size()) return true;

    m_pending.clear();
    m_pending_pos = 0;

    unsigned char in[kChunk];
    while (m_pending.empty() && !m_eof) {
        const size_t got = std::fread(in, 1, sizeof(in), m_file);
        if (got == 0) {
            m_eof = true;
            break;
        }
        int used = 0, channels_out = 0, samples = 0;
        float** chan = nullptr;
        // stb_vorbis_decode_frame_pushdata: returns bytes consumed; samples
        // are per-channel frames written to *chan.
        used = stb_vorbis_decode_frame_pushdata(
            m_vorbis, in, static_cast<int>(got), &channels_out, &chan,
            &samples);
        if (used > 0) {
            // Rewind file to just after used bytes (pushdata does not own
            // the buffer; we re-read from after it next round).
            const long here = std::ftell(m_file);
            std::fseek(m_file, here - static_cast<long>(got) + used,
                       SEEK_SET);
        }
        if (samples > 0 && chan) {
            m_pending.resize(static_cast<size_t>(samples) * m_channels);
            for (int s = 0; s < samples; ++s)
                for (int c = 0; c < m_channels; ++c)
                    m_pending[static_cast<size_t>(s) * m_channels + c] =
                        chan[c][s];
        }
    }
    return !m_pending.empty();
}

int OggStream::read_s16(int16_t* dst, int frames, bool loop)
{
    int written = 0;
    while (written < frames) {
        if (!fill_pending()) {
            // Only loop when the rewind actually produced decodable audio;
            // a truncated or replaced file must end the read, not spin the
            // audio callback forever.
            if (loop && m_file && rewind() && fill_pending()) continue;
            break;
        }
        while (m_pending_pos < m_pending.size() && written < frames) {
            for (int c = 0; c < m_channels; ++c) {
                const float f = m_pending[m_pending_pos + c];
                long v = std::lrintf(f * 32767.f);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                dst[static_cast<size_t>(written) * m_channels + c] =
                    static_cast<int16_t>(v);
            }
            m_pending_pos += m_channels;
            ++written;
        }
    }
    return written;
}

}  // namespace m2host
