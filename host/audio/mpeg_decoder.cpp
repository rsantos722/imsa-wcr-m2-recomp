// mpeg_decoder.cpp — FFmpeg MPEG-audio decode path (see header).
//
// Output conversion is done manually (planar float -> interleaved S16) to
// avoid a libswresample dependency in the scaffold.
#include "mpeg_decoder.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "folio_loader.h"
#include "mpeg_still_packet.h"
#include "ppc_ctx.h"

extern "C" {
void FN_GetCompressedBfr__63be8(PPCContext* c);
void FN_CompressedBfrRead__63c9c(PPCContext* c);
}

extern "C" {
#include "shim_abi.h"
}

#if M2_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}
#define FFMPEG_GUARD 1
#else
#define FFMPEG_GUARD 0
#endif

namespace m2host {

namespace {

static uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// The released MPA decoder is deliberately narrower than a general FFmpeg
// MPEG-audio decoder: mpaudiodecode.c accepts ISO/IEC 11172-3 (MPEG-1), Layer
// II, at its only supported 44.1 kHz sample rate. Keep that decision at the
// host boundary instead of silently accepting a different layer/rate.
static bool source_header_supported(const uint8_t* data, int size) {
    if (!data || size < 4) return false;
    const uint32_t h = read_be32(data);
    const uint32_t sync = (h >> 20) & 0xfffu;
    const uint32_t id = (h >> 19) & 1u;
    const uint32_t layer = (h >> 17) & 3u;
    const uint32_t bitrate_index = (h >> 12) & 0xfu;
    const uint32_t sample_rate_index = (h >> 10) & 3u;
    return sync == 0xfffu && id == 1u && layer == 2u &&
           bitrate_index != 0u && bitrate_index != 0xfu &&
           sample_rate_index == 0u;
}

#if FFMPEG_GUARD
static void reset_parser(AVCodecParserContext*& parser, int codec_id) {
    if (parser) av_parser_close(parser);
    parser = codec_id != 0 ? av_parser_init(static_cast<AVCodecID>(codec_id))
                           : nullptr;
}
#endif

}  // namespace

MpegDecoder::~MpegDecoder() {
    close();
}

bool MpegDecoder::open() {
#if FFMPEG_GUARD
    if (m_available) return true;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MP2);
    if (!codec) {
        std::printf("[mpeg] no MPEG audio decoder in libavcodec\n");
        return false;
    }
    m_codec_id = codec->id;
    m_parser = av_parser_init(codec->id);
    if (!m_parser) {
        m_codec_id = 0;
        std::printf("[mpeg] no MPEG audio parser in libavcodec\n");
        return false;
    }
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx || avcodec_open2(m_ctx, codec, nullptr) < 0) {
        if (m_ctx) avcodec_free_context(&m_ctx);
        av_parser_close(m_parser);
        m_parser = nullptr;
        m_codec_id = 0;
        return false;
    }
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_pkt || !m_frame) {
        close();
        return false;
    }
    m_parser_has_partial = false;
    m_available = true;
    std::printf("[mpeg] decoder ready (%s)\n", codec->name);
    return true;
#else
    std::printf("[mpeg] built without FFmpeg: decoder unavailable\n");
    return false;
#endif
}

void MpegDecoder::close() {
#if FFMPEG_GUARD
    if (m_frame) av_frame_free(&m_frame);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_ctx) avcodec_free_context(&m_ctx);
    if (m_parser) av_parser_close(m_parser);
    m_frame = nullptr;
    m_pkt = nullptr;
    m_ctx = nullptr;
    m_parser = nullptr;
#endif
    m_codec_id = 0;
    m_parser_has_partial = false;
    m_available = false;
    m_ready_frames.clear();
}

void MpegDecoder::flush() {
#if FFMPEG_GUARD
    if (m_ctx) avcodec_flush_buffers(m_ctx);
    if (m_pkt) av_packet_unref(m_pkt);
    if (m_frame) av_frame_unref(m_frame);
    if (m_parser) reset_parser(m_parser, m_codec_id);
#endif
    m_parser_has_partial = false;
    m_ready_frames.clear();
}

