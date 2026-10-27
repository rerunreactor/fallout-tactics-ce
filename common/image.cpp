// image.cpp -- see image.h for the provenance of the dispatcher.
//
// stb_image is compiled here, and ONLY here, with every decoder but JPEG
// switched off:
//
//   * ZAR is the engine's own format and has its own decoder (zar.cpp);
//   * PNG is already read by game/ui.cpp's `ReadPng` (the nine interface fonts
//     and `gui/back/game_bar_fill.png` go through it), so turning stb's on
//     would give the same file two decoders with two sets of bugs;
//   * GIF / PSD / TGA / HDR / PIC / PNM are not shipped by the game at all.
//
// STBI_NO_STDIO drops the `FILE*` entry points: everything this port loads is
// already a buffer, and the wasm build has no host filesystem for them anyway.
// STBI_NO_FAILURE_STRINGS is deliberately NOT set -- `stbi_failure_reason()` is
// what the `Missing()` list reports.

#include "image.h"

#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
// The vendored header is kept byte-identical to upstream, so its own warnings
// are suppressed here rather than patched out of it (`stbi__is_16_main` takes a
// parameter it does not use once the 16-bit loaders are compiled out).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "../third_party/stb/stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ft {

// ---------------------------------------------------------------------------
// `FormatManager::DetectImageFormatFromStream @ 006d2cb0` -- the signature
// checks, in the order a registered handler would be asked.
//
// The magic numbers:
//   "<zar>\0"                 the engine's own tag, ImageZAR::ReadFromStream
//                             @ 006f7d50 (zar.cpp: the literal is the
//                             bracketed one, not "ZAR").
//   89 50 4E 47 0D 0A 1A 0A   the PNG signature -- 8 bytes, and the longest
//                             here, so it is the manager's m_maxSignatureSize.
//   FF D8 FF                  SOI plus the first marker byte of any JFIF /
//                             Exif / raw-tables JPEG.  The task's rule is
//                             "FF D8 -> JPEG"; the third byte is checked as
//                             well because every marker after SOI begins FF
//                             and it costs nothing, and stb will reject
//                             anything that slips through regardless.
//   "BM"                      Windows bitmap, recognised but not decoded.
// ---------------------------------------------------------------------------
ImageFormat DetectImageFormat(const uint8_t* data, size_t n) {
    if (data == nullptr) return ImageFormat::kUnknown;
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n >= 8 && std::memcmp(data, kPngSig, 8) == 0) return ImageFormat::kPng;
    if (n >= 6 && std::memcmp(data, "<zar>", 6) == 0) return ImageFormat::kZar;
    if (n >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
        return ImageFormat::kJpeg;
    }
    if (n >= 2 && data[0] == 'B' && data[1] == 'M') return ImageFormat::kBmp;
    return ImageFormat::kUnknown;
}

const char* ImageFormatName(ImageFormat f) {
    switch (f) {
        case ImageFormat::kZar: return "zar";
        case ImageFormat::kPng: return "png";
        case ImageFormat::kJpeg: return "jpeg";
        case ImageFormat::kBmp: return "bmp";
        default: return "unknown";
    }
}

bool DecodeJpegToRGBA(const uint8_t* data, size_t n, int32_t* widthOut, int32_t* heightOut,
                      std::vector<uint8_t>* rgbaOut, std::string* errorOut) {
    if (widthOut != nullptr) *widthOut = 0;
    if (heightOut != nullptr) *heightOut = 0;
    if (rgbaOut != nullptr) rgbaOut->clear();
    if (data == nullptr || n == 0) {
        if (errorOut != nullptr) *errorOut = "empty jpeg buffer";
        return false;
    }
    // stb takes an int length, and the largest shipped JPEG is 557 KB.
    if (n > static_cast<size_t>(0x7fffffff)) {
        if (errorOut != nullptr) *errorOut = "jpeg too large";
        return false;
    }
    int w = 0, h = 0, channelsInFile = 0;
    // 4 = force RGBA out.  A JFIF stream has no alpha, so stb fills 0xff; the
    // backdrops are opaque and the interface blits them with alpha blending.
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(n), &w, &h, &channelsInFile, 4);
    if (px == nullptr) {
        if (errorOut != nullptr) {
            const char* why = stbi_failure_reason();
            *errorOut = std::string("jpeg decode failed: ") + (why != nullptr ? why : "?");
        }
        return false;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(px);
        if (errorOut != nullptr) *errorOut = "jpeg has zero-sized image";
        return false;
    }
    if (rgbaOut != nullptr) {
        rgbaOut->assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    }
    stbi_image_free(px);
    if (widthOut != nullptr) *widthOut = w;
    if (heightOut != nullptr) *heightOut = h;
    return true;
}

}  // namespace ft
