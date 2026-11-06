// audio_shim.cpp — audio folio shim over an SDL3 device stream.
//
// Model: the game's instrument/attachment graph collapses onto kMaxVoices
// software voices mixed into one S16-stereo output stream. The currently
// registered entry points are the repinned on-disc ordinals; DSP resource
// execution and playback effects remain a separate recovery step.
#include "audio_shim.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ogg_stream.h"
#include "ppc_ctx.h"
#include "selector_observed.h"
#include "audio_dispatch.h"
#include "shim_abi.h"

extern "C" {
#include "audio_folio.h"
}
#include "folio_loader.h"

using namespace m2host;

static AudioShim* g_audio = nullptr;

// The guest call sites establish status contracts for 19/24/28/38/41/44/45/46/50/51/52/54/56/57/58/59/60/67/68/69/70/71,
// a Cue signal-mask query for 23, frame-count value contracts for 35/37, and
// the M2 source establishes Item ownership for Load/UnloadInstrument. The latter
// uses the shared fixed-width guest Item pool; it does not reinterpret guest
// EAs as host pointers or claim that DSP resources/playback are active.
// Serializes every typed audio selector body against the per-frame audio
// pump and against other guest task threads that call the same selectors
// (soundtrack thread vs state/main thread). The runtime owns the recursive
// lock; see g_audio_graph_lock in recomp_runtime.c.
extern "C" void m2_guest_audio_graph_lock(void);
extern "C" void m2_guest_audio_graph_unlock(void);
// Setter-priority handshake with the asynchronous DSPP worker is declared in
// shims/audio_dispatch.h (enter on block, leave once the lock is owned).
// Names the audio selector ordinal for the audio-lock attribution collector.
extern "C" void m2_audio_diag_set_current_ordinal(uint32_t ordinal);

namespace {
struct AudioGraphGuard {
    // The lock wrapper announces a blocking acquisition for the asynchronous
    // DSPP worker's sample-frame yield; the guard only scopes the hold.
    AudioGraphGuard() { m2_guest_audio_graph_lock(); }
    ~AudioGraphGuard() { m2_guest_audio_graph_unlock(); }
};

inline int16_t clamp16(int32_t x) {
    return static_cast<int16_t>(std::clamp<int32_t>(x, -32768, 32767));
}
}  // namespace

static int m2_audio_dispatch_c_impl(PPCContext* c, uint32_t ordinal,
                                    uint32_t pc) {
    (void)pc;
    if (!c || !m2_is_audio_typed_selector(ordinal)) return 0;
    // Ordinal 24 SleepUntilAudioTime blocks the calling task on its Cue signal
    // until the frame/DSPP timer pump raises it, and the pump itself takes the
    // audio graph lock. Running this arm before AudioGraphGuard keeps the wait
    // outside that lock; every other arm stays serialized by the guard.
    if (ordinal == 24) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_sleep_until_audio_time(
            c, c->gpr[3], c->gpr[4], c->gpr[5]));
        return 1;
    }
    // Read-only selectors: GetAudioTime (35) and GetAudioFrameCount (37)
    // read atomic frame counters and require no mutation of the audio graph.
    // Serving them outside AudioGraphGuard eliminates thousands of lock acquisitions
    // and contention against the asynchronous DSPP worker.
    if (ordinal == 35) {
        c->gpr[3] = m2_guest_audio_get_audio_time(c);
        return 1;
    }
    if (ordinal == 37) {
        c->gpr[3] = m2_guest_audio_get_audio_frame_count(c);
        return 1;
    }
    AudioGraphGuard guard;
    if (ordinal == 8) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_create_mixer_template(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 27) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_load_instrument(
            c, c->gpr[3], c->gpr[4], c->gpr[5]));
        return 1;
    }
    if (ordinal == 17) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_unload_instrument(
            c->gpr[3]));
        return 1;
    }
    if (ordinal == 32) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_load_ins_template(
            c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 34) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_arm_trigger_named(
                c, c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6]));
        return 1;
    }
    if (ordinal == 23) {
        c->gpr[3] = m2_guest_audio_get_cue_signal(c, c->gpr[3]);
        return 1;
    }
    if (ordinal == 19) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_get_resource_info(
            c, c->gpr[3], c->gpr[4], c->gpr[5]));
        return 1;
    }
    if (ordinal == 28) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_get_item_info(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == M2_AUDIO_SET_ITEM_INFO_ORDINAL) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_set_item_info(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 58) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_signal_at_audio_time(
                c, c->gpr[3], c->gpr[4], c->gpr[5]));
        return 1;
    }
    if (ordinal == 52) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_stop_attachment(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 56) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_set_audio_clock_rate(
            c, c->gpr[3], c->fpr[1]));
        return 1;
    }
    if (ordinal == 71) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_read_audio_clock(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 38) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_read_probe_part(
            c, c->gpr[3], static_cast<int32_t>(c->gpr[4]), c->gpr[5]));
        return 1;
    }
    if (ordinal == 41) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_abort_timer_cue(c, c->gpr[3]));
        return 1;
    }
    if (ordinal == 44) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_resume_instrument(c, c->gpr[3]));
        return 1;
    }
    if (ordinal == 45) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_pause_instrument(c, c->gpr[3]));
        return 1;
    }
    if (ordinal == 50) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_monitor_attachment(
                c, c->gpr[3], c->gpr[4], static_cast<int32_t>(c->gpr[5])));
        return 1;
    }
    if (ordinal == 51) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_link_attachments(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 54) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_start_attachment(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 67) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_stop_instrument(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 68) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_release_instrument(c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 57) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_connect_instrument_parts(
                c, c->gpr[3], c->gpr[4], static_cast<int32_t>(c->gpr[5]),
                c->gpr[6], c->gpr[7], static_cast<int32_t>(c->gpr[8])));
        return 1;
    }
    if (ordinal == 59) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_disconnect_instrument_parts(
                c, c->gpr[3], c->gpr[4], static_cast<int32_t>(c->gpr[5])));
        return 1;
    }
    if (ordinal == 60) {
        c->gpr[3] = static_cast<uint32_t>(
            m2_guest_audio_read_knob_part(
                c, c->gpr[3], static_cast<int32_t>(c->gpr[4]), c->gpr[5]));
        return 1;
    }
    if (ordinal == 69) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_start_instrument(
            c, c->gpr[3], c->gpr[4]));
        return 1;
    }
    if (ordinal == 70) {
        c->gpr[3] = static_cast<uint32_t>(m2_guest_audio_set_knob_part(
            c, c->gpr[3], static_cast<int32_t>(c->gpr[4]), c->fpr[1]));
        return 1;
    }
    // Keep the typed-selector predicate and the implementation list in
    // lockstep. A newly classified ordinal must not silently become an
    // unconditional-success host stub.
    return 0;
}

// Public audio boundary entry point. Notes the (folio 6, ordinal) dispatch in
// the observed record (selector_observed.h) whether the call arrives through
// recomp_call_selector or directly from a focused test.
// Exact guest selector call site of the most recent audio folio dispatch.
// Selector entries that take no PPCContext (UnloadInstrument) still need the
// site for the bounded [audio-unload] diagnostic; observation only.
extern "C" uint32_t m2_audio_selector_pc = 0;

extern "C" int m2_audio_dispatch_c(PPCContext* c, uint32_t ordinal,
                                   uint32_t pc) {
    m2_audio_selector_pc = pc;
    // Name the ordinal for the audio-graph-lock attribution collector
    // (M2_AUDIO_LOCK_DIAG, default-off; two thread-local stores per dispatch).
    m2_audio_diag_set_current_ordinal(ordinal);
    const int handled = m2_audio_dispatch_c_impl(c, ordinal, pc);
    m2_audio_diag_set_current_ordinal(0xFFFFFFFFu);
    m2_selector_note(M2_FOLIO_AUDIO, ordinal, handled);
    return handled;
}

AudioShim::AudioShim() = default;
AudioShim::~AudioShim() = default;

bool AudioShim::music_open(const std::filesystem::path& ogg)
{
    m_music = std::make_unique<OggStream>();
    if (!m_music->open(ogg)) {
        m_music.reset();
        return false;
    }
    return true;
}

void AudioShim::music_play(bool loop)
{
    if (!m_music) return;
    m_music_on = true;
    m_music_loop = loop;
}

void AudioShim::music_stop()
{
    m_music_on = false;
}

namespace {

constexpr uint32_t kMaxCbd2BufferBytes = 16u << 20;

static bool cbd2_diag_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_AUDIO_CBD2_DIAG");
        return value && *value && *value != '0';
    }();
    return enabled;
}

// DSPP mixer drain report shares the audio-start diag gate so live synth
// voice production shows up in the same runs as the start census.
static bool dspp_diag_enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("M2_AUDIO_START_DIAG");
        return value && *value && *value != '0';
    }();
    return enabled;
}

// Event cap: the default 128 is enough for a short run but a long movie
// can spend the whole budget on segment completions before the music/sfx
// streams start. M2_AUDIO_CBD2_DIAG_N raises it (ceiling 4096) for
// long listening sessions.
static uint32_t cbd2_diag_cap()
{
    static const uint32_t cap = [] {
        const char* value = std::getenv("M2_AUDIO_CBD2_DIAG_N");
        if (!value || !*value) return 128u;
        long v = std::strtol(value, nullptr, 0);
        if (v < 0) v = 0;
        if (v > 4096) v = 4096;
        return static_cast<uint32_t>(v);
    }();
    return cap;
}

