// audio_shim.h — audio (6) host shim: SDL3-backed software mixer.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_set>

#include <filesystem>

struct SDL_AudioStream;

namespace m2host {

class OggStream;

// One playable binding ("attachment" in OS parlance) -> one mixer voice.
constexpr int kMaxVoices = 32;

struct Voice {
    // Per-voice lock. The live SDL callback (AudioShim::mix_into) and the
    // guest audio surface (start/stop/pause/release, rate/gain updates, the
    // direct/suppressed partition, and the graph tap) read and write the same
    // fields from two threads, so every one of those operations holds this
    // lock for the whole operation on the slot. Lock order is m_cbd2_mutex,
    // then this mutex, then m_pool_mutex; nothing takes them in the other
    // order (a pool upload only ever takes m_pool_mutex on its own).
    // `mutable` so a diagnostic const reader can lock the slot too.
    mutable std::mutex mutex;
    bool active = false;
    bool paused = false;
    bool looping = false;
    bool sustain_loop = false;
    bool release_loop = false;
    bool released = false;
    // M2 DSPPReleaseInstrument only rewrites the next DMA block; the
    // currently playing sustain-loop pass completes first. While this is set
    // the voice finishes the current pass and the release transition is
    // applied at the loop boundary.
    bool release_pending = false;
    float gain = 1.0f;
    // Effective sampler rate as a fraction of the 44.1 kHz DAC rate.
    // The M2 SampleRate knob is continuous, so both playback heads use
    // fractional source-frame cursors.
    double rate = 1.0;
    uint32_t sample_id = 0;  // 1-based; 0 = silent voice
    // Guest attachment Item to signal when a non-looping voice consumes its
    // final sample frame. Zero means the caller does not monitor this voice.
    uint32_t completion_attachment = 0;
    double pos = 0.0;        // SDL play cursor (audio thread via mix_into)
    int32_t loop_begin = 0;  // active loop start, inclusive, in sample frames
    int32_t loop_end = 0;    // active loop end, exclusive, in sample frames
    int32_t sustain_begin = 0;
    int32_t sustain_end = 0;
    int32_t release_begin = 0;
    int32_t release_end = 0;
    // R2 resident-mix tap cursor (guest thread via tap_voice_frame): a second
    // read head over the same immutable uploaded PCM so the guest graph tick
    // observes voice signals at the DSPP frame cadence without disturbing
    // SDL playback. Reset on start; one-shot taps hold exhausted past the end.
    double tap_pos = 0.0;
    // Each read head changes its DMA segment only at its own boundary. The
    // muted SDL shadow can run ahead of the audible graph tap.
    bool tap_looping = false;
    bool tap_release_pending = false;
    int32_t tap_loop_begin = 0;
    int32_t tap_loop_end = 0;
    // R2-full connection partition: false while a graph route carries this
    // voice to the DAC FIFO (guest sets it on graph events). Direct SDL
    // accumulation skips a suppressed voice while the SDL cursor advances
    // for phase alignment. The tap owns one-shot completion while suppressed.
    // Fresh claims default to direct.
    bool direct = true;
    // Published by the guest graph thread after it consumes the final frame;
    // mix_into retires a graph-routed one-shot and queues its completion.
    std::atomic<bool> tap_finished{false};
};

// Host-side attachment object; the game sees it as an opaque handle.
struct M2Attachment {
    uint32_t sample_id = 0;
    bool looping = false;
};

class AudioShim {
public:
    AudioShim();
    ~AudioShim();

    bool init(bool headless, bool controlled = false);
    // Test seam for the no-playback-device path. Production always leaves this
    // null; the focused gate sets it to force `SDL_OpenAudioDeviceStream` to
    // fail, because SDL ignores SDL_AUDIODRIVER once its audio subsystem is
    // already initialised, so the environment cannot reach that branch. This
    // changes only whether a device is obtained, never what the host does
    // with the audio graph, so it cannot mask a behavioural difference.
    void set_force_no_device_for_test(bool force) {
        m_force_no_device = force;
    }
    // Explicit platform-time service for a diagnostic session (735 samples
    // per 60 Hz tick). No asynchronous audio callback advances guest time.
    void diagnostic_tick();
    // Three startup blocks serve the next request and leave two queued for
    // scheduling jitter. DSP order and the guest clock stay unchanged.
    uint32_t prime_dspp_output();
    void shutdown();

    // Voice fields are owned by two threads; `voice(i)` hands out the slot
    // only for use under `lock_voice(i)`. That pair is the supported way to
    // touch a voice from outside the shim.
    Voice& voice(int i) { return m_voices[i]; }
    std::unique_lock<std::mutex> lock_voice(int i) {
        return std::unique_lock<std::mutex>(m_voices[i].mutex);
    }
    // R2 resident-mix tap: advance the guest-thread tap cursor for a voice by
    // one frame, returning the S16 stereo pair without touching the SDL play
    // cursor. One-shot taps hold at 0 once exhausted; looping taps wrap.
    // The returned pair includes the sampler's Amplitude knob/start-tag gain,
    // matching the signal presented at its Output port. Returns false (outs
    // zeroed) when the voice is inactive, unknown, or exhausted.
    bool tap_voice_frame(uint32_t voice_index, int16_t *l_out,
                         int16_t *r_out);