int MpegDecoder::decode(const uint8_t* data, int size,
                        std::vector<int16_t>& out_pcm, uint32_t pts,
                        uint32_t pts_valid) {
#if FFMPEG_GUARD
    if (!m_available || !m_parser || !m_ctx || !data || size <= 0) return 0;
    int appended = 0;
    bool emitted_from_this_input = false;
    bool bytes_after_last_packet = m_parser_has_partial;
    const uint8_t* cursor = data;
    int remaining = size;

    while (remaining > 0) {
        uint8_t* packet_data = nullptr;
        int packet_size = 0;
        const int consumed = av_parser_parse2(
            m_parser, m_ctx, &packet_data, &packet_size, cursor, remaining,
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed < 0 || consumed > remaining) {
            flush();
            return -1;
        }
        if (consumed == 0 && packet_size == 0) break;
        cursor += consumed;
        remaining -= consumed;
        if (consumed > 0) bytes_after_last_packet = true;
        if (packet_size <= 0) continue;

        if (!source_header_supported(packet_data, packet_size)) {
            flush();
            return -1;
        }
        if (av_new_packet(m_pkt, packet_size) != 0) {
            flush();
            return -1;
        }
        std::memcpy(m_pkt->data, packet_data, static_cast<size_t>(packet_size));
        const int send_err = avcodec_send_packet(m_ctx, m_pkt);
        av_packet_unref(m_pkt);
        if (send_err < 0) {
            flush();
            return -1;
        }
        bytes_after_last_packet = false;

        while (true) {
            const int receive_err = avcodec_receive_frame(m_ctx, m_frame);
            if (receive_err == AVERROR(EAGAIN) || receive_err == AVERROR_EOF)
                break;
            if (receive_err < 0) {
                flush();
                return -1;
            }

            const AVSampleFormat fmt = static_cast<AVSampleFormat>(m_frame->format);
            const int ch = m_frame->ch_layout.nb_channels;
            const int n = m_frame->nb_samples;
            if (m_frame->sample_rate != 44100 || (ch != 1 && ch != 2) ||
                n != 1152) {
                av_frame_unref(m_frame);
                flush();
                return -1;
            }

            MpegAudioFrame decoded;
            decoded.header = read_be32(packet_data);
            decoded.pts = pts;
            decoded.pts_valid = (!emitted_from_this_input &&
                                 !m_parser_has_partial) ? pts_valid : 0u;
            decoded.pcm.reserve(static_cast<size_t>(n) * 2u);
            auto c16 = [](float f) {
                int v = static_cast<int>(f * 32768.0f);
                return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            };
            if (fmt == AV_SAMPLE_FMT_FLTP) {
                const float* left = reinterpret_cast<const float*>(m_frame->data[0]);
                const float* right = ch > 1
                    ? reinterpret_cast<const float*>(m_frame->data[1]) : left;
                for (int i = 0; i < n; ++i) {
                    decoded.pcm.push_back(static_cast<int16_t>(c16(left[i])));
                    decoded.pcm.push_back(static_cast<int16_t>(c16(ch > 1 ? right[i] : left[i])));
                }
            } else if (fmt == AV_SAMPLE_FMT_S16P) {
                const int16_t* left = reinterpret_cast<const int16_t*>(m_frame->data[0]);
                const int16_t* right = ch > 1
                    ? reinterpret_cast<const int16_t*>(m_frame->data[1]) : left;
                for (int i = 0; i < n; ++i) {
                    decoded.pcm.push_back(left[i]);
                    decoded.pcm.push_back(ch > 1 ? right[i] : left[i]);
                }
            } else if (fmt == AV_SAMPLE_FMT_S16) {
                const int16_t* samples = reinterpret_cast<const int16_t*>(m_frame->data[0]);
                for (int i = 0; i < n; ++i) {
                    decoded.pcm.push_back(samples[i * ch]);
                    decoded.pcm.push_back(ch > 1 ? samples[i * ch + 1] : samples[i * ch]);
                }
            } else if (fmt == AV_SAMPLE_FMT_FLT) {
                const float* samples = reinterpret_cast<const float*>(m_frame->data[0]);
                for (int i = 0; i < n; ++i) {
                    decoded.pcm.push_back(static_cast<int16_t>(c16(samples[i * ch])));
                    decoded.pcm.push_back(static_cast<int16_t>(c16(
                        ch > 1 ? samples[i * ch + 1] : samples[i * ch])));
                }
            } else {
                av_frame_unref(m_frame);
                flush();
                return -1;
            }

            appended += static_cast<int>(decoded.pcm.size() * sizeof(int16_t));
            out_pcm.insert(out_pcm.end(), decoded.pcm.begin(), decoded.pcm.end());
            m_ready_frames.push_back(std::move(decoded));
            emitted_from_this_input = true;
            av_frame_unref(m_frame);
        }
    }
    m_parser_has_partial = bytes_after_last_packet;
    return appended;
#else
    (void)data;
    (void)size;
    (void)pts;
    (void)pts_valid;
    (void)out_pcm;
    return 0;
#endif
}

bool MpegDecoder::pop_frame(MpegAudioFrame& frame) {
    if (m_ready_frames.empty()) return false;
    frame = std::move(m_ready_frames.front());
    m_ready_frames.pop_front();
    return true;
}

}  // namespace m2host