static void cbd2_diag(const char* event, uint32_t stream_key,
                      uint32_t attachment_id, uint32_t related_id,
                      uint32_t byte_count, size_t frame_count)
{
    static std::atomic<uint32_t> count{0};
    if (!cbd2_diag_enabled()) return;
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
    if (n >= cbd2_diag_cap()) return;
    std::fprintf(stderr,
                 "[audio-cbd2] n=%u event=%s stream=%#x att=%#x "
                 "related=%#x bytes=%u frames=%zu\n",
                 n + 1u, event, stream_key, attachment_id, related_id,
                 byte_count, frame_count);
}

static int32_t cbd2_cdiv(int32_t value, int32_t divisor)
{
    const int32_t quotient = std::abs(value) / std::abs(divisor);
    return ((value < 0) != (divisor < 0)) ? -quotient : quotient;
}

static std::vector<int16_t> decode_cbd2_segment(const uint8_t* bytes,
                                                uint32_t byte_count,
                                                int32_t predictor[2])
{
    if (!bytes || byte_count < 2u || (byte_count & 1u) != 0u ||
        byte_count > kMaxCbd2BufferBytes)
        return {};

    const size_t frames = byte_count / 2u;
    std::vector<int16_t> pcm(frames * 2u);
    for (size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < 2; ++channel) {
            const uint8_t raw = bytes[frame * 2u + channel];
            const int32_t sample = raw >= 128u
                                       ? static_cast<int32_t>(raw) - 256
                                       : static_cast<int32_t>(raw);
            // sampler_cbd2_f2 resets the per-channel predictor for an even
            // compressed byte, then adds the signed cube divided by 64. The
            // DSP saturates that sum and stores it back, so the predictor
            // itself never leaves the 16-bit range.
            if ((raw & 1u) == 0u) predictor[channel] = 0;
            predictor[channel] = std::clamp(
                predictor[channel] + cbd2_cdiv(sample * sample * sample, 64),
                -32768, 32767);
            pcm[frame * 2u + channel] = static_cast<int16_t>(predictor[channel]);
        }
    }
    return pcm;
}

static std::vector<int16_t> decode_pcm16be_segment(const uint8_t* bytes,
                                                   uint32_t byte_count)
{
    if (!bytes || byte_count < 4u || (byte_count & 3u) != 0u ||
        byte_count > kMaxCbd2BufferBytes)
        return {};

    const size_t frames = byte_count / 4u;
    std::vector<int16_t> pcm(frames * 2u);
    for (size_t i = 0; i < frames * 2u; ++i) {
        const size_t off = i * 2u;
        pcm[i] = static_cast<int16_t>(
            (static_cast<uint16_t>(bytes[off]) << 8) | bytes[off + 1u]);
    }
    return pcm;
}

}  // namespace

bool AudioShim::start_cbd2_stream(uint32_t stream_key, uint32_t attachment_id,
                                  const uint8_t* bytes, uint32_t byte_count,
                                  float gain)
{
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lock(m_cbd2_mutex);
        if (!bytes || byte_count < 2u || (byte_count & 1u) != 0u ||
            byte_count > kMaxCbd2BufferBytes)
            return false;

        Cbd2Stream* stream = nullptr;
        for (auto& candidate : m_cbd2_streams) {
            if (candidate.key == stream_key) {
                stream = &candidate;
                break;
            }
        }
        if (!stream) {
            m_cbd2_streams.push_back(Cbd2Stream{});
            stream = &m_cbd2_streams.back();
            stream->key = stream_key;
        }
        stream->gain = std::isfinite(gain) ? gain : 1.0f;
        for (const auto& segment : stream->segments) {
            if (segment.attachment_id == attachment_id) return true;
        }
        pcm = decode_cbd2_segment(bytes, byte_count, stream->predictor);
        if (pcm.empty()) return false;
        const size_t frame_count = pcm.size() / 2u;
        stream->completed_attachments.erase(attachment_id);
        stream->segments.push_back(
            Cbd2Segment{attachment_id, std::move(pcm), 0});
        cbd2_diag("start", stream_key, attachment_id, 0u, byte_count,
                  frame_count);
    }
    return true;
}

int AudioShim::link_cbd2_stream(uint32_t stream_key,
                                 uint32_t first_attachment,
                                 uint32_t next_attachment,
                                 const uint8_t* bytes, uint32_t byte_count)
{
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    if (!bytes || byte_count < 2u || (byte_count & 1u) != 0u ||
        byte_count > kMaxCbd2BufferBytes)
        return false;

    Cbd2Stream* stream = nullptr;
    for (auto& candidate : m_cbd2_streams) {
        if (candidate.key == stream_key) {
            stream = &candidate;
            break;
        }
    }
    if (!stream) return false;

    size_t first_index = stream->segments.size();
    for (size_t i = 0; i < stream->segments.size(); ++i) {
        if (stream->segments[i].attachment_id == first_attachment) {
            first_index = i;
            break;
        }
    }
    if (first_index == stream->segments.size())
        return stream->completed_attachments.count(first_attachment) ? 2 : 0;
    for (const auto& segment : stream->segments) {
        if (segment.attachment_id == next_attachment) return true;
    }
    // SoundSpooler links the new node to the tail of the active queue. If a
    // caller presents an older node, the successor is already represented by
    // the following segment and there is nothing to append here.
    if (first_index + 1u != stream->segments.size()) return true;

    std::vector<int16_t> pcm =
        decode_cbd2_segment(bytes, byte_count, stream->predictor);
    if (pcm.empty()) return false;
    const size_t frame_count = pcm.size() / 2u;
    stream->completed_attachments.erase(next_attachment);
    stream->segments.push_back(
        Cbd2Segment{next_attachment, std::move(pcm), 0});
    cbd2_diag("link", stream_key, next_attachment, first_attachment,
              byte_count, frame_count);
    return true;
}

bool AudioShim::start_pcm16be_stream(uint32_t stream_key,
                                     uint32_t attachment_id,
                                     const uint8_t* bytes,
                                     uint32_t byte_count, float gain)
{
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    std::vector<int16_t> pcm = decode_pcm16be_segment(bytes, byte_count);
    if (pcm.empty()) return false;
    Cbd2Stream* stream = nullptr;
    for (auto& candidate : m_cbd2_streams) {
        if (candidate.key == stream_key) {
            stream = &candidate;
            break;
        }
    }
    if (!stream) {
        m_cbd2_streams.push_back(Cbd2Stream{});
        stream = &m_cbd2_streams.back();
        stream->key = stream_key;
    }
    stream->gain = std::isfinite(gain) ? gain : 1.0f;
    for (const auto& segment : stream->segments)
        if (segment.attachment_id == attachment_id) return true;
    stream->completed_attachments.erase(attachment_id);
    stream->segments.push_back(Cbd2Segment{attachment_id, std::move(pcm), 0});
    return true;
}

int AudioShim::link_pcm16be_stream(uint32_t stream_key,
                                    uint32_t first_attachment,
                                    uint32_t next_attachment,
                                    const uint8_t* bytes,
                                    uint32_t byte_count)
{
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    std::vector<int16_t> pcm = decode_pcm16be_segment(bytes, byte_count);
    if (pcm.empty()) return false;
    Cbd2Stream* stream = nullptr;
    for (auto& candidate : m_cbd2_streams) {
        if (candidate.key == stream_key) {
            stream = &candidate;
            break;
        }
    }
    if (!stream) return false;
    size_t first_index = stream->segments.size();
    for (size_t i = 0; i < stream->segments.size(); ++i) {
        if (stream->segments[i].attachment_id == first_attachment) {
            first_index = i;
            break;
        }
    }
    if (first_index == stream->segments.size())
        return stream->completed_attachments.count(first_attachment) ? 2 : 0;
    for (const auto& segment : stream->segments)
        if (segment.attachment_id == next_attachment) return true;
    if (first_index + 1u != stream->segments.size()) return true;
    stream->completed_attachments.erase(next_attachment);
    stream->segments.push_back(
        Cbd2Segment{next_attachment, std::move(pcm), 0});
    return true;
}

void AudioShim::unlink_cbd2_stream(uint32_t stream_key,
                                   uint32_t first_attachment)
{
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    for (auto& stream : m_cbd2_streams) {
        if (stream.key != stream_key) continue;
        size_t first_index = stream.segments.size();
        for (size_t i = 0; i < stream.segments.size(); ++i) {
            if (stream.segments[i].attachment_id == first_attachment) {
                first_index = i;
                break;
            }
        }
        if (first_index != stream.segments.size() &&
            first_index + 1u < stream.segments.size()) {
            stream.segments.erase(stream.segments.begin() +
                                      static_cast<ptrdiff_t>(first_index + 1u),
                                  stream.segments.end());
        }
        return;
    }
}

void AudioShim::reset_cbd2_stream(uint32_t stream_key)
{
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    m_cbd2_streams.erase(
        std::remove_if(m_cbd2_streams.begin(), m_cbd2_streams.end(),
                       [stream_key](const Cbd2Stream& stream) {
                           return stream.key == stream_key;
                       }),
        m_cbd2_streams.end());
}