    // Register an owned PCM buffer (interleaved S16 stereo); returns id != 0.
    uint32_t add_sample(std::vector<int16_t> pcm);
    // Borrowed pointer into the pool; stable for the lifetime of the shim
    // because the pool only grows and std::deque::push_back never invalidates
    // references to existing elements. The lookup itself is guarded: guest
    // uploads and audio-thread/tap reads run concurrently.
    const std::vector<int16_t>* sample(uint32_t id) const {
        if (!id) return nullptr;
        std::lock_guard<std::mutex> lock(m_pool_mutex);
        return id <= m_pool.size() ? &m_pool[id - 1] : nullptr;
    }
    bool device_open() const { return m_stream != nullptr; }
    // Silences the open playback stream without changing the device-backed
    // timing path: the stream stays open and its callback keeps consuming at
    // the same rate, only the mixed samples are attenuated. Used by unattended
    // runs so a host on a shared machine makes no sound; it is not a headless
    // or mixer-disabled mode (those change which thread owns the DSPP pump).
    void set_output_muted(bool muted);
    uint32_t rendered_frames() const {
        return m_rendered_frames.load(std::memory_order_relaxed);
    }
    // Sample time for spatial motion, including time between device blocks.
    uint32_t clock_frames() const;

    // --- streamed OGG music (decoded on demand, never resident) ---
    // Open parses headers only; safe on headless (mixing is skipped but the
    // stream still validates the file for tests).
    bool music_open(const std::filesystem::path& ogg);
    void music_play(bool loop = true);
    void music_stop();
    bool music_playing() const { return m_music_on; }
    void set_music_volume(float v) { m_music_volume = v; }

    // Platform sampler for the original fixed-rate stereo CBD2 instrument.
    // The guest owns the source bytes; the host copies and decodes each
    // published buffer, preserving the sampler predictor across attachments.
    bool start_cbd2_stream(uint32_t stream_key, uint32_t attachment_id,
                           const uint8_t* bytes, uint32_t byte_count,
                           float gain);
    // Link result: 0 failed, 1 queued, 2 first attachment already completed.
    int link_cbd2_stream(uint32_t stream_key, uint32_t first_attachment,
                          uint32_t next_attachment, const uint8_t* bytes,
                          uint32_t byte_count);
    // The MPEG subscriber uses the same byte-width SoundSpooler samples with
    // sampler_16_f2.dsp. Those buffers already contain big-endian interleaved
    // stereo S16 output from MPAudioDecode and must bypass CBD2 decoding.
    bool start_pcm16be_stream(uint32_t stream_key, uint32_t attachment_id,
                              const uint8_t* bytes, uint32_t byte_count,
                              float gain);
    int link_pcm16be_stream(uint32_t stream_key, uint32_t first_attachment,
                             uint32_t next_attachment, const uint8_t* bytes,
                             uint32_t byte_count);
    // The sampler's Amplitude knob, applied from the next mixed frame.
    void set_stream_gain(uint32_t stream_key, float gain);
    void unlink_cbd2_stream(uint32_t stream_key, uint32_t first_attachment);
    void reset_cbd2_stream(uint32_t stream_key);
    // Completion events from both streamed CBD2 segments and uploaded PCM
    // voices share one queue so the guest pump preserves the host's event
    // production order across the two platform playback paths.
    bool pop_attachment_completion(uint32_t* attachment_id);

    // Platform sink for DSPP pure-synth output. The guest thread pushes
    // ticked stereo S16 frames at the mixer's 44.1 kHz cadence; the audio
    // thread drains them in mix_into under m_cbd2_mutex. Bounded FIFO (1 s);
    // guest overproduction drops with a counter. M2_DSPP_CAPTURE=path dumps
    // pushed PCM for offline analysis (cap ~10 s), off by default.
    void push_dspp_frames(const int16_t* pcm, uint32_t frames);
    // Frames the asynchronous DSPP worker should produce now to bring the FIFO
    // up to its target of three device blocks: one serves the next request and
    // two absorb producer scheduling jitter.
    uint32_t dspp_fill_request();

    // --- streamed OGG music (decoded on demand, never resident) ---

private:
    // SDL3 stream callback: we synthesize `additional_amount` bytes per call.
    static void mix_cb(void* user, SDL_AudioStream* stream,
                       int additional_amount, int total_amount);
    void mix_into(int16_t* out, int frames);
    friend int m2_opponent_audio_clock_test_main(void);
    friend int m2_audio_dspp_fifo_test_main(void);

    std::deque<std::vector<int16_t>> m_pool;  // stable references on push_back
    // Content hash per pool entry (same index as m_pool). A re-published
    // sample (music republishes on every note) is found by comparing 8-byte
    // hashes instead of every pooled PCM buffer.
    std::vector<uint64_t> m_pool_hash;
    mutable std::mutex m_pool_mutex;         // guards m_pool against uploads
    Voice m_voices[kMaxVoices];
    SDL_AudioStream* m_stream = nullptr;
    std::atomic<uint32_t> m_rendered_frames{0};
    std::atomic<uint64_t> m_sample_clock_epoch_ns{0};
    std::atomic<uint32_t> m_last_mix_frames{0};
    bool m_dspp_primed = false;
    bool m_controlled = false;
    bool m_force_no_device = false;
    bool m_output_muted = false;