// --- folio wiring -----------------------------------------------------------------
// The guest's five R_PPC_IMPREL24 imports are the platform-facing
// mpegaudiodecoder contract. The selector dispatcher below invokes the
// original subscriber callbacks for buffer ownership and uses the host MPEG
// decoder only for the module's compressed-audio operation.

static m2host::MpegDecoder g_mpeg;
static std::unordered_map<uint32_t, std::unique_ptr<m2host::MpegDecoder>>
    g_mpa_contexts;

namespace {

constexpr uint32_t kMpaDecoderTokenBase = 0x003f2000u;
constexpr uint32_t kMpaDecoderTokenStride = 0x100u;
constexpr uint32_t kMpaDecoderTokenLimit = 0x003f3000u;
constexpr uint32_t kMpaScratchBytes = 0x18u;
constexpr uint32_t kMpaSamplesPerFrame = 1152u;

static m2host::MpegDecoder* mpa_context(uint32_t token) {
    const auto it = g_mpa_contexts.find(token);
    return it == g_mpa_contexts.end() ? nullptr : it->second.get();
}

static uint32_t alloc_mpa_context_token() {
    for (uint32_t token = kMpaDecoderTokenBase;
         token < kMpaDecoderTokenLimit; token += kMpaDecoderTokenStride) {
        if (g_mpa_contexts.find(token) == g_mpa_contexts.end()) return token;
    }
    return 0u;
}

static bool guest_range(const PPCContext* c, uint32_t ea, uint32_t bytes) {
    return c && c->mem && ea <= RECOMP_GUEST_MEM &&
           bytes <= RECOMP_GUEST_MEM - ea;
}

static int32_t write_mpa_frame(PPCContext* c, uint32_t output_buffer,
                               uint32_t header, uint32_t pts_out,
                               uint32_t pts_valid_out,
                               const m2host::MpegAudioFrame& frame) {
    constexpr size_t kStereoSamples =
        static_cast<size_t>(kMpaSamplesPerFrame) * 2u;
    if (frame.pcm.size() != kStereoSamples)
        return -1;

    wr32(c, header, frame.header);
    wr32(c, pts_out, frame.pts);
    wr32(c, pts_valid_out, frame.pts_valid);
    for (uint32_t i = 0; i < kMpaSamplesPerFrame; ++i) {
        const int16_t left = frame.pcm[static_cast<size_t>(i) * 2u];
        const int16_t right = frame.pcm[static_cast<size_t>(i) * 2u + 1u];
        const uint32_t packed =
            (static_cast<uint32_t>(static_cast<uint16_t>(left)) << 16) |
            static_cast<uint16_t>(right);
        wr32(c, output_buffer + i * 4u, packed);
    }
    return 0;
}

static int32_t release_mpa_input(PPCContext* c, uint32_t the_unit,
                                 uint32_t input) {
    // The callback owns/replies the compressed buffer after the decoder has
    // copied it into its persistent parser state. This is also required on a
    // malformed input path so the subscriber queue cannot be stranded.
    c->gpr[3] = the_unit;
    c->gpr[4] = input;
    FN_CompressedBfrRead__63c9c(c);
    return static_cast<int32_t>(c->gpr[3]);
}

static int32_t mpa_decode_one(PPCContext* c, m2host::MpegDecoder& decoder,
                              uint32_t the_unit,
                              uint32_t output_buffer, uint32_t header,
                              uint32_t pts_out, uint32_t pts_valid_out) {
    if (!guest_range(c, the_unit, 0x38u) ||
        !guest_range(c, output_buffer, kMpaSamplesPerFrame * 4u) ||
        !guest_range(c, header, 4u) ||
        !guest_range(c, pts_out, 4u) ||
        !guest_range(c, pts_valid_out, 4u)) {
        return -1;
    }

    // A subscriber chunk may contain up to four frames. The original
    // decoder thread supplies one decompressed buffer per MPAudioDecode call;
    // the host parser therefore queues all frames but exposes exactly one
    // here before asking the guest callback for more compressed data.
    m2host::MpegAudioFrame ready;
    if (decoder.pop_frame(ready))
        return write_mpa_frame(c, output_buffer, header, pts_out,
                               pts_valid_out, ready);
    if (!decoder.available() && !decoder.open())
        return -1;

    // MPAudioDecode obtains input through the guest callback pair supplied to
    // CreateMPAudioDecoder. Keep requesting/releasing subscriber buffers
    // until one complete output frame is available; a split frame may cross
    // an arbitrary number of compressed messages.
    for (;;) {
        const uint32_t scratch = c->gpr[1] + 0x28u;
        if (scratch < c->gpr[1] ||
            !guest_range(c, scratch, kMpaScratchBytes))
            return -1;
        for (uint32_t off = 0; off < kMpaScratchBytes; off += 4u)
            wr32(c, scratch + off, 0u);

        c->gpr[3] = the_unit;
        c->gpr[4] = scratch;       // const uint8_t **buf
        c->gpr[5] = scratch + 4u;  // int32 *len
        c->gpr[6] = scratch + 8u;  // uint32 *pts
        c->gpr[7] = scratch + 12u; // uint32 *ptsIsValidFlag
        FN_GetCompressedBfr__63be8(c);
        const int32_t get_status = static_cast<int32_t>(c->gpr[3]);
        if (get_status != 0)
            return get_status;

        const uint32_t input = rd32(c, scratch);
        const uint32_t input_len = rd32(c, scratch + 4u);
        const uint32_t pts = rd32(c, scratch + 8u);
        const uint32_t pts_valid = rd32(c, scratch + 12u);
        if (!guest_range(c, input, input_len) || input_len < 4u) {
            release_mpa_input(c, the_unit, input);
            return -1;
        }

        std::vector<int16_t> pcm;
        if (input_len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            release_mpa_input(c, the_unit, input);
            return -1;
        }
        const int decoded_bytes = decoder.decode(
            c->mem + input, static_cast<int>(input_len), pcm, pts, pts_valid);
        const int32_t read_status = release_mpa_input(c, the_unit, input);
        if (read_status != 0)
            return read_status;
        if (decoded_bytes < 0)
            return -1;

        if (decoder.pop_frame(ready))
            return write_mpa_frame(c, output_buffer, header, pts_out,
                                   pts_valid_out, ready);
    }
}

}  // namespace