bool AudioShim::pop_attachment_completion(uint32_t* attachment_id)
{
    if (!attachment_id) return false;
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    if (m_attachment_completions.empty()) return false;
    *attachment_id = m_attachment_completions.front();
    m_attachment_completions.pop_front();
    return true;
}

// Music mixes under the voices at output rate 44100. All OGG
// exports are 44.1 kHz so the common path is a straight interleaved add;
// other rates take the linear-resample fallback (good enough for menu loops,
// and every shipped track encodes at 44.1 kHz anyway).
static void mix_music(m2host::OggStream& music, double* cursor,
                      int16_t* out, int frames, float volume)
{
    const int src_rate = music.rate() > 0 ? music.rate() : 44100;
    const int ch = music.channels() > 0 ? music.channels() : 2;

    if (src_rate == 44100) {
        // Exact path: all shipped exports are 44.1 kHz.
        std::vector<int16_t> pcm(static_cast<size_t>(frames + 2) * ch);
        const int have = music.read_s16(pcm.data(), frames, true);
        const int n = std::min(have, frames);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                const int sc = c < ch ? c : ch - 1;
                float mixed = out[i * 2 + c] +
                              pcm[static_cast<size_t>(i) * ch + sc] * volume;
                long v = lrintf(mixed);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                out[i * 2 + c] = static_cast<int16_t>(v);
            }
        return;
    }

    std::vector<int16_t> pcm(static_cast<size_t>(frames + 4) * ch);
    const int have =
        music.read_s16(pcm.data(), frames + 2, true);  // loop at EOF
    if (have <= 0) return;

    const double step = static_cast<double>(src_rate) / 44100.0;
    int written = 0;
    for (int i = 0; i < frames; ++i) {
        const double pos = *cursor + step * i;
        const int i0 = static_cast<int>(pos);
        if (i0 >= have) break;
        const int i1 = (i0 + 1 < have) ? i0 + 1 : i0;
        const float t = static_cast<float>(pos - i0);
        for (int c = 0; c < 2; ++c) {
            const int sc = c < ch ? c : ch - 1;
            const float a = pcm[static_cast<size_t>(i0) * ch + sc];
            const float b = pcm[static_cast<size_t>(i1) * ch + sc];
            const float smp = a + (b - a) * t;
            const float mixed = out[i * 2 + c] + smp * volume;
            long v = lrintf(mixed);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[i * 2 + c] = static_cast<int16_t>(v);
        }
        written = i + 1;
    }
    // Advance/normalize the fractional cursor by what was actually played.
    *cursor += step * written;
    if (*cursor >= 1.0) {
        const double whole = std::floor(*cursor);
        *cursor -= whole;
        (void)whole;
    }
}

void AudioShim::mix_cb(void* user, SDL_AudioStream* stream, int additional_amount,
                       int total_amount) {
    auto* self = static_cast<AudioShim*>(user);
    (void)total_amount;
    if (additional_amount <= 0) return;
    const int frames = additional_amount /
                       static_cast<int>(2 * sizeof(int16_t));
    if (self->m_sample_clock_epoch_ns.load(std::memory_order_relaxed) == 0u)
        self->m_sample_clock_epoch_ns.store(SDL_GetTicksNS(),
                                           std::memory_order_release);
    if (static_cast<size_t>(frames * 2) > self->m_mix_scratch.size()) {
        self->m_mix_scratch.resize(static_cast<size_t>(frames * 2), 0);
    } else {
        std::fill_n(self->m_mix_scratch.data(), static_cast<size_t>(frames * 2), int16_t{0});
    }
    self->mix_into(self->m_mix_scratch.data(), frames);
    SDL_PutAudioStreamData(stream, self->m_mix_scratch.data(),
                           static_cast<int>(frames * 2 * sizeof(int16_t)));
}

// M2 DSPPReleaseSampleAttachment only rewrites the next DMA block: the
// currently playing DMA block (the sustain-loop pass) always runs to its end
// before the queued silence, release tail, or release loop starts. A released
// voice therefore keeps its cursor and applies the transition at the loop
// boundary. Defined with the release entry points below; forward-declared for
// the mixer and tap cursors that reach the boundary first.
static void voice_apply_release_transition(Voice& v, uint32_t voice_index,
                                        double over, bool tap = false);

// sampler_sqs2_v1 includes sampler_16_v1.j / sub_sampler_16_v1.ins: its
// fractional Phase blends FIFO_CURRENT and FIFO_NEXT before Amplitude.
// The next frame follows the queued DMA segment, including loop wrap or a
// pending release; it must not sample beyond a partial sustain loop.
static int32_t voice_interpolated_sample(const Voice& v,
                                         const std::vector<int16_t>& sample,
                                         bool tap, size_t channel) {
    const double pos = tap ? v.tap_pos : v.pos;
    const size_t frame = static_cast<size_t>(pos);
    const size_t total = sample.size() / 2u;
    size_t next = frame + 1u;
    const bool looping = tap ? v.tap_looping : v.looping;
    const bool pending = tap ? v.tap_release_pending : v.release_pending;
    const int32_t begin = tap ? v.tap_loop_begin : v.loop_begin;
    const int32_t end = tap ? v.tap_loop_end : v.loop_end;
    if (looping && end > begin && next >= static_cast<size_t>(end)) {
        if (!pending) {
            next = static_cast<size_t>(begin);
        } else {
            const bool gap = v.release_end != v.sustain_end &&
                             v.release_begin != v.sustain_end &&
                             v.sustain_begin != v.release_end &&
                             v.sustain_begin != v.release_begin;
            next = static_cast<size_t>(v.release_loop && !gap
                                          ? v.release_begin : v.sustain_end);
        }
    } else if (looping && end <= begin && next >= total) {
        next = 0;  // Whole-payload loop without explicit frame bounds.
    }
    const double current = sample[frame * 2u + channel];
    // A finite attachment queues silence after its payload.
    const double following = next < total ? sample[next * 2u + channel] : 0.0;
    return static_cast<int32_t>(
        (current + (following - current) * (pos - frame)) * v.gain);
}

