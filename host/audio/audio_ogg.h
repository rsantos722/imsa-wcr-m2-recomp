// audio_ogg.h — optional OGG music library and playback policy.
//
// Module responsibilities:
//   - host/audio/ogg_stream.{h,cpp}: the stb_vorbis pull-decoder.
//   - host/audio/audio_shim.{h,cpp}: music_open/play/stop + mix-under-voices on the
//     original SDL3 S16 mixer — same voice model, gain and clamp as pooled
//     CBD2 samples, so the original mix behavior is preserved.
//   - THIS module: name -> file registry over <music-dir>/{audio,sfx}
//     (**/*.ogg) and the tiny play/stop policy on top.
//
// The library is off by default: m2host scans it only when --music-dir is
// given, and no gameplay path selects these files yet. The original CBD2 music
// path is unaffected.
//
// Keys are lowercased stems: "<audio|sfx>/<relpath-without-ext>"
// ("audio/Music/techno_med") plus every bare basename ("techno_med");
// ambiguous basenames keep the first hit in scan order.
#pragma once

#include <cstddef>
#include <string>

namespace m2host {

class AudioShim;

namespace audio_ogg {

// Index every *.ogg under <converted_root>/{audio,sfx}. Safe to call twice
// (re-scans). Missing roots are not an error — the library just stays empty.
void scan_library(const std::string& converted_root);

size_t library_size();

bool exists(const std::string& name);

std::string lookup_path(const std::string& name);

// Resolve + open + start looping through the shim's single music channel.
// Returns false (and logs) when the name is unknown or the file is malformed.
bool play(const std::string& name, AudioShim& shim, bool loop = true);

void stop(AudioShim& shim);

}  // namespace audio_ogg

}  // namespace m2host
