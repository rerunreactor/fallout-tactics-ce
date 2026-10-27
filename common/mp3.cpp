#include "mp3.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "../third_party/minimp3/minimp3.h"

#include <cstring>
#include <memory>

namespace ft {
namespace {

// An ID3v2 header is "ID3" + 2 version bytes + flags + a 4-byte synchsafe size
// (7 bits per byte) that does NOT include the 10-byte header itself.
size_t Id3v2Size(const std::vector<uint8_t>& d) {
    if (d.size() < 10 || d[0] != 'I' || d[1] != 'D' || d[2] != '3') return 0;
    const uint8_t flags = d[5];
    size_t size = (static_cast<size_t>(d[6] & 0x7f) << 21) |
                  (static_cast<size_t>(d[7] & 0x7f) << 14) |
                  (static_cast<size_t>(d[8] & 0x7f) << 7) |
                  static_cast<size_t>(d[9] & 0x7f);
    size += 10;
    if ((flags & 0x10) != 0) size += 10;  // a footer, when present
    return size > d.size() ? d.size() : size;
}

// ID3v1 is a fixed 128-byte "TAG" trailer; APEv2 ends with a 32-byte footer
// whose size field covers the whole tag.
size_t TrailerSize(const std::vector<uint8_t>& d) {
    size_t trailer = 0;
    if (d.size() >= 128 && std::memcmp(d.data() + d.size() - 128, "TAG", 3) == 0) {
        trailer = 128;
    }
    const size_t end = d.size() - trailer;
    if (end >= 32 && std::memcmp(d.data() + end - 32, "APETAGEX", 8) == 0) {
        size_t apeSize = 0;
        std::memcpy(&apeSize, d.data() + end - 32 + 12, 4);
        if (apeSize <= end) trailer += apeSize;
    }
    return trailer;
}

}  // namespace

bool DecodeMp3Buffer(const std::vector<uint8_t>& data, int* rateOut, int* channelsOut,
                     std::vector<int16_t>* out, std::string* errorOut) {
    out->clear();
    const size_t head = Id3v2Size(data);
    const size_t tail = TrailerSize(data);
    if (data.size() <= head + tail) {
        *errorOut = "no audio frames (tags only)";
        return false;
    }
    const uint8_t* p = data.data() + head;
    int left = static_cast<int>(data.size() - head - tail);

    // mp3dec_t is ~16 KB of MDCT/QMF state; it must not go on the stack in the
    // wasm build, whose main stack is 5 MB but whose audio thread is not.
    auto dec = std::make_unique<mp3dec_t>();
    mp3dec_init(dec.get());

    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int rate = 0, channels = 0;
    bool any = false;
    for (;;) {
        mp3dec_frame_info_t info;
        std::memset(&info, 0, sizeof(info));
        const int samples = mp3dec_decode_frame(dec.get(), p, left, pcm, &info);
        if (info.frame_bytes <= 0) break;  // no more syncs, or a truncated tail
        p += info.frame_bytes;
        left -= info.frame_bytes;
        if (samples > 0) {
            // The first decoded frame fixes the format. A stream that changes
            // rate or channel count mid-file is not something the shipped data
            // does, and mixing the two would be worse than stopping.
            if (!any) {
                rate = info.hz;
                channels = info.channels;
                any = true;
            } else if (info.hz != rate || info.channels != channels) {
                break;
            }
            out->insert(out->end(), pcm,
                        pcm + static_cast<size_t>(samples) * static_cast<size_t>(channels));
        }
        if (left <= 0) break;
    }
    if (!any || out->empty()) {
        *errorOut = "no decodable MPEG audio frames";
        return false;
    }
    *rateOut = rate;
    *channelsOut = channels;
    return true;
}

}  // namespace ft