void AudioShim::mix_into(int16_t* out, int frames) {
    std::lock_guard<std::mutex> cbd2_lock(m_cbd2_mutex);
    auto finish_voice = [this](Voice& voice) {
        const uint32_t attachment = voice.completion_attachment;
        voice.active = false;
        voice.completion_attachment = 0;
        if (attachment != 0u)
            m_attachment_completions.push_back(attachment);
    };
    // Per-source output census (diagnostic only, same gate as [audio-start]).
    // The final mix is additive here, so each source's contribution is
    // separable; "some audio plays" must not be confused with "every expected
    // source plays". One line per 1 s window, bounded to 512 windows.
    const bool mix_census_on = dspp_diag_enabled();
    static uint64_t mix_frames;
    static uint64_t mix_nz[5];
    static int32_t mix_peak[5];
    static uint32_t mix_windows;
    auto census = [](int src, int32_t l, int32_t r) {
        int32_t a = l < 0 ? -l : l;
        const int32_t b = r < 0 ? -r : r;
        if (b > a) a = b;
        if (a != 0) ++mix_nz[src];
        if (a > mix_peak[src]) mix_peak[src] = a;
    };
    auto census_flush = [&]() {
        if (mix_windows < 512u) {
            uint32_t active = 0;
            for (int i = 0; i < kMaxVoices; ++i) {
                std::unique_lock<std::mutex> voice_lock(m_voices[i].mutex);
                if (m_voices[i].active && m_voices[i].sample_id) ++active;
            }
            std::printf("[audio-mix] w=%u frames=%llu music(nz=%llu pk=%d) "
                        "voices(nz=%llu pk=%d) cbd2(nz=%llu pk=%d) "
                        "dspp(nz=%llu pk=%d) final(nz=%llu pk=%d) "
                        "voices_active=%u direct_at_end=",
                        mix_windows, (unsigned long long)mix_frames,
                        (unsigned long long)mix_nz[0], mix_peak[0],
                        (unsigned long long)mix_nz[1], mix_peak[1],
                        (unsigned long long)mix_nz[2], mix_peak[2],
                        (unsigned long long)mix_nz[3], mix_peak[3],
                        (unsigned long long)mix_nz[4], mix_peak[4], active);
            bool any_direct = false;
            for (uint32_t i = 0; i < (uint32_t)kMaxVoices; ++i) {
                // Read under the slot lock: the guest audio surface can stop
                // or retarget any of these fields between the reads.
                std::unique_lock<std::mutex> voice_lock(m_voices[i].mutex);
                const Voice& v = m_voices[i];
                if (!v.active || !v.sample_id || !v.direct || v.paused)
                    continue;
                std::printf("%s%u:sid%u:att%#x:rate%.4g:gain%.3g:loop%d",
                            any_direct ? "," : "", i, v.sample_id,
                            v.completion_attachment, v.rate, v.gain,
                            v.looping ? 1 : 0);
                any_direct = true;
            }
            if (!any_direct) std::printf("none");
            std::printf("\n");
        }
        ++mix_windows;
        mix_frames = 0;
        for (int i = 0; i < 5; ++i) { mix_nz[i] = 0; mix_peak[i] = 0; }
    };
    if (m_music_on && m_music) {
        mix_music(*m_music, &m_music_pos, out, frames, m_music_volume);
    }
    uint64_t dspp_drained = 0;
    std::vector<int32_t> voice_mix_heap;
    int32_t voice_mix_stack[2048 * 2];
    int32_t* voice_mix = voice_mix_stack;
    if (frames > 2048) {
        voice_mix_heap.assign(static_cast<size_t>(frames) * 2u, 0);
        voice_mix = voice_mix_heap.data();
    } else {
        std::memset(voice_mix_stack, 0, static_cast<size_t>(frames) * 2u * sizeof(int32_t));
    }

    for (int vi = 0; vi < kMaxVoices; ++vi) {
        // The audio thread and the guest audio surface share these
        // fields. The slot lock is held once per voice for the whole block,
        // so a control operation never interleaves with this block's cursor advance.
        std::unique_lock<std::mutex> voice_lock(m_voices[vi].mutex);
        Voice& v = m_voices[vi];
        if (!v.active || !v.sample_id || v.paused) continue;
        // A graph-routed one-shot is consumed by tap_voice_frame, not by
        // this muted shadow cursor. Retire it here so attachment events
        // remain ordered through the audio thread's completion queue.
        if (!v.direct && !v.tap_looping &&
            v.tap_finished.load(std::memory_order_acquire)) {
            finish_voice(v);
            continue;
        }
        const std::vector<int16_t>* s = sample(v.sample_id);
        const int32_t total = s ? static_cast<int32_t>(s->size() / 2u) : 0;
        if (!s || total <= 0) {
            finish_voice(v);
            continue;
        }
        for (int i = 0; i < frames; ++i) {
            if (v.looping && v.loop_end > v.loop_begin &&
                v.pos >= v.loop_end) {
                if (v.release_pending)
                    voice_apply_release_transition(v, (uint32_t)vi,
                                                v.pos - v.loop_end);
                else
                    v.pos = static_cast<double>(v.loop_begin);
            }
            if (v.pos < 0 || v.pos >= total) {
                if (v.looping && total > 0) {
                    v.pos = static_cast<double>(
                        v.loop_begin >= 0 && v.loop_begin < total
                            ? v.loop_begin
                            : 0);
                }
                else {
                    if (v.direct ||
                        v.tap_finished.load(std::memory_order_acquire))
                        finish_voice(v);
                    break;
                }
            }
            // R2-full partition: a suppressed voice advances this muted
            // shadow cursor for phase alignment, but the graph tap controls
            // one-shot completion. Only the SDL accumulation skips it.
            if (v.direct) {
                voice_mix[i * 2] += voice_interpolated_sample(v, *s, false, 0);
                voice_mix[i * 2 + 1] += voice_interpolated_sample(v, *s, false, 1);
            }
            v.pos += v.rate;
            if (v.looping && v.loop_end > v.loop_begin &&
                v.pos >= v.loop_end) {
                if (v.release_pending) {
                    voice_apply_release_transition(v, (uint32_t)vi,
                                                v.pos - v.loop_end);
                } else {
                    const double span =
                        static_cast<double>(v.loop_end - v.loop_begin);
                    while (v.pos >= v.loop_end) v.pos -= span;
                }
            }
            if (v.direct ? (!v.looping && v.pos >= total)
                         : (!v.tap_looping &&
                            v.tap_finished.load(std::memory_order_acquire))) {
                finish_voice(v);
                break;
            }
        }
    }

    // Fade the DSPP layer out over the last frames of a queue that this block
    // will empty. Pushes wait on m_cbd2_mutex, so it cannot refill first.
    const uint32_t dspp_start_queued = dspp_queued_frames();
    const uint32_t dspp_fade_out_frames =
        !m_controlled && dspp_start_queued <= static_cast<uint32_t>(frames)
            ? std::min(kDsppFadeFrames, dspp_start_queued)
            : 0u;
    uint32_t dspp_left = dspp_start_queued;

    for (int i = 0; i < frames; ++i) {
        const int32_t music_l = out[i * 2];
        const int32_t music_r = out[i * 2 + 1];
        const int32_t voice_l = voice_mix[i * 2];
        const int32_t voice_r = voice_mix[i * 2 + 1];
        int32_t l = music_l + voice_l;
        int32_t r = music_r + voice_r;
        for (auto& stream : m_cbd2_streams) {
            while (!stream.segments.empty() &&
                   stream.segments.front().pos >=
                       stream.segments.front().pcm.size() / 2u) {
                const Cbd2Segment& completed = stream.segments.front();
                cbd2_diag("complete", stream.key, completed.attachment_id,
                          0u, static_cast<uint32_t>(completed.pcm.size()),
                          completed.pcm.size() / 2u);
                m_attachment_completions.push_back(
                    completed.attachment_id);
                stream.completed_attachments.insert(stream.segments.front().attachment_id);
                stream.segments.pop_front();
            }
            if (stream.segments.empty()) continue;

            Cbd2Segment& segment = stream.segments.front();
            const size_t frame = segment.pos++;
            l += static_cast<int32_t>(segment.pcm[frame * 2u] *
                                      stream.gain);
            r += static_cast<int32_t>(segment.pcm[frame * 2u + 1u] *
                                      stream.gain);
            if (segment.pos >= segment.pcm.size() / 2u) {
                cbd2_diag("complete", stream.key, segment.attachment_id, 0u,
                          static_cast<uint32_t>(segment.pcm.size()),
                          segment.pcm.size() / 2u);
                m_attachment_completions.push_back(segment.attachment_id);
                stream.completed_attachments.insert(stream.segments.front().attachment_id);
                stream.segments.pop_front();
            }
        }
        const int32_t cbd2_l = l - music_l - voice_l;
        const int32_t cbd2_r = r - music_r - voice_r;
        int16_t dspp_sample_l = 0;
        int16_t dspp_sample_r = 0;
        const bool dspp_available = dspp_pop_pair(dspp_sample_l, dspp_sample_r);
        int32_t dspp_raw_l = static_cast<int32_t>(dspp_sample_l);
        int32_t dspp_raw_r = static_cast<int32_t>(dspp_sample_r);
        if (dspp_available) {
            ++dspp_drained;
            if (dspp_left <= dspp_fade_out_frames) {
                const int32_t divisor =
                    static_cast<int32_t>(dspp_fade_out_frames) + 1;
                dspp_raw_l = dspp_raw_l * static_cast<int32_t>(dspp_left) / divisor;
                dspp_raw_r = dspp_raw_r * static_cast<int32_t>(dspp_left) / divisor;
            }
            if (m_dspp_fade_in_pos < kDsppFadeFrames) {
                ++m_dspp_fade_in_pos;
                const int32_t gain = static_cast<int32_t>(m_dspp_fade_in_pos);
                const int32_t divisor = static_cast<int32_t>(kDsppFadeFrames) + 1;
                dspp_raw_l = dspp_raw_l * gain / divisor;
                dspp_raw_r = dspp_raw_r * gain / divisor;
            }
            // Whatever follows a completed fade-out starts from silence, even
            // if the worker refills the queue before the next block.
            if (dspp_left == 1u && dspp_fade_out_frames != 0u)
                m_dspp_fade_in_pos = 0u;
            --dspp_left;
        } else if (!m_controlled) {
            m_dspp_fade_in_pos = 0u;
        }
        const int32_t dspp_l = dspp_raw_l;
        const int32_t dspp_r = dspp_raw_r;

        // Additive master bus: music + voices + CBD2 streams + DSPP graph
        // output, saturated once at the platform boundary. The original
        // folio's final transfer is additive (nanokernel.ins gMixer
        // accumulation); no per-sample gain switching is applied here,
        // because a gain that toggles per sample is itself a click source.
        const int32_t master_l = music_l + voice_l + cbd2_l + dspp_raw_l;
        const int32_t master_r = music_r + voice_r + cbd2_r + dspp_raw_r;
        out[i * 2] = clamp16(master_l);
        out[i * 2 + 1] = clamp16(master_r);
        if (!m_mix_capture.empty()) {
            const size_t frame = m_mix_capture_frames++ % (m_mix_capture.size() / 5u);
            int16_t* capture = m_mix_capture.data() + frame * 5u;
            capture[0] = out[i * 2];
            capture[1] = out[i * 2 + 1];
            capture[2] = clamp16(dspp_l);
            capture[3] = clamp16(dspp_r);
            capture[4] = dspp_available ? 0 : 1;
        }
        if (mix_census_on) {
            census(0, music_l, music_r);
            census(1, voice_l, voice_r);
            census(2, cbd2_l, cbd2_r);
            census(3, dspp_l, dspp_r);
            census(4, out[i * 2], out[i * 2 + 1]);
            if (++mix_frames >= 44100u) census_flush();
        }
    }
    if (dspp_drained > 0) {
        m_dspp_drained.fetch_add(dspp_drained, std::memory_order_relaxed);
        const uint64_t drained = m_dspp_drained.load(std::memory_order_relaxed);
        if (drained >= m_dspp_report_at.load(std::memory_order_relaxed)) {
            m_dspp_report_at.fetch_add(441000, std::memory_order_relaxed);
            if (dspp_diag_enabled()) {
                std::printf("[audio-dspp] mixer drained=%llu pushed=%llu "
                            "dropped=%llu nonzero_frames=%llu fifo=%u\n",
                            (unsigned long long)drained,
                            (unsigned long long)m_dspp_pushed.load(),
                            (unsigned long long)m_dspp_dropped.load(),
                            (unsigned long long)(m_dspp_nonzero.load() / 2u),
                            dspp_queued_frames());
            }
        }
    }

    // Quiet queue-health observation, independent of the verbose DSP trace.
    // Empty frames before the first push are startup, not playback underruns.
    static const bool buffer_diag = std::getenv("M2_AUDIO_BUFFER_DIAG") != nullptr;
    if (buffer_diag && m_dspp_pushed.load(std::memory_order_relaxed) != 0) {
        m_dspp_empty_frames += static_cast<uint64_t>(frames) - dspp_drained;
        m_dspp_observed_frames += static_cast<uint64_t>(frames);
        if (m_dspp_observed_frames >= m_dspp_buffer_report_at) {
            std::fprintf(stderr, "[audio-buffer] rendered=%llu empty=%llu fifo=%u pushed=%llu drained=%llu callback=%d\n",
                         (unsigned long long)m_dspp_observed_frames,
                         (unsigned long long)m_dspp_empty_frames,
                         dspp_queued_frames(),
                         (unsigned long long)m_dspp_pushed.load(),
                         (unsigned long long)m_dspp_drained.load(), frames);
            m_dspp_buffer_report_at += 44100u;
        }
    }
    m_rendered_frames.fetch_add(static_cast<uint32_t>(frames),
                                std::memory_order_relaxed);
    m_last_mix_frames.store(static_cast<uint32_t>(frames), std::memory_order_relaxed);

}