    std::unique_ptr<OggStream> m_music;
    bool m_music_on = false;
    bool m_music_loop = true;
    float m_music_volume = 0.6f;
    double m_music_pos = 0.0;  // source-frame cursor (fractional, resampled)

    struct Cbd2Segment {
        uint32_t attachment_id = 0;
        std::vector<int16_t> pcm;  // interleaved S16 stereo
        size_t pos = 0;             // stereo-frame cursor
    };
    struct Cbd2Stream {
        uint32_t key = 0;
        float gain = 1.0f;
        int32_t predictor[2] = {0, 0};
        std::deque<Cbd2Segment> segments;
        std::unordered_set<uint32_t> completed_attachments;
    };

    std::mutex m_cbd2_mutex;
    std::vector<Cbd2Stream> m_cbd2_streams;
    std::deque<uint32_t> m_attachment_completions;

    // DSPP pure-synth mix queue: interleaved stereo S16 pushed by the guest
    // thread, drained by the audio thread in mix_into (same mutex as CBD2).
    // A fixed ring keeps the per-output-sample drain at an index increment
    // instead of a deque block boundary; every access is under m_cbd2_mutex.
    static constexpr uint32_t kDsppFifoCapFrames = 44100u;
    static constexpr uint32_t kDsppFifoCapSamples = kDsppFifoCapFrames * 2u;
    // Length of the fade at each edge of a FIFO underrun (about 2.9 ms), so a
    // gap in the DSPP stream does not click. Controlled runs keep exact samples.
    static constexpr uint32_t kDsppFadeFrames = 128u;
    uint32_t m_dspp_fade_in_pos = kDsppFadeFrames;
    std::vector<int16_t> m_dspp_ring =
        std::vector<int16_t>(kDsppFifoCapSamples, 0);
    uint32_t m_dspp_head = 0;   // index of the next sample to consume
    uint32_t m_dspp_count = 0;  // queued samples (two per stereo frame)

    uint32_t dspp_queued_frames() const { return m_dspp_count / 2u; }
    uint32_t dspp_target_frames() const;
    uint32_t dspp_room_frames() const {
        return (kDsppFifoCapSamples - m_dspp_count) / 2u;
    }
    void dspp_push_sample(int16_t value) {
        if (m_dspp_count >= kDsppFifoCapSamples) return;
        m_dspp_ring[(m_dspp_head + m_dspp_count) % kDsppFifoCapSamples] = value;
        ++m_dspp_count;
    }
    // Consume one interleaved stereo pair. Returns false (and leaves the
    // outputs silent) when a full pair is not queued.
    bool dspp_pop_pair(int16_t& left, int16_t& right) {
        if (m_dspp_count < 2u) {
            left = 0;
            right = 0;
            return false;
        }
        left = m_dspp_ring[m_dspp_head];
        m_dspp_head = (m_dspp_head + 1u) % kDsppFifoCapSamples;
        right = m_dspp_ring[m_dspp_head];
        m_dspp_head = (m_dspp_head + 1u) % kDsppFifoCapSamples;
        m_dspp_count -= 2u;
        return true;
    }
    // Startup priming: silence ahead of the queued samples.
    void dspp_prepend_silence(uint32_t frames) {
        uint32_t samples = frames * 2u;
        if (samples > kDsppFifoCapSamples - m_dspp_count)
            samples = kDsppFifoCapSamples - m_dspp_count;
        m_dspp_head =
            (m_dspp_head + kDsppFifoCapSamples - samples) % kDsppFifoCapSamples;
        for (uint32_t i = 0; i < samples; ++i)
            m_dspp_ring[(m_dspp_head + i) % kDsppFifoCapSamples] = 0;
        m_dspp_count += samples;
    }
    std::atomic<uint64_t> m_dspp_pushed{0};
    std::atomic<uint64_t> m_dspp_dropped{0};
    std::atomic<uint64_t> m_dspp_drained{0};
    uint64_t m_dspp_empty_frames = 0;
    uint64_t m_dspp_observed_frames = 0;
    uint64_t m_dspp_buffer_report_at = 44100;
    // Optional bounded tail capture: final L/R, DSPP L/R, FIFO-empty flag.
    // Allocated before device start; callback only writes existing storage.
    std::filesystem::path m_mix_capture_path;
    std::vector<int16_t> m_mix_capture;
    uint64_t m_mix_capture_frames = 0;
    std::atomic<uint64_t> m_dspp_nonzero{0};
    std::atomic<uint64_t> m_dspp_report_at{441000};
    std::FILE* m_dspp_cap = nullptr;
    uint64_t m_dspp_cap_left = 0;
    std::vector<int16_t> m_mix_scratch;
};
}  // namespace m2host