extern "C" void m2_mpegaudio_decode(const uint8_t* data, int32_t size) {
    std::vector<int16_t> pcm;
    g_mpeg.decode(data, size, pcm);
    // This compatibility entry predates the typed MPAudioDecode boundary and
    // has no output parameters. Do not let its decoded FIFO contaminate the
    // next production dispatch call.
    m2host::MpegAudioFrame discarded;
    while (g_mpeg.pop_frame(discarded)) {}
}

extern "C" int m2_mpegaudio_dispatch(PPCContext* c, uint32_t ordinal,
                                      uint32_t pc) {
    (void)pc;
    if (!c || !c->mem) return 0;

    switch (ordinal) {
    case 1: {  // CreateMPAudioDecoder(MPADecoderContext **, callbacks)
        if (!guest_range(c, c->gpr[3], 4u)) {
            c->gpr[3] = static_cast<uint32_t>(-1);
            return 1;
        }
        const uint32_t token = alloc_mpa_context_token();
        if (token == 0u) {
            c->gpr[3] = static_cast<uint32_t>(-1);
            return 1;
        }
        auto decoder = std::make_unique<m2host::MpegDecoder>();
        if (!decoder->open()) {
            c->gpr[3] = static_cast<uint32_t>(-1);
            return 1;
        }
        try {
            g_mpa_contexts.emplace(token, std::move(decoder));
        } catch (const std::bad_alloc&) {
            c->gpr[3] = static_cast<uint32_t>(-1);
            return 1;
        }
        wr32(c, c->gpr[3], token);
        c->gpr[3] = 0u;
        return 1;
    }
    case 2: {  // DeleteMPAudioDecoder(MPADecoderContext *)
        const uint32_t token = c->gpr[3];
        if (token != 0u) g_mpa_contexts.erase(token);
        c->gpr[3] = 0u;
        return 1;
    }
    case 3: {  // MPAFlush(MPADecoderContext *)
        if (m2host::MpegDecoder* decoder = mpa_context(c->gpr[3]))
            decoder->flush();
        c->gpr[3] = 0u;
        return 1;
    }
    case 0: {  // MPAudioDecode
        m2host::MpegDecoder* decoder = mpa_context(c->gpr[4]);
        const int32_t status =
            decoder ? mpa_decode_one(c, *decoder, c->gpr[3], c->gpr[7],
                                     c->gpr[8], c->gpr[5], c->gpr[6])
                    : -1;
        c->gpr[3] = static_cast<uint32_t>(status);
        return 1;
    }
    default:
        return 0;
    }
}

void mpeg_decoder_register() {
    using namespace m2host;
    if (!g_mpeg.available()) g_mpeg.open();
    folio_register(M2_FOLIO_MPEGAUDIODECODER, 0,
                   reinterpret_cast<void*>(&m2_mpegaudio_decode));
}