void AudioShim::set_output_muted(bool muted)
{
    m_output_muted = muted;
    if (m_stream) SDL_SetAudioStreamGain(m_stream, muted ? 0.0f : 1.0f);
}

bool AudioShim::init(bool headless, bool controlled) {
    m_controlled = controlled;
    m_rendered_frames.store(0, std::memory_order_relaxed);
    m_sample_clock_epoch_ns.store(0, std::memory_order_relaxed);
    m_last_mix_frames.store(0, std::memory_order_relaxed);
    m_dspp_primed = false;
    m_mix_scratch.assign(4096 * 2, 0);
    if (const char* capture = std::getenv("M2_AUDIO_CAPTURE"); capture && *capture) {
        m_mix_capture_path = capture;
        m_mix_capture.resize(44100u * 30u * 5u);
        m_mix_capture_frames = 0;
    }
    if (controlled) {
        // Use the same production mixer and completion queues, with the
        // diagnostic controller supplying sample time. This sink is silent.
        g_audio = this;
        std::printf("[audio] diagnostic clock: explicit S16 stereo / 44100 Hz service\n");
        return true;
    }
    if (headless) {
        std::printf("[audio] headless: mixer disabled\n");
        return true;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 44100;
    m_stream = m_force_no_device
                   ? nullptr
                   : SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                               &spec, mix_cb, this);
    if (!m_stream) {
        SDL_Log("[audio] no playback device (%s); running silent", SDL_GetError());
        // A missing playback device is a platform condition, not a guest
        // audio error. Bind the shim anyway, exactly as the controlled and
        // headless sinks above do, so the guest's DSPP graph keeps running and
        // its own decisions are preserved. Leaving `g_audio` null here made
        // every `m2_host_audio_*` entry point fail: the guest's
        // StartInstrument took the `result < 0` arm and returned
        // M2_AUDIO_ERR_NO_MEMORY to an attachment that was never out of
        // voices, so a host with no sound card reported a guest audio failure
        // it did not have. Nothing is pushed anywhere (`m_stream` is null, so
        // there is no callback to consume it); `mix_into` is device-independent
        // and the guest-owned tap/R2 mix and completion queues still run.
        g_audio = this;
        return true;  // non-fatal
    }
    SDL_ResumeAudioStreamDevice(m_stream);
    if (m_output_muted) SDL_SetAudioStreamGain(m_stream, 0.0f);
    g_audio = this;
    std::printf("[audio] mixer open: S16 stereo @ %d Hz\n", spec.freq);
    return true;
}

void AudioShim::diagnostic_tick() {
    if (!m_controlled) return;
    int16_t samples[735 * 2] = {0};
    mix_into(samples, 735);
}

void AudioShim::set_stream_gain(uint32_t stream_key, float gain) {
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    for (auto& stream : m_cbd2_streams)
        if (stream.key == stream_key && std::isfinite(gain)) stream.gain = gain;
}

uint32_t AudioShim::dspp_target_frames() const {
    // Before the first callback use a 1024-frame startup fallback; thereafter
    // use the input block observed at this device.
    const uint32_t observed = m_last_mix_frames.load(std::memory_order_relaxed);
    return std::min(observed ? observed : 1024u, kDsppFifoCapFrames / 3u) * 3u;
}

uint32_t AudioShim::dspp_fill_request() {
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    const uint32_t target = dspp_target_frames();
    const uint32_t queued = dspp_queued_frames();
    return queued < target ? target - queued : 0u;
}

uint32_t AudioShim::prime_dspp_output() {
    if (!m_stream || m_controlled) return 0u;
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    if (m_dspp_primed) return 0u;
    m_dspp_primed = true;
    const uint32_t reserve = dspp_target_frames();
    const uint32_t queued = static_cast<uint32_t>(dspp_queued_frames());
    const uint32_t padding = queued < reserve ? reserve - queued : 0u;
    dspp_prepend_silence(padding);
    m_dspp_pushed.fetch_add(padding, std::memory_order_relaxed);
    return padding;
}

void AudioShim::shutdown() {
    if (m_stream) SDL_DestroyAudioStream(m_stream);
    m_stream = nullptr;
    if (!m_mix_capture_path.empty() && !m_mix_capture.empty()) {
        if (std::FILE* capture = std::fopen(m_mix_capture_path.string().c_str(), "wb")) {
            const size_t capacity = m_mix_capture.size() / 5u;
            const size_t frames = std::min<uint64_t>(m_mix_capture_frames, capacity);
            const size_t first = m_mix_capture_frames > capacity
                                     ? m_mix_capture_frames % capacity : 0u;
            const size_t head = std::min(frames, capacity - first);
            std::fwrite(m_mix_capture.data() + first * 5u, sizeof(int16_t), head * 5u, capture);
            std::fwrite(m_mix_capture.data(), sizeof(int16_t), (frames - head) * 5u, capture);
            std::fclose(capture);
            std::fprintf(stderr, "[audio-capture] frames=%zu first=%llu channels=5 path=%s\n",
                         frames, (unsigned long long)(m_mix_capture_frames - frames),
                         m_mix_capture_path.string().c_str());
        }
        m_mix_capture_path.clear();
        m_mix_capture.clear();
    }
    // Only release the process-wide binding if this instance still owns it.
    // Unconditionally clearing it let a second shim (the focused no-device
    // gate constructs one) tear down the first shim's live binding, after
    // which every `m2_host_audio_*` call failed against a destroyed object.
    if (g_audio == this) g_audio = nullptr;
}

