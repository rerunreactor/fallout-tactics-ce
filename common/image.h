// image.h -- the format dispatcher, and the JPEG decoder behind it.
//
// PROVENANCE.
//
// The original does NOT pick an image decoder off the file extension.
// `ImageFormatManager` (@ 006d2850) registers six
// `ImageFormat` handlers at start-up, each of which advertises a signature size
// (`GetSignatureSize`, which feeds `m_maxSignatureSize`), and
// `FormatManager::DetectImageFormatFromStream @ 006d2cb0` then
//
//     "Reads a header buffer from a stream with putback support and iterates
//      registered format devices, returning the first whose header-size and
//      signature checks succeed; logs an error if none match."
//
// i.e. it SNIFFS THE MAGIC.  That is why the shipped data is free to disagree
// with itself about case and extension -- `gui/back/title_screen.jpg` is spelt
// `title_screen.JPG` on disk, `wait5.JPG` sits among `wait1..7.jpg`, and the
// exe's own string table has all of them lowercase.
//
// This file is the port's equivalent: `DetectImageFormat` is the sniff, and
// the game's one image entry point (`game::ResourceRoot::Image`, ui.cpp) now
// dispatches on its result instead of on the filename.  Adding JPEG is
// therefore one handler, not a special case at every call site.
//
// The JPEG handler itself is stb_image (third_party/stb/stb_image.h, public
// domain / MIT), compiled JPEG-only: see image.cpp.  The engine has its own
// ZAR decoder (zar.cpp) and its own PNG reader (game/ui.cpp), so stb's other
// decoders are switched off rather than duplicated.

#ifndef FT_IMAGE_H
#define FT_IMAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace ft {

// The formats this port can name.  The original registers six handlers; the
// three that matter to the shipped `gui/` tree are ZAR, PNG and JPEG.  BMP is
// recognised so a mis-sniff reports "bmp, unsupported" rather than "not a zar".
enum class ImageFormat {
    kUnknown = 0,
    kZar,
    kPng,
    kJpeg,
    kBmp,
};

// `FormatManager::DetectImageFormatFromStream @ 006d2cb0`, over a buffer that
// is already in memory (this port reads whole files; there is no putback to
// do).  `n` may be shorter than any signature, in which case the answer is
// kUnknown.  The longest signature here is 8 bytes -- the manager's
// `m_maxSignatureSize`.
ImageFormat DetectImageFormat(const uint8_t* data, size_t n);
inline ImageFormat DetectImageFormat(const std::vector<uint8_t>& file) {
    return DetectImageFormat(file.data(), file.size());
}

// A name for logs and for the `Missing()` list.
const char* ImageFormatName(ImageFormat f);

// Baseline / progressive JPEG -> straight-alpha RGBA, alpha 0xff everywhere
// (JFIF has no alpha channel; every shipped .jpg is an opaque backdrop).
// Greyscale and YCbCr are both handled by stb.
bool DecodeJpegToRGBA(const uint8_t* data, size_t n, int32_t* widthOut, int32_t* heightOut,
                      std::vector<uint8_t>* rgbaOut, std::string* errorOut);
inline bool DecodeJpegToRGBA(const std::vector<uint8_t>& file, int32_t* widthOut,
                             int32_t* heightOut, std::vector<uint8_t>* rgbaOut,
                             std::string* errorOut) {
    return DecodeJpegToRGBA(file.data(), file.size(), widthOut, heightOut, rgbaOut, errorOut);
}

}  // namespace ft

#endif  // FT_IMAGE_H