extern "C" int m2_decode_mpeg_still(const uint8_t* bitstream, size_t bitstream_size,
                                    uint8_t* dst_buffer, size_t dst_buffer_size,
                                    int width, int height) {
#if FFMPEG_GUARD
    if (!bitstream || bitstream_size == 0 || !dst_buffer ||
        dst_buffer_size < static_cast<size_t>(width * height * 2)) {
        return -1;
    }
    std::vector<uint8_t> prepared = m2host::prepare_mpeg_still_packet(
        bitstream, bitstream_size, width, height);
    if (!prepared.empty()) {
        prepared.resize(bitstream_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        bitstream = prepared.data();
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) return -1;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return -1;
    }

    pkt->data = const_cast<uint8_t*>(bitstream);
    pkt->size = static_cast<int>(bitstream_size);
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret >= 0) {
        ret = avcodec_receive_frame(ctx, frame);
        if (ret < 0) {
            avcodec_send_packet(ctx, nullptr);
            ret = avcodec_receive_frame(ctx, frame);
        }
    }
    if (ret == 0) {
        const int src_w = frame->width;
        const int src_h = frame->height;
        const int out_w = width > 0 ? width : src_w;
        const int out_h = height > 0 ? height : src_h;
        for (int y = 0; y < out_h && y < src_h; ++y) {
            for (int x = 0; x < out_w && x < src_w; ++x) {
                const int Y = frame->data[0][y * frame->linesize[0] + x];
                const int U = frame->data[1][(y / 2) * frame->linesize[1] + (x / 2)] - 128;
                const int V = frame->data[2][(y / 2) * frame->linesize[2] + (x / 2)] - 128;

                const int C = Y - 16;
                int r = (298 * C + 409 * V + 128) >> 8;
                int g = (298 * C - 100 * U - 208 * V + 128) >> 8;
                int b = (298 * C + 516 * U + 128) >> 8;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;

                const uint16_t r5 = static_cast<uint16_t>(r >> 3);
                const uint16_t g5 = static_cast<uint16_t>(g >> 3);
                const uint16_t b5 = static_cast<uint16_t>(b >> 3);
                const uint16_t rgb555 = (r5 << 10) | (g5 << 5) | b5;

                const size_t dst_idx = (static_cast<size_t>(y) * out_w + x) * 2;
                dst_buffer[dst_idx + 0] = static_cast<uint8_t>((rgb555 >> 8) & 0xff);
                dst_buffer[dst_idx + 1] = static_cast<uint8_t>(rgb555 & 0xff);
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return ret == 0 ? 0 : -1;
#else
    (void)bitstream;
    (void)bitstream_size;
    (void)dst_buffer;
    (void)dst_buffer_size;
    (void)width;
    (void)height;
    return -1;
#endif
}

#include <algorithm>

namespace m2host {

#if FFMPEG_GUARD
static void convert_yuv420p_to_rgb555(const AVFrame* frame, uint8_t* dst_buffer, int out_w, int out_h) {
    const int src_w = frame->width;
    const int src_h = frame->height;
    for (int y = 0; y < out_h && y < src_h; ++y) {
        for (int x = 0; x < out_w && x < src_w; ++x) {
            const int Y = frame->data[0][y * frame->linesize[0] + x];
            const int U = frame->data[1][(y / 2) * frame->linesize[1] + (x / 2)] - 128;
            const int V = frame->data[2][(y / 2) * frame->linesize[2] + (x / 2)] - 128;

            const int C = Y - 16;
            int r = (298 * C + 409 * V + 128) >> 8;
            int g = (298 * C - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * C + 516 * U + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            const uint16_t r5 = static_cast<uint16_t>(r >> 3);
            const uint16_t g5 = static_cast<uint16_t>(g >> 3);
            const uint16_t b5 = static_cast<uint16_t>(b >> 3);
            const uint16_t rgb555 = (r5 << 10) | (g5 << 5) | b5;

            const size_t dst_idx = (static_cast<size_t>(y) * out_w + x) * 2;
            dst_buffer[dst_idx + 0] = static_cast<uint8_t>((rgb555 >> 8) & 0xff);
            dst_buffer[dst_idx + 1] = static_cast<uint8_t>(rgb555 & 0xff);
        }
    }
}
#endif

namespace {

constexpr int kMpegVideoNoMem = static_cast<int>(0xd57b9006u);
constexpr int kMpegVideoParamError = static_cast<int>(0xd57b9017u);

uint32_t round_macroblock(uint32_t value) {
    return (value + 15u) & ~15u;
}

// This is the released MPEGVideoBuffers.c::CalcBufferInfo calculation as
// applied by AllocDecodeBuffers to one MPEG sequence header. It is kept next
// to the host decoder because the fixed-buffer decision must happen before
// FFmpeg is allowed to consume a packet.
int fixed_buffer_status(uint32_t width, uint32_t height,
                        uint32_t ref_capacity, uint32_t strip_capacity,
                        bool ref_client_control,
                        bool strip_client_control) {
    const uint32_t rounded_width = round_macroblock(width);
    const uint32_t rounded_height = round_macroblock(height);
    if (rounded_width > 2730u)
        return kMpegVideoParamError;

    const uint64_t yuv_bytes =
        (static_cast<uint64_t>(rounded_width) * rounded_height * 3u) / 2u;
    const uint32_t ref_required = static_cast<uint32_t>(
        ((yuv_bytes + 4095u) / 4096u) * 4096u * 2u);

    uint32_t strip_required = 0u;
    if (rounded_width <= 341u)
        strip_required = 16u * 1024u;
    else if (rounded_width <= 682u)
        strip_required = 32u * 1024u;
    else if (rounded_width <= 1365u)
        strip_required = 64u * 1024u;
    else
        strip_required = 128u * 1024u;

    if (ref_client_control && ref_capacity < ref_required)
        return kMpegVideoNoMem;
    if (strip_client_control && strip_capacity < strip_required)
        return kMpegVideoNoMem;
    return 0;
}

bool is_sequence_header(const std::vector<uint8_t>& bytes, size_t offset,
                        uint32_t* width_out, uint32_t* height_out) {
    if (offset > bytes.size() || bytes.size() - offset < 7u ||
        bytes[offset + 0u] != 0x00u || bytes[offset + 1u] != 0x00u ||
        bytes[offset + 2u] != 0x01u || bytes[offset + 3u] != 0xb3u)
        return false;
    const uint32_t width = (static_cast<uint32_t>(bytes[offset + 4u]) << 4u) |
                           (static_cast<uint32_t>(bytes[offset + 5u]) >> 4u);
    const uint32_t height =
        ((static_cast<uint32_t>(bytes[offset + 5u]) & 0x0fu) << 8u) |
        static_cast<uint32_t>(bytes[offset + 6u]);
    if (width_out) *width_out = width;
    if (height_out) *height_out = height;
    return true;
}

size_t find_sequence_header(const std::vector<uint8_t>& bytes, size_t start) {
    for (size_t i = start; i + 4u <= bytes.size(); ++i) {
        if (bytes[i + 0u] == 0x00u && bytes[i + 1u] == 0x00u &&
            bytes[i + 2u] == 0x01u && bytes[i + 3u] == 0xb3u)
            return i;
    }
    return bytes.size();
}

}  // namespace

MpegVideoDecoder::MpegVideoDecoder() = default;
MpegVideoDecoder::~MpegVideoDecoder() {
    close();
}

// Both open_locked/close_locked assume m_mutex is held. feed() calls
// open_locked() after taking the lock so availability and codec state cannot
// be torn down between the check and the decode call.
bool MpegVideoDecoder::open_locked() {
#if FFMPEG_GUARD
    close_locked();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) {
        std::printf("[mpeg-video] no MPEG-1 video decoder in libavcodec\n");
        return false;
    }
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) return false;
    if (avcodec_open2(m_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&m_ctx);
        return false;
    }
    m_parser = av_parser_init(AV_CODEC_ID_MPEG1VIDEO);
    if (!m_parser) {
        avcodec_free_context(&m_ctx);
        return false;
    }
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_pkt || !m_frame) {
        if (m_frame) av_frame_free(&m_frame);
        if (m_pkt) av_packet_free(&m_pkt);
        if (m_parser) av_parser_close(m_parser);
        if (m_ctx) avcodec_free_context(&m_ctx);
        m_parser = nullptr;
        m_ctx = nullptr;
        return false;
    }
    m_available = true;
    m_first_frame = true;
    m_last_pts = 0;
    return true;
#else
    return false;
#endif
}

bool MpegVideoDecoder::open() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return open_locked();
}