uint32_t AudioShim::add_sample(std::vector<int16_t> pcm) {
    // Guest uploads race the audio thread's pool reads; the deque keeps
    // existing element references valid, so only the container needs guarding.
    std::lock_guard<std::mutex> lock(m_pool_mutex);
    // Music republishes a sample on every note. Pool buffers are immutable
    // snapshots, so identical PCM can share storage without changing any
    // guest Sample/Attachment identity or live voice data. Compare a content
    // hash first; a full buffer comparison only runs on a hash match.
    uint64_t hash = 1469598103934665603ull;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    for (size_t i = 0, n = pcm.size() * sizeof(int16_t); i < n; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    for (size_t i = 0; i < m_pool.size(); ++i)
        if (m_pool_hash[i] == hash && m_pool[i] == pcm)
            return static_cast<uint32_t>(i + 1u);
    m_pool.push_back(std::move(pcm));
    m_pool_hash.push_back(hash);
    return static_cast<uint32_t>(m_pool.size());
}

void AudioShim::push_dspp_frames(const int16_t* pcm, uint32_t frames) {
    if (!pcm || frames == 0) return;
    std::lock_guard<std::mutex> lock(m_cbd2_mutex);
    // Lazy env-gated capture of the pushed DSPP contribution (guest thread
    // only): raw S16LE stereo for offline content analysis.
    if (!m_dspp_cap) {
        const char* path = std::getenv("M2_DSPP_CAPTURE");
        if (path && *path) {
            m_dspp_cap = std::fopen(path, "wb");
            m_dspp_cap_left = 44100u * 10u;
        } else {
            // Negative cache: never check the environment again.
            m_dspp_cap_left = 0;
        }
        if (!m_dspp_cap) m_dspp_cap_left = 0;
    }
    const uint32_t room = dspp_room_frames();
    const uint32_t keep = frames <= room ? frames : room;
    for (uint32_t i = 0; i < keep * 2u; ++i) {
        if (pcm[i] != 0) {
            // Counted per sample here; reported as frames below via /2.
            m_dspp_nonzero.fetch_add(1, std::memory_order_relaxed);
        }
        dspp_push_sample(pcm[i]);
    }
    m_dspp_pushed.fetch_add(keep, std::memory_order_relaxed);
    if (keep < frames)
        m_dspp_dropped.fetch_add(frames - keep, std::memory_order_relaxed);
    if (m_dspp_cap && m_dspp_cap_left > 0) {
        const uint32_t dump = keep <= m_dspp_cap_left ? keep : (uint32_t)m_dspp_cap_left;
        std::fwrite(pcm, sizeof(int16_t), dump * 2u, m_dspp_cap);
        m_dspp_cap_left -= dump;
        if (m_dspp_cap_left == 0) {
            std::fclose(m_dspp_cap);
            m_dspp_cap = nullptr;
        }
    }
}

extern "C" void m2_host_audio_push_dspp_frames(const int16_t* pcm,
                                              uint32_t frames) {
    if (g_audio) g_audio->push_dspp_frames(pcm, frames);
}

extern "C" void m2_host_audio_set_stream_gain(uint32_t stream_key, float gain) {
    if (g_audio) g_audio->set_stream_gain(stream_key, gain);
}

extern "C" uint32_t m2_host_audio_dspp_fill_request(void) {
    return g_audio ? g_audio->dspp_fill_request() : 0u;
}

extern "C" uint32_t m2_host_audio_frame_count(void) {
    return g_audio ? g_audio->rendered_frames() : 0u;
}

uint32_t AudioShim::clock_frames() const {
    const uint64_t epoch = m_sample_clock_epoch_ns.load(std::memory_order_acquire);
    // Controlled and device-free runs retain their explicit sample clock.
    if (epoch == 0u) return rendered_frames();
    const uint64_t elapsed = SDL_GetTicksNS() - epoch;
    return static_cast<uint32_t>((elapsed / 1000000000u) * 44100u +
                                (elapsed % 1000000000u) * 44100u / 1000000000u);
}

extern "C" uint32_t m2_host_audio_clock_frame_count(void) {
    return g_audio ? g_audio->clock_frames() : 0u;
}

extern "C" int32_t m2_host_audio_start_cbd2_stream(
    uint32_t stream_key, uint32_t attachment_id, const uint8_t* bytes,
    uint32_t byte_count, float gain) {
    return g_audio &&
                   g_audio->start_cbd2_stream(stream_key, attachment_id,
                                              bytes, byte_count, gain)
               ? 1
               : 0;
}

extern "C" int32_t m2_host_audio_link_cbd2_stream(
    uint32_t stream_key, uint32_t first_attachment, uint32_t next_attachment,
    const uint8_t* bytes, uint32_t byte_count) {
    return g_audio ?
                   g_audio->link_cbd2_stream(stream_key, first_attachment,
                                             next_attachment, bytes,
                                             byte_count)
               : 0;
}

extern "C" int32_t m2_host_audio_start_pcm16be_stream(
    uint32_t stream_key, uint32_t attachment_id, const uint8_t* bytes,
    uint32_t byte_count, float gain) {
    return g_audio &&
                   g_audio->start_pcm16be_stream(stream_key, attachment_id,
                                                 bytes, byte_count, gain)
               ? 1
               : 0;
}

extern "C" int32_t m2_host_audio_link_pcm16be_stream(
    uint32_t stream_key, uint32_t first_attachment, uint32_t next_attachment,
    const uint8_t* bytes, uint32_t byte_count) {
    return g_audio ?
                   g_audio->link_pcm16be_stream(stream_key, first_attachment,
                                                next_attachment, bytes,
                                                byte_count)
               : 0;
}

extern "C" void m2_host_audio_unlink_cbd2_stream(uint32_t stream_key,
                                                   uint32_t first_attachment) {
    if (g_audio) g_audio->unlink_cbd2_stream(stream_key, first_attachment);
}

extern "C" void m2_host_audio_reset_cbd2_stream(uint32_t stream_key) {
    if (g_audio) g_audio->reset_cbd2_stream(stream_key);
}

extern "C" int32_t m2_host_audio_pop_cbd2_completion(
    uint32_t* attachment_id) {
    return g_audio && g_audio->pop_attachment_completion(attachment_id) ? 1 : 0;
}

extern "C" int32_t m2_host_audio_pop_attachment_completion(
    uint32_t* attachment_id) {
    return g_audio && g_audio->pop_attachment_completion(attachment_id) ? 1 : 0;
}

// Add a sample to the host pool. Returns 1-based sample_id (0 on failure).
// pcm: interleaved S16 stereo, frames: number of frame pairs.
extern "C" uint32_t m2_host_audio_add_sample(const int16_t *pcm,
                                              uint32_t frames) {
    if (!g_audio || !pcm || frames == 0) return 0;
    std::vector<int16_t> buf(pcm, pcm + frames * 2);
    return g_audio->add_sample(std::move(buf));
}

// Bounded voice-lifecycle observation under the existing M2_AUDIO_START_DIAG
// gate: start parameters, release transition, and tap exhaustion. Used to
// separate "the guest stopped the voice" from "the host voice ran out of
// sample data early", which the start/stop census alone cannot distinguish.
// Callers hold the voice slot lock; this only reads the snapshot they pass.
static bool voice_life_diag_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("M2_AUDIO_START_DIAG");
        return value && *value && *value != '0';
    }();
    return enabled;
}
static void voice_life_log(const char* what, uint32_t voice_index,
                           const Voice& v, int32_t total) {
    static uint32_t count = 0;
    if (!voice_life_diag_enabled() || count >= 64u) return;
    ++count;
    std::fprintf(stderr,
                 "[voice-life] n=%u %s vid=%u sid=%u total=%d pos=%g tap=%g "
                 "rate=%g loop=%d lb=%d le=%d sus=%d sb=%d se=%d rel=%d "
                 "rb=%d re=%d tap_done=%d\n",
                 count, what, voice_index, v.sample_id, total, v.pos, v.tap_pos,
                 v.rate, v.looping ? 1 : 0, v.loop_begin, v.loop_end,
                 v.sustain_loop ? 1 : 0, v.sustain_begin, v.sustain_end,
                 v.released ? 1 : 0, v.release_begin, v.release_end,
                 v.tap_finished.load(std::memory_order_acquire) ? 1 : 0);
}

// Start voice playback, returning the 0-based voice index, or -1 when the
// sample is unknown or all voices are busy. The index lets folio contracts
// track ownership (kMaxVoices slots fit a uint32_t mask exactly). An optional
// attachment handle preserves the guest-visible end-of-sample event; the
// generic two-argument API remains unmonitored for standalone callers.
static int32_t m2_host_audio_start_voice_indexed_impl(uint32_t sample_id,
                                                      int looping,
                                                      uint32_t attachment_id,
                                                      int32_t sustain_begin,
                                                      int32_t sustain_end,
                                                      int32_t release_begin,
                                                      int32_t release_end,
                                                      bool use_loop_metadata,
                                                      float rate_hz,
                                                      float gain) {
    if (!g_audio || sample_id == 0) return -1;
    const std::vector<int16_t>* source = g_audio->sample(sample_id);
    const int32_t total = source
                              ? static_cast<int32_t>(source->size() / 2u)
                              : 0;
    if (total <= 0) return -1;
    const bool valid_sustain =
        use_loop_metadata && sustain_begin >= 0 && sustain_end > sustain_begin &&
        sustain_end <= total;
    const bool valid_release =
        use_loop_metadata && release_begin >= 0 && release_end > release_begin &&
        release_end <= total;
    // StartAttachment restarts its existing FIFO channel. Allocating another
    // voice for the same attachment layers the old playback underneath it
    // and can eventually fill the pool with orphaned copies.
    int restart_index = -1;
    if (attachment_id != 0u) {
        for (int i = 0; i < kMaxVoices; ++i) {
            Voice& v = g_audio->voice(i);
            std::unique_lock<std::mutex> voice_lock(v.mutex);
            if (v.active && v.completion_attachment == attachment_id) {
                restart_index = i;
                break;
            }
        }
    }
    for (int i = 0; i < kMaxVoices; ++i) {
        if (restart_index >= 0 && i != restart_index) continue;
        Voice &v = g_audio->voice(i);
        // One slot at a time, claimed under its own lock: the audio thread
        // may be retiring a voice in this very pass, and `active` must not be
        // published before the rest of the claim (or be published twice).
        std::unique_lock<std::mutex> voice_lock(v.mutex);
        if (!v.active || i == restart_index) {
            v.active = true;
            v.paused = false;
            v.pos = 0.0;
            v.tap_pos = 0.0;
            v.tap_finished.store(false, std::memory_order_relaxed);
            v.sample_id = sample_id;
            v.sustain_loop = valid_sustain;
            v.release_loop = valid_release;
            v.released = false;
            v.release_pending = false;
            v.sustain_begin = valid_sustain ? sustain_begin : 0;
            v.sustain_end = valid_sustain ? sustain_end : 0;
            v.release_begin = valid_release ? release_begin : 0;
            v.release_end = valid_release ? release_end : 0;
            v.looping = valid_sustain || (use_loop_metadata && valid_release) ||
                        looping != 0;
            if (valid_sustain) {
                v.loop_begin = sustain_begin;
                v.loop_end = sustain_end;
            } else if (use_loop_metadata && valid_release) {
                // M2 DSPP startup uses a release loop as the initial loop
                // when no sustain loop exists.
                v.loop_begin = release_begin;
                v.loop_end = release_end;
            } else if (looping != 0) {
                v.loop_begin = 0;
                v.loop_end = total;
            } else {
                v.loop_begin = 0;
                v.loop_end = 0;
            }
            v.tap_looping = v.looping;
            v.tap_loop_begin = v.loop_begin;
            v.tap_loop_end = v.loop_end;
            v.tap_release_pending = false;
            // Keep the attachment identity while a sustain loop is active;
            // ReleaseInstrument can then transition this voice and the
            // authored tail can still produce its completion Cue.
            v.completion_attachment =
                (!use_loop_metadata && looping != 0) ? 0u : attachment_id;
            v.rate = rate_hz > 0.0f
                         ? static_cast<double>(rate_hz) / 44100.0
                         : 0.0;
            if (v.rate > 2.0) v.rate = 2.0;
            v.gain = gain < -1.0f ? -1.0f : gain > 1.0f ? 1.0f : gain;
            voice_life_log("start", (uint32_t)i, v, total);
            v.direct = true;  // R2-full: fresh claims play direct until the
                              // guest partition suppresses graph-routed ones.
            return i;
        }
    }
    static std::atomic<uint32_t> full_reports{0};
    if (full_reports.fetch_add(1, std::memory_order_relaxed) < 8u)
        std::fprintf(stderr,
                     "[audio] voice pool full (%d slots): sample=%u attachment=%#x\n",
                     kMaxVoices, sample_id, attachment_id);
    return -1;
}

