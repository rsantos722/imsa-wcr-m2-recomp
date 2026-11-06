// mpeg_decoder.h — mpegaudiodecoder folio shim backed by FFmpeg.
//
// The game imports this folio through five R_PPC_IMPREL24 call sites. The
// typed host boundary below preserves the MPAudioDecoder contract;
// without FFmpeg the class compiles to an always-unavailable stub.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// Global-scope FFmpeg opaque types (must not land inside namespace m2host).
struct AVCodecContext;
struct AVCodecParserContext;
struct AVPacket;
struct AVFrame;

namespace m2host {

struct MpegAudioFrame {
    // Interleaved host-native S16 stereo. Mono source frames are duplicated
    // to the second channel at the platform boundary, matching the original
    // SoundSpooler output contract.
    std::vector<int16_t> pcm;
    uint32_t header = 0;
    uint32_t pts = 0;
    uint32_t pts_valid = 0;
};

class MpegDecoder {
public:
    MpegDecoder() = default;
    ~MpegDecoder();
    MpegDecoder(const MpegDecoder&) = delete;
    MpegDecoder& operator=(const MpegDecoder&) = delete;

    bool available() const { return m_available; }

    // Open the underlying MPEG-1 Layer II / 44.1 kHz codec context. These
    // are the only audio parameters accepted by the original MPA decoder.
    bool open();
    void close();
    void flush();

    // Feed arbitrary-length compressed data; appends all decoded interleaved
    // stereo S16 frames to out_pcm. The optional timing pair belongs to the
    // first frame that begins in this input buffer, as in MPAGetCompressedBfr.
    // Returns bytes of PCM appended, or -1 for a source-invalid frame.
    int decode(const uint8_t* data, int size, std::vector<int16_t>& out_pcm,
               uint32_t pts = 0, uint32_t pts_valid = 0);

    // MPAudioDecode returns one decompressed frame per call. The parser may
    // produce several frames from one subscriber chunk, so the production
    // shim drains this FIFO one frame at a time.
    bool pop_frame(MpegAudioFrame& frame);
    size_t queued_frames() const { return m_ready_frames.size(); }

private:
    AVCodecContext* m_ctx = nullptr;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    AVCodecParserContext* m_parser = nullptr;
    int m_codec_id = 0;
    bool m_parser_has_partial = false;
    std::deque<MpegAudioFrame> m_ready_frames;
    bool m_available = false;
};

struct DecodedVideoFrame {
    std::vector<uint8_t> rgb555;
    uint32_t pts = 0;
    uint32_t user_data = 0;
};

class MpegVideoDecoder {
public:
    MpegVideoDecoder();
    ~MpegVideoDecoder();

    bool available() const { return m_available; }
    bool open();
    void close();
    void reset();
    void flush();

    // Reproduce the released MPEGV device's persistent client-buffer mode.
    // Nonzero client-control flags make later sequence headers fail with the
    // M2 NOMEM/PARAMERROR identities when their derived requirements do
    // not fit; both flags clear the mode and restore automatic allocation.
    int configure_buffers(uint32_t ref_min_size, uint32_t strip_min_size,
                          bool ref_client_control,
                          bool strip_client_control);

    // Feeds compressed MPEG-1 video payload. Returns number of frames decoded, or -1 on error.
    int feed(const uint8_t* data, size_t len, uint32_t flags, uint32_t pts, uint32_t user_data);

    // Checks whether at least one decoded frame is available in the queue.
    bool has_frame();

    // Pops the oldest decoded frame only when dst_buf can hold the complete
    // RGB555 image. Writes pts and user_data to output pointers if non-null.
    // Returns 1 if a frame was popped, 0 if the queue was empty or the
    // destination contract was invalid.
    int pop_frame(uint8_t* dst_buf, size_t dst_len, uint32_t* pts_out, uint32_t* user_data_out);

private:
    AVCodecContext* m_ctx = nullptr;
    AVCodecParserContext* m_parser = nullptr;
    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame = nullptr;
    std::deque<DecodedVideoFrame> m_frames;
    std::mutex m_mutex;
    bool m_available = false;
    bool m_first_frame = true;
    uint32_t m_last_pts = 0;
    uint32_t m_ref_capacity = 0;
    uint32_t m_strip_capacity = 0;
    bool m_ref_client_control = false;
    bool m_strip_client_control = false;
    bool m_wait_sequence_header = false;
    std::vector<uint8_t> m_sequence_probe_tail;

    // Locked internals: every caller must already hold m_mutex. feed() opens
    // under the same lock that guards use so a concurrent close() cannot null
    // m_ctx/m_parser between the availability check and the codec call.
    bool open_locked();
    void close_locked();
};

}  // namespace m2host

extern "C" {
int m2_mpegaudio_dispatch(struct PPCContext* c, uint32_t ordinal,
                          uint32_t pc);
int m2_decode_mpeg_still(const uint8_t* bitstream, size_t bitstream_size,
                         uint8_t* dst_buffer, size_t dst_buffer_size,
                         int width, int height);

int m2_mpeg_video_feed(const uint8_t* data, size_t len, uint32_t flags,
                       uint32_t pts, uint32_t user_data);
int m2_mpeg_video_configure_buffers(uint32_t ref_min_size,
                                    uint32_t strip_min_size,
                                    uint32_t ref_client_control,
                                    uint32_t strip_client_control);
int m2_mpeg_video_has_frame(void);
int m2_mpeg_video_pop_frame(uint8_t* dst_buf, size_t dst_len,
                            uint32_t* pts_out, uint32_t* user_data_out);
void m2_mpeg_video_flush(void);
void m2_mpeg_video_reset(void);
}