// Assumes the caller holds m_mutex. The public close() wrapper below is the
// only other entry; the destructor runs it only on a clean process exit where
// guest tasks have already been joined.
void MpegVideoDecoder::close_locked() {
#if FFMPEG_GUARD
    if (m_frame) av_frame_free(&m_frame);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_parser) av_parser_close(m_parser);
    if (m_ctx) avcodec_free_context(&m_ctx);
    m_frame = nullptr;
    m_pkt = nullptr;
    m_parser = nullptr;
    m_ctx = nullptr;
#endif
    m_available = false;
    m_first_frame = true;
    m_last_pts = 0;
    m_frames.clear();
}

void MpegVideoDecoder::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    close_locked();
}

void MpegVideoDecoder::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
#if FFMPEG_GUARD
    if (m_ctx) avcodec_flush_buffers(m_ctx);
#endif
    m_frames.clear();
    m_first_frame = true;
    m_last_pts = 0;
}

void MpegVideoDecoder::flush() {
    reset();
}

int MpegVideoDecoder::configure_buffers(uint32_t ref_min_size,
                                         uint32_t strip_min_size,
                                         bool ref_client_control,
                                         bool strip_client_control) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ref_capacity = ref_min_size;
    m_strip_capacity = strip_min_size;
    m_ref_client_control = ref_client_control;
    m_strip_client_control = strip_client_control;
    m_wait_sequence_header = ref_client_control || strip_client_control;
    m_sequence_probe_tail.clear();