extern "C" int32_t m2_host_audio_start_voice_indexed(uint32_t sample_id,
                                                     int looping) {
    return m2_host_audio_start_voice_indexed_impl(
        sample_id, looping, 0u, -1, -1, -1, -1, false, 44100.0f, 1.0f);
}

extern "C" int32_t m2_host_audio_start_voice_indexed_for_attachment(
    uint32_t sample_id, int looping, uint32_t attachment_id) {
    return m2_host_audio_start_voice_indexed_impl(sample_id, looping,
                                                  attachment_id, -1, -1, -1,
                                                  -1, false, 44100.0f, 1.0f);
}

extern "C" int32_t
m2_host_audio_start_voice_indexed_for_attachment_with_loops(
    uint32_t sample_id, uint32_t attachment_id, int32_t sustain_begin,
    int32_t sustain_end, int32_t release_begin, int32_t release_end) {
    return m2_host_audio_start_voice_indexed_impl(
        sample_id, 0, attachment_id, sustain_begin, sustain_end,
        release_begin, release_end, true, 44100.0f, 1.0f);
}

extern "C" int32_t
m2_host_audio_start_voice_indexed_for_attachment_with_params(
    uint32_t sample_id, uint32_t attachment_id, int32_t sustain_begin,
    int32_t sustain_end, int32_t release_begin, int32_t release_end,
    float rate_hz, float gain) {
    return m2_host_audio_start_voice_indexed_impl(
        sample_id, 0, attachment_id, sustain_begin, sustain_end,
        release_begin, release_end, true, rate_hz, gain);
}

// Start voice playback. Returns 0 on success, -1 on failure (all voices busy).
extern "C" int32_t m2_host_audio_start_voice(uint32_t sample_id, int looping) {
    return m2_host_audio_start_voice_indexed(sample_id, looping) < 0 ? -1 : 0;
}

// Caller holds v.mutex. Each cursor owns its current DMA segment: crossing
// the muted SDL shadow's boundary must not seek the audible graph tap.
//
// Apply the deferred DSPP release transition at the end of the current DMA
// block. `over` is the fractional distance past the block boundary already
// consumed by the crossing cursor; the voice resumes the same distance into
// the next block (release gap, release loop, or authored release tail). A
// whole-payload sustain loop has an empty tail, so the voice completes at the
// boundary exactly as the M2's queued silence does.
static void voice_apply_release_transition(Voice& v, uint32_t voice_index,
                                        double over, bool tap) {
    if (over < 0.0) over = 0.0;
    double& pos = tap ? v.tap_pos : v.pos;
    bool& looping = tap ? v.tap_looping : v.looping;
    int32_t& loop_begin = tap ? v.tap_loop_begin : v.loop_begin;
    int32_t& loop_end = tap ? v.tap_loop_end : v.loop_end;
    (tap ? v.tap_release_pending : v.release_pending) = false;
    if (v.release_loop) {
        const bool has_release_gap =
            v.release_end != v.sustain_end &&
            v.release_begin != v.sustain_end &&
            v.sustain_begin != v.release_end &&
            v.sustain_begin != v.release_begin;
        loop_begin = v.release_begin;
        loop_end = v.release_end;
        // DSPPReleaseSampleAttachment plays the post-sustain release gap
        // before entering a distinct release loop. If the loop shares an
        // endpoint with the sustain loop, it jumps directly to that loop.
        pos = static_cast<double>(has_release_gap ? v.sustain_end
                                                  : v.release_begin) + over;
        looping = true;
    } else {
        // No release loop: leave the sustain loop at its exclusive end and
        // play the authored tail to the end of the sample. An empty tail
        // (whole-payload loop) completes at the boundary.
        pos = static_cast<double>(v.sustain_end) + over;
        loop_begin = 0;
        loop_end = 0;
        looping = false;
    }
    const std::vector<int16_t>* rel_s =
        g_audio ? g_audio->sample(v.sample_id) : nullptr;
    voice_life_log("release-apply", voice_index, v,
                   rel_s ? static_cast<int32_t>(rel_s->size() / 2u) : -1);
}

static int32_t m2_host_audio_release_voice_locked(Voice& v,
                                                  uint32_t voice_index) {
    if (!v.active) return -1;
    if (!v.sustain_loop || v.released) return 0;

    v.released = true;
    // M2 DSPPReleaseInstrument rewrites the next DMA block only; the
    // currently playing sustain-loop pass always runs to its end first. Keep
    // the cursor and defer the transition to the loop boundary. Only a
    // release outside an active pass applies immediately.
    for (bool tap : {false, true}) {
        const bool looping = tap ? v.tap_looping : v.looping;
        const int32_t begin = tap ? v.tap_loop_begin : v.loop_begin;
        const int32_t end = tap ? v.tap_loop_end : v.loop_end;
        const double pos = tap ? v.tap_pos : v.pos;
        if (looping && end > begin && pos < end)
            (tap ? v.tap_release_pending : v.release_pending) = true;
        else
            voice_apply_release_transition(v, voice_index,
                                        looping && end > begin ? pos - end
                                                               : 0.0,
                                        tap);
    }
    const std::vector<int16_t>* rel_s =
        g_audio ? g_audio->sample(v.sample_id) : nullptr;
    voice_life_log("release", voice_index, v,
                   rel_s ? static_cast<int32_t>(rel_s->size() / 2u) : -1);
    return 0;
}

extern "C" int32_t m2_host_audio_release_voice(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return -1;
    Voice& v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    return m2_host_audio_release_voice_locked(v, voice_index);
}

extern "C" int32_t
m2_host_audio_release_voice_for_attachment(uint32_t attachment_id) {
    if (!g_audio || attachment_id == 0u) return -1;
    for (uint32_t i = 0; i < (uint32_t)kMaxVoices; ++i) {
        Voice& v = g_audio->voice((int)i);
        // Match and release under the same lock: a two-pass scan could hand
        // the transition to a voice the mixer retires in between.
        std::unique_lock<std::mutex> voice_lock(v.mutex);
        if (v.active && v.completion_attachment == attachment_id)
            return m2_host_audio_release_voice_locked(v, i);
    }
    return -1;
}

// Sample currently owned by a voice slot, or 0 when inactive/unknown. Lets
// folio contracts verify ownership before stopping a possibly reused slot:
// direct one-shots end in mix_into; graph-routed one-shots end there only
// after tap_voice_frame has consumed their final frame. The claim is read
// under the slot lock so ownership cannot be sampled mid-claim.
extern "C" uint32_t m2_host_audio_voice_sample(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return 0;
    Voice &v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    return v.active ? v.sample_id : 0;
}

// A PCM buffer can be shared by many cars. The owning attachment, rather
// than that buffer's sample id, distinguishes their recycled voice slots.
extern "C" uint32_t m2_host_audio_voice_attachment(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return 0;
    Voice& v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    return v.active ? v.completion_attachment : 0;
}

// Stop a voice. Returns 0 on success.
extern "C" int32_t m2_host_audio_stop_voice(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return -1;
    Voice& v = g_audio->voice((int)voice_index);
    // The audio thread publishes a completion for a voice it retires in the
    // same frame. Stopping under the slot lock keeps that hand-off atomic:
    // either the mixer owns the completion or this stop does, never both and
    // never neither.
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    v.active = false;
    v.paused = false;
    v.tap_finished.store(false, std::memory_order_relaxed);
    v.completion_attachment = 0;

    return 0;
}

extern "C" int32_t m2_host_audio_pause_voice(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return -1;
    Voice& v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    v.paused = true;
    return 0;
}

extern "C" int32_t m2_host_audio_resume_voice(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return -1;
    Voice& v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    v.paused = false;
    return 0;
}

