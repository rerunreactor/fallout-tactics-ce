// stream.h -- the byte-stream reader the ported loaders sit on.
//
// The original reads through phoenix's `Stream` class, whose error flags are the
// `*(uint *)(stream + vtbl[1] + 8) |= 2` sequences all over the decompilation
// (std::ios::failbit).  That is modelled here by `m_failed`: once set, every
// further read returns zero and the caller unwinds, exactly as the original's
// `if ((flags & 6) != 0) break;` tests do.

#ifndef FT_STREAM_H
#define FT_STREAM_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ft {

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    explicit Reader(const std::vector<uint8_t>& v) : m_data(v.data()), m_size(v.size()) {}

    size_t Pos() const { return m_pos; }
    size_t Size() const { return m_size; }
    bool Failed() const { return m_failed; }
    void Fail() { m_failed = true; }
    const std::string& Error() const { return m_error; }

    void Fail(const std::string& why) {
        if (!m_failed) {
            m_failed = true;
            m_error = why;
        }
    }

    void Seek(size_t pos) {
        if (pos > m_size) {
            Fail("seek past end");
            return;
        }
        m_pos = pos;
    }

    bool Read(void* out, size_t n) {
        if (m_failed || m_pos + n > m_size) {
            Fail("read past end");
            std::memset(out, 0, n);
            return false;
        }
        std::memcpy(out, m_data + m_pos, n);
        m_pos += n;
        return true;
    }

    const uint8_t* Take(size_t n) {
        if (m_failed || m_pos + n > m_size) {
            Fail("read past end");
            return nullptr;
        }
        const uint8_t* p = m_data + m_pos;
        m_pos += n;
        return p;
    }

    uint8_t U8() {
        uint8_t v = 0;
        Read(&v, 1);
        return v;
    }
    int16_t I16() {
        uint8_t b[2] = {0, 0};
        Read(b, 2);
        return static_cast<int16_t>(static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8));
    }
    uint32_t U32() {
        uint8_t b[4] = {0, 0, 0, 0};
        Read(b, 4);
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }
    int32_t I32() { return static_cast<int32_t>(U32()); }

    // A NUL-terminated byte string.
    std::string CString() {
        std::string out;
        for (;;) {
            if (m_failed || m_pos >= m_size) {
                Fail("unterminated string");
                return out;
            }
            const uint8_t c = m_data[m_pos++];
            if (c == 0) {
                return out;
            }
            out.push_back(static_cast<char>(c));
        }
    }

    // `Stream::ReadFormatToken` / `ParseTokenAndLookupNumericId`: a NUL-terminated
    // tag followed by a NUL-terminated decimal version.  Used by "<sprite>",
    // "<spranim>", "<spranim_img>" and "<zar>" alike.
    bool FormatToken(std::string* tagOut, int32_t* versionOut) {
        *tagOut = CString();
        const std::string version = CString();
        if (m_failed) {
            return false;
        }
        *versionOut = 0;
        for (char c : version) {
            if (c < '0' || c > '9') {
                Fail("non-numeric format version");
                return false;
            }
            *versionOut = *versionOut * 10 + (c - '0');
        }
        return !version.empty();
    }

    // The TAGGED length-prefixed string.  Bit 31 of the count means "the
    // characters that follow are 16 bit", and the low 31 bits are the
    // CHARACTER count, so a wide string occupies `2 * n` bytes.  This is the
    // same rule `mapview/esh.cpp` decodes for entity-stream strings; the world
    // written by `World::Save` (the save-game form) uses it for the
    // <entity_file> class-name table too, where an editor-written `.mis` uses
    // the narrow form.  Reading narrow strings through it is unchanged.
    std::string TaggedString(bool* wideOut = nullptr) {
        const uint32_t raw = U32();
        if (m_failed) return std::string();
        const bool wide = (raw & 0x80000000u) != 0;
        if (wideOut != nullptr) *wideOut = wide;
        const size_t chars = static_cast<size_t>(raw & 0x7fffffffu);
        const size_t bytes = wide ? chars * 2 : chars;
        if (bytes > m_size - m_pos) {
            Fail("bad string length");
            return std::string();
        }
        const uint8_t* p = m_data + m_pos;
        m_pos += bytes;
        if (!wide) return std::string(reinterpret_cast<const char*>(p), chars);
        std::string out;
        out.reserve(chars);
        for (size_t i = 0; i < chars; ++i) {
            const uint32_t c = static_cast<uint32_t>(p[i * 2]) |
                               (static_cast<uint32_t>(p[i * 2 + 1]) << 8);
            out.push_back(c < 0x100 ? static_cast<char>(c) : '?');
        }
        return out;
    }

    // The int32-length-prefixed string the sequence and collection names use.
    std::string LengthPrefixedString() {
        const int32_t length = I32();
        if (m_failed || length < 0 || static_cast<size_t>(length) > m_size - m_pos) {
            Fail("bad string length");
            return std::string();
        }
        std::string out(reinterpret_cast<const char*>(m_data + m_pos), static_cast<size_t>(length));
        m_pos += static_cast<size_t>(length);
        return out;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
    bool m_failed = false;
    std::string m_error;
};

}  // namespace ft

#endif  // FT_STREAM_H