#if FFMPEG_GUARD
    if (m_ctx) avcodec_flush_buffers(m_ctx);
    if (m_pkt) av_packet_unref(m_pkt);
    if (m_frame) av_frame_unref(m_frame);
    if (m_parser) reset_parser(m_parser, AV_CODEC_ID_MPEG1VIDEO);
#endif
    m_frames.clear();
    m_first_frame = true;
    m_last_pts = 0;
    return 0;
}

int MpegVideoDecoder::feed(const uint8_t* data, size_t len, uint32_t flags, uint32_t pts, uint32_t user_data) {
#if FFMPEG_GUARD
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_available && !open_locked()) return -1;

    constexpr uint32_t FMV_FLUSH_FLAG_VAL = 0x08000000u;
    constexpr uint32_t FMV_END_OF_STREAM_FLAG_VAL = 0x20000000u;
    constexpr uint32_t FMV_VALID_PTS_VAL = 0x80000000u;

    if (flags & FMV_FLUSH_FLAG_VAL) {
        if (m_ctx) avcodec_flush_buffers(m_ctx);
        m_frames.clear();
        m_first_frame = true;
        m_last_pts = 0;
        m_sequence_probe_tail.clear();
    }

    // ALLOCBUFFERS/SET* leave the released device in a fixed-capacity mode.
    // Until the next sequence header, non-header writes are completed with
    // zero status and no decode. Once a sequence header appears, check the
    // source-derived strip/reference requirements before passing any bytes to
    // the replacement decoder.
    // The probe buffer must outlive the parser feed below: in the
    // wait-for-sequence-header case `data`/`len` are rebased onto `scan`, so
    // it cannot be a block-local that dies before the decode.
    std::vector<uint8_t> sequence_scan;
    if (m_ref_client_control || m_strip_client_control) {
        std::vector<uint8_t>& scan = sequence_scan;
        scan.reserve(m_sequence_probe_tail.size() + len);
        scan.insert(scan.end(), m_sequence_probe_tail.begin(),
                    m_sequence_probe_tail.end());
        if (data && len != 0u)
            scan.insert(scan.end(), data, data + len);

        size_t sequence = find_sequence_header(scan, 0u);
        if (m_wait_sequence_header) {
            if (sequence == scan.size()) {
                const size_t keep = std::min<size_t>(6u, scan.size());
                m_sequence_probe_tail.assign(scan.end() - keep, scan.end());
                return 0;
            }
            if (scan.size() - sequence < 7u) {
                m_sequence_probe_tail.assign(scan.begin() + sequence,
                                             scan.end());
                return 0;
            }

            uint32_t width = 0u;
            uint32_t height = 0u;
            (void)is_sequence_header(scan, sequence, &width, &height);
            const int capacity_status = fixed_buffer_status(
                width, height, m_ref_capacity, m_strip_capacity,
                m_ref_client_control, m_strip_client_control);
            if (capacity_status != 0) {
                if (m_ctx) avcodec_flush_buffers(m_ctx);
                m_frames.clear();
                m_sequence_probe_tail.clear();
                return capacity_status;
            }
            m_wait_sequence_header = false;
            m_sequence_probe_tail.clear();
            data = scan.data() + sequence;
            len = scan.size() - sequence;
        } else {
            for (size_t cursor = sequence; cursor < scan.size();) {
                if (scan.size() - cursor < 7u)
                    break;
                uint32_t width = 0u;
                uint32_t height = 0u;
                if (is_sequence_header(scan, cursor, &width, &height)) {
                    const int capacity_status = fixed_buffer_status(
                        width, height, m_ref_capacity, m_strip_capacity,
                        m_ref_client_control, m_strip_client_control);
                    if (capacity_status != 0) {
                        if (m_ctx) avcodec_flush_buffers(m_ctx);
                        m_frames.clear();
                        m_wait_sequence_header = true;
                        m_sequence_probe_tail.clear();
                        return capacity_status;
                    }
                }
                sequence = find_sequence_header(scan, cursor + 1u);
                if (sequence == scan.size()) break;
                cursor = sequence;
            }
            const size_t keep = std::min<size_t>(6u, scan.size());
            m_sequence_probe_tail.assign(scan.end() - keep, scan.end());
        }
    }

    int frames_decoded = 0;

    auto drain = [&]() {
        while (avcodec_receive_frame(m_ctx, m_frame) == 0) {
            DecodedVideoFrame df;
            df.rgb555.resize(576 * 432 * 2, 0);
            convert_yuv420p_to_rgb555(m_frame, df.rgb555.data(), 576, 432);

            uint32_t frame_pts = 0;
            if (m_frame->pts != AV_NOPTS_VALUE && m_frame->pts >= 0) {
                frame_pts = static_cast<uint32_t>(m_frame->pts);
                m_last_pts = frame_pts;
                m_first_frame = false;
            } else if (m_first_frame) {
                frame_pts = (flags & FMV_VALID_PTS_VAL) ? pts : 0;
                m_last_pts = frame_pts;
                m_first_frame = false;
            } else {
                m_last_pts += 3600;  // 25 fps in 90kHz ticks
                frame_pts = m_last_pts;
            }
            df.pts = frame_pts;
            df.user_data = user_data;
            m_frames.push_back(std::move(df));
            av_frame_unref(m_frame);
            ++frames_decoded;
        }
    };

    if (data && len > 0) {
        const uint8_t* cur_data = data;
        int cur_len = static_cast<int>(len);
        while (cur_len > 0) {
            uint8_t* pout = nullptr;
            int pout_len = 0;
            const int64_t parser_pts = (flags & FMV_VALID_PTS_VAL) ? static_cast<int64_t>(pts) : AV_NOPTS_VALUE;
            int parsed = av_parser_parse2(m_parser, m_ctx, &pout, &pout_len,
                                          cur_data, cur_len,
                                          parser_pts, AV_NOPTS_VALUE, 0);
            if (parsed < 0) break;
            cur_data += parsed;
            cur_len -= parsed;
            if (pout_len > 0) {
                m_pkt->data = pout;
                m_pkt->size = pout_len;
                m_pkt->pts = m_parser->pts;
                m_pkt->dts = m_parser->dts;
                if (avcodec_send_packet(m_ctx, m_pkt) == 0) {
                    drain();
                }
                av_packet_unref(m_pkt);
            }
        }
    }

    if (flags & FMV_END_OF_STREAM_FLAG_VAL) {
        if (avcodec_send_packet(m_ctx, nullptr) == 0) {
            drain();
        }
    }

    return frames_decoded;