// R2-full connection partition: suppress (on == 0) or restore direct SDL
// accumulation for one mixer voice. The tap cursor is independent and keeps
// reading a suppressed voice for the graph tick. The set takes the voice slot
// lock so it lands between two mixer frames of the same voice instead of
// inside one: a suppressed voice must keep advancing its muted shadow cursor.
// Out-of-range indices are ignored.
extern "C" void m2_host_audio_set_voice_direct(uint32_t voice_index, int on) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return;
    Voice& v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    v.direct = on != 0;
}

// Drain boundary: hand a still-sounding graph-routed one-shot voice back to
// the direct mixer when its instrument is destroyed. The R2 partition
// suppressed this voice's direct accumulation because an installed graph
// route carried it to the DAC; that route dies with the instrument, so the
// suppression has nothing left to prevent and would only lose the rest of the
// guest's authored one-shot. A looping voice is refused: a loop has no
// authored end, so the device stop must stand. The monitored completion is
// cleared with the instrument that owned it, matching the M2 device, where
// a stopped attachment never signals. Returns 1 when the voice was handed
// over, 0 when there was nothing to drain. The qualification and the handover
// share one lock hold, so a voice retired concurrently cannot be drained.
extern "C" int m2_host_audio_drain_voice_to_direct(uint32_t voice_index) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return 0;
    Voice& v = g_audio->voice((int)voice_index);
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    if (!v.active || v.tap_looping || v.direct) return 0;
    v.direct = true;
    v.completion_attachment = 0;
    return 1;
}

extern "C" void m2_host_audio_set_voice_params(uint32_t voice_index,
                                                float rate_hz, float gain) {
    if (!g_audio || voice_index >= (uint32_t)kMaxVoices) return;
    Voice& v = g_audio->voice((int)voice_index);
    // Live knob traffic: the rate and gain a mixer frame consumes must be a
    // pair this call published, not one field from before and one from after.
    std::unique_lock<std::mutex> voice_lock(v.mutex);
    if (!v.active) return;
    double rate = rate_hz > 0.0f ? static_cast<double>(rate_hz) / 44100.0 : 0.0;
    if (rate > 2.0) rate = 2.0;
    const float g = std::isfinite(gain) ? std::clamp(gain, -1.0f, 1.0f) : 0.0f;
    v.rate = rate;
    v.gain = g;
}

// R2 resident-mix tap: a second read head over the voice's uploaded PCM for
// the guest graph tick. mix_into owns v.pos on the audio thread; this owns
// tap_pos on the guest thread. Both read the same immutable sample bytes
// (the pool only grows, with stable references per the m_pool comment), so
// neither cursor disturbs the other, and the slot lock only serializes the two
// heads against each other: each cursor still advances exactly once per its
// own call, so a tap frame never consumes the mixer's frame and vice versa.
bool AudioShim::tap_voice_frame(uint32_t voice_index, int16_t *l_out,
                                int16_t *r_out) {
    if (l_out) *l_out = 0;
    if (r_out) *r_out = 0;
    if (voice_index >= (uint32_t)kMaxVoices) return false;
    std::unique_lock<std::mutex> voice_lock(m_voices[voice_index].mutex);
    Voice &v = m_voices[voice_index];
    if (!v.active || v.sample_id == 0 || v.paused) return false;
    const std::vector<int16_t> *s = sample(v.sample_id);
    if (!s) return false;
    const int32_t total = static_cast<int32_t>(s->size() / 2u);
    double pos = v.tap_pos;
    if (v.tap_looping && v.tap_loop_end > v.tap_loop_begin &&
        pos >= v.tap_loop_end) {
        if (v.tap_release_pending) {
            voice_apply_release_transition(v, voice_index,
                                        pos - v.tap_loop_end, true);
            pos = v.tap_pos;
        } else {
            pos = static_cast<double>(v.tap_loop_begin);
        }
    }
    if (pos < 0 || pos >= total) {
        if (!v.tap_looping || total <= 0) {
            if (!v.tap_looping && pos >= total) {
                voice_life_log("tap-eof", voice_index, v, total);
                v.tap_finished.store(true, std::memory_order_release);
            }
            return false;
        }
        pos = static_cast<double>(
            v.tap_loop_begin >= 0 && v.tap_loop_begin < total
                ? v.tap_loop_begin : 0);
    }
    v.tap_pos = pos;
    if (l_out) *l_out = static_cast<int16_t>(std::clamp(
        voice_interpolated_sample(v, *s, true, 0), -32768, 32767));
    if (r_out) *r_out = static_cast<int16_t>(std::clamp(
        voice_interpolated_sample(v, *s, true, 1), -32768, 32767));
    v.tap_pos = pos + v.rate;
    if (v.tap_looping && v.tap_loop_end > v.tap_loop_begin &&
        v.tap_pos >= v.tap_loop_end) {
        if (v.tap_release_pending) {
            voice_apply_release_transition(v, voice_index,
                                        v.tap_pos - v.tap_loop_end, true);
        } else {
            const double span =
                static_cast<double>(v.tap_loop_end - v.tap_loop_begin);
            while (v.tap_pos >= v.tap_loop_end) v.tap_pos -= span;
        }
    }
    if (!v.tap_looping && v.tap_pos >= total) {
        v.tap_finished.store(true, std::memory_order_release);
    }
    return true;
}

// Advance the resident-mix tap cursor for a mixer voice by one DSPP frame
// (see AudioShim::tap_voice_frame). Returns 1 with *l_out/*r_out set, or 0
// with outs zeroed when the mixer is down or the voice is inactive, unknown,
// or exhausted. The recomp seam links the weak default in recomp_runtime.c
// when this SDL-backed strong override is absent.
extern "C" int m2_host_audio_voice_tap_frame(uint32_t voice_index,
                                             int16_t *l_out, int16_t *r_out) {
    if (!g_audio) {
        if (l_out) *l_out = 0;
        if (r_out) *r_out = 0;
        return 0;
    }
    return g_audio->tap_voice_frame(voice_index, l_out, r_out) ? 1 : 0;
}

// --- folio implementations ------------------------------------------------------

extern "C" {

int32_t m2_ConnectInstrumentParts(m2_item_t a, const char* part_a,
                                  m2_item_t b, const char* part_b,
                                  int32_t a5, int32_t a6) {
    (void)a5;
    (void)a6;
    std::printf("[audio] ConnectInstrumentParts(%08x '%s', %08x '%s')\n",
                static_cast<unsigned>(a), part_a ? part_a : "?",
                static_cast<unsigned>(b), part_b ? part_b : "?");
    return M2_OK;
}

m2_err_t m2_LinkAttachments(m2_attachment_t* att, m2_item_t instrument, int32_t flags) {
    (void)flags;
    std::printf("[audio] LinkAttachments(att=%p instr=%08x)\n", (void*)att,
                static_cast<unsigned>(instrument));
    return M2_OK;
}

m2_err_t m2_StartAttachment(m2_attachment_t* att) {
    bool started = false;
    if (g_audio && att) {
        // Claim and publish under the slot lock: `active` must never be
        // visible to the mixer or to another start before the rest of the
        // claim, and the slot pointer must not escape its lock. A null
        // attachment still publishes no claim, as before.
        for (int i = 0; i < kMaxVoices; ++i) {
            Voice& v = g_audio->voice(i);
            std::unique_lock<std::mutex> voice_lock(v.mutex);
            if (v.active) continue;
            v.active = true;
            v.pos = 0;
            v.tap_pos = 0;
            v.sample_id = reinterpret_cast<M2Attachment*>(att)->sample_id;
            v.looping = reinterpret_cast<M2Attachment*>(att)->looping;
            v.sustain_loop = false;
            v.release_loop = false;
            v.released = false;
            v.release_pending = false;
            v.loop_begin = 0;
            v.loop_end = 0;
            v.tap_looping = v.looping;
            v.tap_release_pending = false;
            v.tap_loop_begin = 0;
            v.tap_loop_end = 0;
            v.sustain_begin = 0;
            v.sustain_end = 0;
            v.release_begin = 0;
            v.release_end = 0;
            v.completion_attachment = 0;
            v.direct = true;  // R2-full: recycled claims play direct again.
            started = true;
            break;
        }
    }
    std::printf("[audio] StartAttachment(%p)%s\n", (void*)att,
                (!att || started) ? "" : " [no voice — logged only]");
    return M2_OK;
}

float m2_ReadKnobPart(m2_item_t instrument, const char* part) {
    std::printf("[audio] ReadKnobPart(%08x, '%s')\n",
                static_cast<unsigned>(instrument), part ? part : "?");
    return 0.0f;
}

m2_err_t m2_ReleaseInstrument(m2_item_t instrument) {
    std::printf("[audio] ReleaseInstrument(%08x)\n",
                static_cast<unsigned>(instrument));
    return M2_OK;
}

}  // extern "C"

void audio_shim_register() {
    using namespace m2host;
    folio_register(M2_FOLIO_AUDIO, 51, reinterpret_cast<void*>(&m2_LinkAttachments));
    folio_register(M2_FOLIO_AUDIO, 54, reinterpret_cast<void*>(&m2_StartAttachment));
    folio_register(M2_FOLIO_AUDIO, 60, reinterpret_cast<void*>(&m2_ReadKnobPart));
    folio_register(M2_FOLIO_AUDIO, 68, reinterpret_cast<void*>(&m2_ReleaseInstrument));
    folio_register(M2_FOLIO_AUDIO, 57, reinterpret_cast<void*>(&m2_ConnectInstrumentParts));
}
