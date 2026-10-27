// mp3.h -- MPEG-1/2 layer III decoding, from memory.
//
// MILESTONE 20.  Tactics ships its soundtrack as 50 loose MP3s under
// `core/music/` and all of its `loc-mis_*` mission speech as MP3 members
// *inside* the `.bos` archives.  Until now the only decoder in the tree was
// macOS's AudioToolbox, borrowed by `game/audio.cpp`, and it has two problems:
// it does not exist off Apple platforms (so the Emscripten build was silent),
// and `ExtAudioFileOpenURL` wants a *file URL* (so even on macOS every MP3 that
// lives inside an archive was skipped).
//
// This is minimp3 (`../third_party/minimp3/minimp3.h`, CC0, public domain),
// which decodes from a byte buffer and therefore fixes both at once: the same
// code path serves native and wasm, loose files and archive members.
//
// The output is interleaved int16 at the file's own sample rate; the caller
// resamples (`game/audio.cpp`'s `Resample`) exactly as it does for WAV.

#ifndef FT_MP3_H
#define FT_MP3_H

#include <cstdint>
#include <string>
#include <vector>

namespace ft {

// Decode a whole MP3 from memory. On success `*rateOut` is the stream's sample
// rate and `*channelsOut` is 1 or 2; `out` is interleaved int16.
//
// ID3v2 tags at the head and ID3v1/APE trailers are skipped -- minimp3's frame
// scanner would otherwise spend the whole tag looking for a sync word, and a
// 4 KB embedded cover art blob is enough to make it give up.
bool DecodeMp3Buffer(const std::vector<uint8_t>& data, int* rateOut, int* channelsOut,
                     std::vector<int16_t>* out, std::string* errorOut);

}  // namespace ft

#endif  // FT_MP3_H