#else
    (void)data; (void)len; (void)flags; (void)pts; (void)user_data;
    return -1;
#endif
}

bool MpegVideoDecoder::has_frame() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_frames.empty();
}

int MpegVideoDecoder::pop_frame(uint8_t* dst_buf, size_t dst_len, uint32_t* pts_out, uint32_t* user_data_out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_frames.empty()) return 0;
    const DecodedVideoFrame& f = m_frames.front();
    // The guest FMV contract is a complete 576x432 RGB555 frame. Do not
    // consume a decoded frame when the destination is absent or short: a
    // partial copy followed by successful IO completion loses the only
    // source frame and leaves the guest with uninitialized image data.
    if (!dst_buf || dst_len < f.rgb555.size()) return 0;
    std::memcpy(dst_buf, f.rgb555.data(), f.rgb555.size());
    if (pts_out) *pts_out = f.pts;
    if (user_data_out) *user_data_out = f.user_data;
    m_frames.pop_front();
    return 1;
}

}  // namespace m2host

static m2host::MpegVideoDecoder g_mpeg_video;

extern "C" int m2_mpeg_video_feed(const uint8_t* data, size_t len, uint32_t flags,
                                  uint32_t pts, uint32_t user_data) {
    return g_mpeg_video.feed(data, len, flags, pts, user_data);
}

extern "C" int m2_mpeg_video_configure_buffers(
    uint32_t ref_min_size, uint32_t strip_min_size,
    uint32_t ref_client_control, uint32_t strip_client_control) {
    return g_mpeg_video.configure_buffers(
        ref_min_size, strip_min_size,
        ref_client_control != 0u, strip_client_control != 0u);
}

extern "C" int m2_mpeg_video_has_frame(void) {
    return g_mpeg_video.has_frame() ? 1 : 0;
}

extern "C" int m2_mpeg_video_pop_frame(uint8_t* dst_buf, size_t dst_len,
                                       uint32_t* pts_out, uint32_t* user_data_out) {
    return g_mpeg_video.pop_frame(dst_buf, dst_len, pts_out, user_data_out);
}

extern "C" void m2_mpeg_video_flush(void) {
    g_mpeg_video.flush();
}

extern "C" void m2_mpeg_video_reset(void) {
    g_mpeg_video.reset();
}
