// Focused MPAudioDecoder boundary test.
//
// The input is read from original MPAU/1FRM chunks in Intro.mov. This
// exercises only the identified subscriber/decoder contract; it does not
// drive a game route or search for additional content.

#include "mpeg_decoder.h"

#include "game_recomp.h"
#include "guest_kernel.h"
#include "recomp_helpers.h"
#include "ppc_ctx.h"
#include "recomp_host.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kSkip = 3;

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool has_id(const uint8_t* p, const char* id) {
    return std::memcmp(p, id, 4) == 0;
}

bool read_intro_audio_frames(std::vector<uint8_t>& first,
                             std::vector<uint8_t>& second) {
#ifndef IMSA_TEST_MOVIE_PATH
#error "IMSA_TEST_MOVIE_PATH must name the user's Intro.mov (set by tests/CMakeLists.txt)"
#endif
    std::ifstream input(IMSA_TEST_MOVIE_PATH, std::ios::binary);
    if (!input) return false;
    std::vector<uint8_t> movie((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    std::vector<std::vector<uint8_t>*> outputs{&first, &second};
    size_t found = 0;
    for (size_t off = 0; off + 20u <= movie.size() && found < outputs.size();
         ++off) {
        if (!has_id(movie.data() + off, "MPAU")) continue;
        const uint32_t chunk_size = read_be32(movie.data() + off + 4u);
        if (chunk_size < 20u ||
            chunk_size > movie.size() - off ||
            !has_id(movie.data() + off + 16u, "1FRM"))
            continue;
        const size_t payload_size = static_cast<size_t>(chunk_size) - 20u;
        outputs[found]->assign(movie.begin() + off + 20u,
                               movie.begin() + off + 20u + payload_size);
        ++found;
        off += static_cast<size_t>(chunk_size) - 1u;
    }
    return found == outputs.size();
}

bool check(bool condition, const char* expression) {
    if (condition) return true;
    std::cerr << "FAIL: " << expression << '\n';
    return false;
}

bool check_frame(const m2host::MpegAudioFrame& frame, uint32_t pts,
                 uint32_t pts_valid) {
    bool ok = true;
    ok &= check(frame.header == 0xfffda20cu, "decoded header");
    ok &= check(frame.pts == pts, "decoded pts");
    ok &= check(frame.pts_valid == pts_valid, "decoded pts-valid flag");
    ok &= check(frame.pcm.size() == 1152u * 2u, "decoded sample count");
    return ok;
}

std::vector<uint8_t> sequence_header(uint32_t width, uint32_t height) {
    // MPEG-1 sequence_header() stores the 12-bit dimensions as width[11:0]
    // followed by height[11:0]. The decoder contract only needs this prefix;
    // the following bytes are intentionally not treated as a frame fixture.
    return {
        0x00u, 0x00u, 0x01u, 0xb3u,
        static_cast<uint8_t>((width >> 4u) & 0xffu),
        static_cast<uint8_t>(((width & 0x0fu) << 4u) |
                             ((height >> 8u) & 0x0fu)),
        static_cast<uint8_t>(height & 0xffu),
        0x00u, 0x00u,
    };
}

bool check_video_fixed_buffer_contract() {
    m2host::MpegVideoDecoder decoder;
    if (!decoder.open())
        return check(false, "MPEG-1 video decoder is available");

    constexpr int kNoMem = static_cast<int32_t>(0xd57b9006u);
    constexpr int kParamError = static_cast<int32_t>(0xd57b9017u);
    bool ok = true;
    ok &= check(decoder.configure_buffers(237568u, 16384u, true, true) == 0,
                "video fixed capacities configure");

    const std::vector<uint8_t> non_header = {
        0x00u, 0x00u, 0x01u, 0xb8u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    ok &= check(decoder.feed(non_header.data(), non_header.size(), 0u, 0u,
                             0u) == 0,
                "non-header write is flushed while awaiting sequence header");

    const std::vector<uint8_t> good = sequence_header(320u, 240u);
    ok &= check(decoder.feed(good.data(), 5u, 0u, 0u, 0u) == 0,
                "sequence header prefix is retained");
    ok &= check(decoder.feed(good.data() + 5u, good.size() - 5u, 0u, 0u,
                             0u) == 0,
                "fitting split sequence header is accepted");

    const std::vector<uint8_t> oversized = sequence_header(640u, 480u);
    ok &= check(decoder.feed(oversized.data(), oversized.size(), 0u, 0u, 0u) ==
                    kNoMem,
                "oversized sequence header returns MPEG NOMEM");
    ok &= check(decoder.feed(non_header.data(), non_header.size(), 0u, 0u,
                             0u) == 0,
                "non-header write is flushed after capacity failure");

    // A zero-dimension ALLOCBUFFERS form clears client control and restores
    // the automatic decoder path; the synthetic prefix must not inherit the
    // previous fixed-capacity refusal.
    ok &= check(decoder.configure_buffers(0u, 0u, false, false) == 0,
                "video automatic allocation configure");
    const int automatic_status = decoder.feed(oversized.data(),
                                              oversized.size(), 0u, 0u, 0u);
    ok &= check(automatic_status != kNoMem && automatic_status != kParamError,
                "automatic allocation does not publish fixed-buffer errors");

    ok &= check(decoder.configure_buffers(237568u, 16384u, true, true) == 0,
                "video fixed capacities reconfigure");
    const std::vector<uint8_t> invalid_width = sequence_header(3000u, 240u);
    ok &= check(decoder.feed(invalid_width.data(), invalid_width.size(), 0u,
                             0u, 0u) == kParamError,
                "sequence header over strip ladder returns PARAMERROR");
    decoder.close();
    return ok;
}

bool check_lifted_callback_dispatch(const std::vector<uint8_t>& first,
                                    const std::vector<uint8_t>& second) {
    // These are the guest fields consumed by the lifted MPEG subscriber
    // callback pair.  The generated offsets are the ABI under test:
    // DecoderThreadCtx::msgPort@+0x0c, CompressedBfrQueue@+0x14,
    // curCompressedBufPtr@+0x34, and DecoderMsg::buffer/size/msgItem/
    // presentationTime/timeIsValidFlag at +0x1c/+0x20/+0x18/+0x24/+0x28.
    constexpr uint32_t kDecoderCtx = 0x1000u;
    constexpr uint32_t kNode = 0x2000u;
    constexpr uint32_t kNode2 = 0x2400u;
    constexpr uint32_t kInput = 0x4000u;
    constexpr uint32_t kInput2 = 0x5000u;
    constexpr uint32_t kTokenOut = 0x8000u;
    constexpr uint32_t kPtsOut = 0x8004u;
    constexpr uint32_t kValidOut = 0x8008u;
    constexpr uint32_t kHeaderOut = 0x800cu;
    constexpr uint32_t kPcmOut = 0x9000u;
    constexpr uint32_t kStack = 0x200000u;
    constexpr uint32_t kPts = 0x3456u;
    constexpr uint32_t kPts2 = 0x4567u;
    constexpr uint32_t kListOffset = 0x14u;

    bool ok = true;
    PPCContext& context = *m2_recomp_context();
    context.gpr[1] = kStack;
    std::memcpy(context.mem + kInput, first.data(), first.size());
    std::memcpy(context.mem + kInput2, second.data(), second.size());

    m2k_context kernel{};
    m2k_init(&kernel);
    ok &= check(m2_guest_kernel_bind(&kernel, &context) == 0,
                "bind callback guest kernel");
    m2_kernel_dispatch_set_global(m2_guest_kernel_handlers());

    const m2k_item_t stream_port = m2k_create_msgport(&kernel);
    const m2k_item_t transport_port = m2k_create_msgport(&kernel);
    const m2k_item_t reply_port = m2k_create_msgport(&kernel);
    const m2k_item_t message = m2k_create_msg(&kernel, reply_port);
    m2k_item_t dispatched = M2K_ITEM_NONE;
    uint32_t decoder_token = 0;
    bool decoder_created = false;

    ok &= check(stream_port != M2K_ITEM_NONE &&
                    transport_port != M2K_ITEM_NONE &&
                    reply_port != M2K_ITEM_NONE && message != M2K_ITEM_NONE,
                "callback message ports and message created");
    if (ok) {
        // Keep the context's input port empty.  GetCompressedBfr first runs
        // ProcessNewMsgs, which must observe the empty port and then dequeue
        // the already-queued compressed node without blocking.
        wr32(&context, kDecoderCtx + 0x0cu, stream_port);
        wr32(&context, kDecoderCtx + 0x10u, 0u);
        wr32(&context, kDecoderCtx + 0x34u, 0u);
        const uint32_t compressed_list = kDecoderCtx + kListOffset;
        const uint32_t decompressed_list = kDecoderCtx + 0x24u;
        m2_guest_kernel_handlers()->PrepList(compressed_list);
        m2_guest_kernel_handlers()->PrepList(decompressed_list);

        // ReplyMsg only accepts a dispatched message.  Move the retained
        // subscriber message through a separate transport port first, just
        // as the real subscriber-to-decoder handoff does.
        ok &= check(m2k_send_msg_item(&kernel, transport_port, message, kNode,
                                      0x34u) == M2K_OK,
                    "queue compressed subscriber message");
        ok &= check(m2k_get_msg(&kernel, transport_port, &dispatched, NULL,
                                NULL) == M2K_OK && dispatched == message,
                    "dispatch compressed subscriber message");

        wr32(&context, kNode + 0u, 0u);
        wr32(&context, kNode + 4u, 0u);
        wr32(&context, kNode + 0x18u, message);
        wr32(&context, kNode + 0x1cu, kInput);
        wr32(&context, kNode + 0x20u, static_cast<uint32_t>(first.size()));
        wr32(&context, kNode + 0x24u, kPts);
        wr32(&context, kNode + 0x28u, 1u);
        m2_guest_kernel_handlers()->AddTail(compressed_list, kNode);
        ok &= check(rd32(&context, compressed_list + 4u) == kNode &&
                        rd32(&context, compressed_list + 8u) == 0u &&
                        rd32(&context, compressed_list + 12u) == kNode,
                    "callback compressed list contains queued node");

        wr32(&context, kTokenOut, 0u);
        context.gpr[3] = kTokenOut;
        ok &= check(m2_mpegaudio_dispatch(&context, 1u, 0u) == 1,
                    "typed MPEG decoder create for callback dispatch");
        decoder_token = rd32(&context, kTokenOut);
        decoder_created = decoder_token != 0u;
        ok &= check(decoder_created, "callback decoder token published");

        if (decoder_created) {
            context.gpr[1] = kStack;
            context.gpr[3] = kDecoderCtx;
            context.gpr[4] = decoder_token;
            context.gpr[5] = kPtsOut;
            context.gpr[6] = kValidOut;
            context.gpr[7] = kPcmOut;
            context.gpr[8] = kHeaderOut;
            ok &= check(m2_mpegaudio_dispatch(&context, 0u, 0u) == 1,
                        "typed MPEG callback/output dispatch handled");
            ok &= check(static_cast<int32_t>(context.gpr[3]) == 0,
                        "callback/output dispatch returned success");
            ok &= check(rd32(&context, kHeaderOut) == 0xfffda20cu,
                        "callback/output dispatch header");
            ok &= check(rd32(&context, kPtsOut) == kPts,
                        "callback/output dispatch PTS");
            ok &= check(rd32(&context, kValidOut) == 1u,
                        "callback/output dispatch PTS validity");
            ok &= check(rd32(&context, kDecoderCtx + 0x34u) == 0u,
                        "callback releases current compressed buffer");

            m2k_item_t reply = M2K_ITEM_NONE;
            uint32_t reply_data = 0;
            uint32_t reply_size = 0;
            int32_t reply_code = -1;
            ok &= check(m2k_get_msg(&kernel, reply_port, &reply, NULL, NULL) ==
                            M2K_OK && reply == message,
                        "callback replies compressed subscriber message");
            ok &= check(m2k_message_info(&kernel, reply, NULL, &reply_data,
                                         &reply_size, &reply_code) == M2K_OK &&
                            reply_data == kNode && reply_size == 0x34u &&
                            reply_code == 0,
                        "callback reply preserves message payload");

            // Re-arm the same guest decoder context with a second authentic
            // subscriber message. The first ReplyMsg has returned its
            // message to the reply port; use a fresh message and node while
            // retaining the same compressed list and decoder token.
            const m2k_item_t message2 = m2k_create_msg(&kernel, reply_port);
            ok &= check(message2 != M2K_ITEM_NONE,
                        "second callback message created");
            if (message2 != M2K_ITEM_NONE) {
                ok &= check(m2k_send_msg_item(&kernel, transport_port,
                                              message2, kNode2, 0x34u) == M2K_OK,
                            "queue second compressed subscriber message");
                ok &= check(m2k_get_msg(&kernel, transport_port, &dispatched,
                                        NULL, NULL) == M2K_OK &&
                                dispatched == message2,
                            "dispatch second compressed subscriber message");

                wr32(&context, kNode2 + 0u, 0u);
                wr32(&context, kNode2 + 4u, 0u);
                wr32(&context, kNode2 + 0x18u, message2);
                wr32(&context, kNode2 + 0x1cu, kInput2);
                wr32(&context, kNode2 + 0x20u,
                     static_cast<uint32_t>(second.size()));
                wr32(&context, kNode2 + 0x24u, kPts2);
                wr32(&context, kNode2 + 0x28u, 1u);
                m2_guest_kernel_handlers()->AddTail(compressed_list, kNode2);

                context.gpr[1] = kStack;
                context.gpr[3] = kDecoderCtx;
                context.gpr[4] = decoder_token;
                context.gpr[5] = kPtsOut;
                context.gpr[6] = kValidOut;
                context.gpr[7] = kPcmOut;
                context.gpr[8] = kHeaderOut;
                ok &= check(m2_mpegaudio_dispatch(&context, 0u, 0u) == 1,
                            "second typed MPEG callback/output dispatch handled");
                ok &= check(static_cast<int32_t>(context.gpr[3]) == 0,
                            "second callback/output dispatch returned success");
                ok &= check(rd32(&context, kHeaderOut) == 0xfffda20cu,
                            "second callback/output dispatch header");
                ok &= check(rd32(&context, kPtsOut) == kPts2,
                            "second callback/output dispatch PTS");
                ok &= check(rd32(&context, kValidOut) == 1u,
                            "second callback/output dispatch PTS validity");
                ok &= check(rd32(&context, kDecoderCtx + 0x34u) == 0u,
                            "second callback releases current compressed buffer");

                reply = M2K_ITEM_NONE;
                reply_data = 0;
                reply_size = 0;
                reply_code = -1;
                ok &= check(m2k_get_msg(&kernel, reply_port, &reply, NULL,
                                        NULL) == M2K_OK && reply == message2,
                            "second callback replies compressed subscriber message");
                ok &= check(m2k_message_info(&kernel, reply, NULL, &reply_data,
                                             &reply_size, &reply_code) == M2K_OK &&
                                reply_data == kNode2 && reply_size == 0x34u &&
                                reply_code == 0,
                            "second callback reply preserves message payload");
            }
        }
    }

    if (decoder_created) {
        context.gpr[3] = decoder_token;
        ok &= check(m2_mpegaudio_dispatch(&context, 2u, 0u) == 1,
                    "typed MPEG callback decoder delete");
    }
    m2_kernel_dispatch_set_global(NULL);
    m2_guest_kernel_unbind();
    m2k_destroy(&kernel);
    return ok;
}

}  // namespace

int main() {
    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    if (!read_intro_audio_frames(first, second)) {
        std::cout << "SKIP: original Intro.mov MPAU/1FRM evidence is absent\n";
        return kSkip;
    }

    m2host::MpegDecoder decoder;
    if (!decoder.open()) {
        std::cout << "SKIP: FFmpeg MPEG-1 Layer II decoder is unavailable\n";
        return kSkip;
    }

    bool ok = true;
    ok &= check_video_fixed_buffer_contract();
    std::vector<int16_t> pcm;
    m2host::MpegAudioFrame frame;

    // One authentic frame in one subscriber buffer produces exactly one
    // fixed-size output frame and retains its PTS.
    int decoded = decoder.decode(first.data(), static_cast<int>(first.size()),
                                 pcm, 0x1234u, 1u);
    ok &= check(decoded == 1152 * 4, "one frame PCM byte count");
    ok &= check(pcm.size() == 1152u * 2u, "one frame PCM sample count");
    ok &= check(decoder.queued_frames() == 1u, "one queued frame");
    ok &= check(decoder.pop_frame(frame), "pop one frame");
    ok &= check_frame(frame, 0x1234u, 1u);
    ok &= check(!decoder.pop_frame(frame), "one frame queue drained");

    // A frame split at an arbitrary byte boundary must be retained by the
    // parser. Its second buffer's timestamp is returned but is not marked as
    // valid, matching BufferInfo.wholeFrame in the released decoder.
    decoder.flush();
    pcm.clear();
    constexpr size_t split = 7u;
    decoded = decoder.decode(first.data(), static_cast<int>(split), pcm,
                             0x2000u, 1u);
    ok &= check(decoded == 0 && decoder.queued_frames() == 0u,
                "split prefix produces no frame");
    decoded = decoder.decode(first.data() + split,
                             static_cast<int>(first.size() - split), pcm,
                             0x3000u, 1u);
    ok &= check(decoded == 1152 * 4, "split frame PCM byte count");
    ok &= check(decoder.pop_frame(frame), "pop split frame");
    ok &= check_frame(frame, 0x3000u, 0u);

    // A subscriber chunk can contain several complete frames. The parser
    // queues both, while only the first carries the chunk's PTS validity.
    decoder.flush();
    pcm.clear();
    std::vector<uint8_t> two_frames = first;
    two_frames.insert(two_frames.end(), second.begin(), second.end());
    decoded = decoder.decode(two_frames.data(),
                             static_cast<int>(two_frames.size()), pcm,
                             0x4000u, 1u);
    ok &= check(decoded == 2 * 1152 * 4, "multi-frame PCM byte count");
    ok &= check(decoder.queued_frames() == 2u, "multi-frame queue count");
    ok &= check(decoder.pop_frame(frame), "pop first multi-frame output");
    ok &= check_frame(frame, 0x4000u, 1u);
    ok &= check(decoder.pop_frame(frame), "pop second multi-frame output");
    ok &= check_frame(frame, 0x4000u, 0u);
    ok &= check(!decoder.pop_frame(frame), "multi-frame queue drained");

    // Negative control: a non-MPEG buffer cannot produce output or leave a
    // stale decoded frame available to the next MPAudioDecode call.
    decoder.flush();
    const uint8_t invalid[32] = {};
    pcm.clear();
    decoded = decoder.decode(invalid, sizeof(invalid), pcm, 0x5000u, 1u);
    ok &= check(decoded <= 0 && decoder.queued_frames() == 0u,
                "invalid MPEG input rejected");

    decoder.close();

    // Lifecycle control through the typed production entry: Create publishes
    // a guest-visible decoder token, Flush is reusable, and Delete releases
    // the host parser. The lifted callback/output handoff is exercised
    // separately below with the full guest subscriber fixture.
    std::vector<uint8_t> guest(0x2000u, 0u);
    PPCContext context{};
    context.mem = guest.data();
    context.gpr[3] = 0x100u;
    ok &= check(m2_mpegaudio_dispatch(&context, 1u, 0u) == 1,
                "typed MPEG create handled");
    ok &= check(read_be32(guest.data() + 0x100u) == 0x003f2000u,
                "typed MPEG decoder token");
    const uint32_t first_token = read_be32(guest.data() + 0x100u);
    context.gpr[3] = 0x104u;
    ok &= check(m2_mpegaudio_dispatch(&context, 1u, 0u) == 1,
                "second typed MPEG create handled");
    const uint32_t second_token = read_be32(guest.data() + 0x104u);
    ok &= check(second_token != first_token, "typed MPEG contexts are distinct");
    context.gpr[3] = first_token;
    ok &= check(m2_mpegaudio_dispatch(&context, 3u, 0u) == 1,
                "typed MPEG flush handled");
    context.gpr[3] = first_token;
    ok &= check(m2_mpegaudio_dispatch(&context, 2u, 0u) == 1,
                "typed MPEG delete handled");
    context.gpr[3] = second_token;
    ok &= check(m2_mpegaudio_dispatch(&context, 2u, 0u) == 1,
                "second typed MPEG delete handled");

    ok &= check_lifted_callback_dispatch(first, second);

    if (!ok) return 1;
    std::cout << "PASS: MPEG audio chunking, callback handoff, frame FIFO, PTS, and negative control\n";
    return 0;
}
