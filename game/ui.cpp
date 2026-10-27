// ui.cpp -- see ui.h for the provenance of every coordinate in this file.

#include "ui.h"

#include "vfs.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "file.h"
#include "image.h"
#include "scene.h"
#include "zar.h"

namespace game {
namespace {

// ---------------------------------------------------------------------------
// A minimal PNG reader.  The fonts are the only PNGs the interface loads and
// all nine of them are 8-bit RGBA (colour type 6), but the other three 8-bit
// types cost four lines each so they are handled too.
// ---------------------------------------------------------------------------
uint32_t BE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int Paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

bool ReadPng(const std::vector<uint8_t>& file, Art* out, std::string* errorOut) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (file.size() < 8 || std::memcmp(file.data(), kSig, 8) != 0) {
        *errorOut = "not a png";
        return false;
    }
    size_t at = 8;
    int32_t w = 0, h = 0;
    int depth = 0, colour = 0, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    while (at + 8 <= file.size()) {
        const uint32_t len = BE32(&file[at]);
        const char* type = reinterpret_cast<const char*>(&file[at + 4]);
        const size_t body = at + 8;
        if (body + len + 4 > file.size()) break;
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = static_cast<int32_t>(BE32(&file[body]));
            h = static_cast<int32_t>(BE32(&file[body + 4]));
            depth = file[body + 8];
            colour = file[body + 9];
            interlace = file[body + 12];
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            plte.assign(file.begin() + body, file.begin() + body + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            trns.assign(file.begin() + body, file.begin() + body + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), file.begin() + body, file.begin() + body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at = body + len + 4;
    }
    if (w <= 0 || h <= 0 || depth != 8 || interlace != 0) {
        *errorOut = "unsupported png (8-bit non-interlaced only)";
        return false;
    }
    const int channels = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1 : colour == 4 ? 2 : 4;
    const size_t stride = static_cast<size_t>(w) * channels;
    std::vector<uint8_t> raw(( stride + 1) * static_cast<size_t>(h));
    uLongf got = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &got, idat.data(), static_cast<uLong>(idat.size())) != Z_OK ||
        got != raw.size()) {
        *errorOut = "png inflate failed";
        return false;
    }
    std::vector<uint8_t> pix(stride * static_cast<size_t>(h));
    for (int32_t y = 0; y < h; ++y) {
        const uint8_t filter = raw[(stride + 1) * static_cast<size_t>(y)];
        const uint8_t* src = &raw[(stride + 1) * static_cast<size_t>(y) + 1];
        uint8_t* dst = &pix[stride * static_cast<size_t>(y)];
        const uint8_t* up = y > 0 ? &pix[stride * static_cast<size_t>(y - 1)] : nullptr;
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= static_cast<size_t>(channels) ? dst[i - channels] : 0;
            const int b = up != nullptr ? up[i] : 0;
            const int c = (up != nullptr && i >= static_cast<size_t>(channels))
                              ? up[i - channels] : 0;
            int v = src[i];
            switch (filter) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += Paeth(a, b, c); break;
                default: break;
            }
            dst[i] = static_cast<uint8_t>(v);
        }
    }
    out->w = w;
    out->h = h;
    out->rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint8_t* s = &pix[i * channels];
        uint8_t* d = &out->rgba[i * 4];
        switch (colour) {
            case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
            case 3: {
                const size_t k = static_cast<size_t>(s[0]) * 3;
                d[0] = k + 2 < plte.size() ? plte[k] : 0;
                d[1] = k + 2 < plte.size() ? plte[k + 1] : 0;
                d[2] = k + 2 < plte.size() ? plte[k + 2] : 0;
                d[3] = s[0] < trns.size() ? trns[s[0]] : 255;
                break;
            }
            case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            default: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
        }
    }
    out->ok = true;
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// MILESTONE 15.  The map value is now the *resolved* path -- a real OS path
// for a loose file, or a `.bos` resource handle -- rather than the relative
// name, because with a mounted resource layer there is no single directory to
// prepend. `ResourceRoot::Resolve` therefore returns the value as it stands.
void ScanDir(const std::string& base, const std::string& rel,
             std::unordered_map<std::string, std::string>* out) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(base + "/" + rel, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string r = rel.empty() ? name : rel + "/" + name;
        std::error_code sec;
        const std::filesystem::file_status st = std::filesystem::status(base + "/" + r, sec);
        if (sec) continue;
        if (std::filesystem::is_directory(st)) {
            ScanDir(base, r, out);
        } else {
            (*out)[Lower(r)] = base + "/" + r;
        }
    }
}

// The same index, built out of the mounted `.bos` archives plus whatever loose
// tree was mounted alongside them.  `ft::VfsEnumerate` already yields the
// lowercased, forward-slashed key this map wants.
void ScanResources(const char* prefix, std::unordered_map<std::string, std::string>* out) {
    ft::VfsEnumerate(prefix, "", [&](const std::string& key) {
        std::string handle;
        if (ft::VfsResolve(key, &handle)) (*out)[key] = handle;
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// Canvas.
// ---------------------------------------------------------------------------
void Canvas::SetClip(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    clip[0] = std::max<int32_t>(0, x0);
    clip[1] = std::max<int32_t>(0, y0);
    clip[2] = std::min<int32_t>(w, x1);
    clip[3] = std::min<int32_t>(h, y1);
}

void Canvas::Pixel(int32_t x, int32_t y, uint32_t rgb, uint8_t a) {
    if (x < clip[0] || y < clip[1] || x >= clip[2] || y >= clip[3] || a == 0) return;
    uint8_t* d = fb + (static_cast<size_t>(y) * w + x) * 4;
    const uint8_t sr = static_cast<uint8_t>(rgb >> 16);
    const uint8_t sg = static_cast<uint8_t>(rgb >> 8);
    const uint8_t sb = static_cast<uint8_t>(rgb);
    if (a == 255) {
        d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255;
        return;
    }
    d[0] = static_cast<uint8_t>((sr * a + d[0] * (255 - a)) / 255);
    d[1] = static_cast<uint8_t>((sg * a + d[1] * (255 - a)) / 255);
    d[2] = static_cast<uint8_t>((sb * a + d[2] * (255 - a)) / 255);
    d[3] = 255;
}

void Canvas::Fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb, uint8_t a) {
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) Pixel(x, y, rgb, a);
    }
}

void Canvas::Blit(const Art& a, int32_t x, int32_t y) { BlitTinted(a, x, y, 1.0f, 1.0f, 1.0f); }

void Canvas::BlitRGBA(const uint8_t* rgba, int32_t w, int32_t h, int32_t x, int32_t y) {
    if (rgba == nullptr || w <= 0 || h <= 0) return;
    for (int32_t sy = 0; sy < h; ++sy) {
        const int32_t dy = y + sy;
        if (dy < clip[1] || dy >= clip[3]) continue;
        for (int32_t sx = 0; sx < w; ++sx) {
            const int32_t dx = x + sx;
            if (dx < clip[0] || dx >= clip[2]) continue;
            const uint8_t* s = &rgba[(static_cast<size_t>(sy) * w + sx) * 4];
            if (s[3] == 0) continue;
            Pixel(dx, dy,
                  (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2],
                  s[3]);
        }
    }
}

void Canvas::BlitTinted(const Art& a, int32_t x, int32_t y, float tr, float tg, float tb) {
    if (!a.ok) return;
    for (int32_t sy = 0; sy < a.h; ++sy) {
        const int32_t dy = y + sy;
        if (dy < clip[1] || dy >= clip[3]) continue;
        for (int32_t sx = 0; sx < a.w; ++sx) {
            const int32_t dx = x + sx;
            if (dx < clip[0] || dx >= clip[2]) continue;
            const uint8_t* s = &a.rgba[(static_cast<size_t>(sy) * a.w + sx) * 4];
            if (s[3] == 0) continue;
            const uint32_t rgb =
                (static_cast<uint32_t>(std::min(255.0f, s[0] * tr)) << 16) |
                (static_cast<uint32_t>(std::min(255.0f, s[1] * tg)) << 8) |
                static_cast<uint32_t>(std::min(255.0f, s[2] * tb));
            Pixel(dx, dy, rgb, s[3]);
        }
    }
}

// ---------------------------------------------------------------------------
// Font.
// ---------------------------------------------------------------------------
bool Font::Load(const std::string& pngPath, int32_t spacing, std::string* errorOut) {
    std::vector<uint8_t> file;
    if (!ft::ReadWholeFile(pngPath, &file, errorOut)) return false;
    Art strip;
    if (!ReadPng(file, &strip, errorOut)) return false;
    m_w = strip.w;
    m_height = strip.h;
    m_spacing = spacing;
    m_rgba.swap(strip.rgba);

    // `AntiFont::BuildFontKerningPairs @0077f0e0`: find the magenta palette
    // entry (FF 00 FF), then push 0, every x in row 0 whose pixel is that
    // entry, and finally the width.  This port has no palette -- the PNG is
    // read as truecolour -- so the test is on the pixel value directly, which
    // is the same test one indirection later.  A separator column is
    // full-height magenta in every shipped strip; only row 0 is examined here,
    // exactly as the original does.
    std::vector<int32_t> sep;
    sep.push_back(0);
    for (int32_t x = 1; x < m_w; ++x) {
        const uint8_t* p = &m_rgba[static_cast<size_t>(x) * 4];
        if (p[0] == 0xFF && p[1] == 0x00 && p[2] == 0xFF) sep.push_back(x);
    }
    if (sep.empty() || sep.back() != m_w) sep.push_back(m_w);
    m_glyphs.clear();
    for (size_t i = 0; i + 1 < sep.size(); ++i) {
        Glyph g;
        g.x0 = sep[i];
        g.w = sep[i + 1] - sep[i];
        m_glyphs.push_back(g);
    }
    // The trailing degenerate one-pixel glyph is the `push(width)` artifact.
    if (!m_glyphs.empty() && m_glyphs.back().w <= 1) m_glyphs.pop_back();
    m_ok = !m_glyphs.empty();
    if (!m_ok) *errorOut = "no glyphs in " + pngPath;
    return m_ok;
}

namespace {
// `AntiFont::GetGlyphIndexForChar @0077fad0` around the unrecoverable
// `FUN_0077faa0`.  The mapping the shipped strips actually carry is ch - 32:
// glyph 0 is transparent (space), 33 is 'A', 65 is 'a', 94 is '~'.
int GlyphIndex(unsigned char ch) { return ch < 32 ? -1 : static_cast<int>(ch) - 32; }
}  // namespace

int32_t Font::Measure(const std::string& s) const {
    if (!m_ok) return 0;
    int32_t w = 0;
    for (unsigned char ch : s) {
        int i = GlyphIndex(ch);
        if (i < 0 || i >= static_cast<int>(m_glyphs.size())) {
            // the case-swap retry
            if (std::isalpha(ch)) i = GlyphIndex(static_cast<unsigned char>(
                std::isupper(ch) ? std::tolower(ch) : std::toupper(ch)));
            if (i < 0 || i >= static_cast<int>(m_glyphs.size())) continue;
        }
        w += m_glyphs[static_cast<size_t>(i)].w + m_spacing;
    }
    return w;
}

int32_t Font::Draw(Canvas* c, int32_t x, int32_t y, const std::string& s, uint32_t rgb) const {
    if (!m_ok) return x;
    const float tr = ((rgb >> 16) & 0xFF) / 255.0f;
    const float tg = ((rgb >> 8) & 0xFF) / 255.0f;
    const float tb = (rgb & 0xFF) / 255.0f;
    for (unsigned char ch : s) {
        int i = GlyphIndex(ch);
        if (i < 0 || i >= static_cast<int>(m_glyphs.size())) {
            if (std::isalpha(ch)) i = GlyphIndex(static_cast<unsigned char>(
                std::isupper(ch) ? std::tolower(ch) : std::toupper(ch)));
            if (i < 0 || i >= static_cast<int>(m_glyphs.size())) continue;
        }
        const Glyph& g = m_glyphs[static_cast<size_t>(i)];
        for (int32_t gy = 0; gy < m_height; ++gy) {
            for (int32_t gx = 0; gx < g.w; ++gx) {
                const uint8_t* p =
                    &m_rgba[(static_cast<size_t>(gy) * m_w + g.x0 + gx) * 4];
                if (p[3] == 0) continue;
                // `dst = lerp(dst, srcRGB * tint, srcAlpha)`: the normal fonts
                // store white RGB so this is tint * coverage, and
                // fnt_monitor_outline stores a grey ramp with binary alpha so
                // its black outline survives the multiply.
                const uint32_t col =
                    (static_cast<uint32_t>(p[0] * tr) << 16) |
                    (static_cast<uint32_t>(p[1] * tg) << 8) |
                    static_cast<uint32_t>(p[2] * tb);
                c->Pixel(x + gx, y + gy, col, p[3]);
            }
        }
        x += g.w + m_spacing;
    }
    return x;
}

void Font::DrawAligned(Canvas* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       const std::string& s, uint32_t rgb, int align) const {
    if (!m_ok) return;
    const int32_t tw = Measure(s);
    int32_t x = x0;
    if (align == 1) x = x1 - tw;
    else if (align == 2) x = x0 + (x1 - x0 - tw) / 2;
    const int32_t y = y0 + ((y1 - y0) - m_height) / 2;
    const int32_t sx0 = c->clip[0], sy0 = c->clip[1], sx1 = c->clip[2], sy1 = c->clip[3];
    c->SetClip(std::max(sx0, x0), std::max(sy0, y0), std::min(sx1, x1), std::min(sy1, y1));
    Draw(c, x, y, s, rgb);
    c->SetClip(sx0, sy0, sx1, sy1);
}

// ---------------------------------------------------------------------------
// ResourceRoot.
// ---------------------------------------------------------------------------
std::vector<std::string> ResourceRoot::List(const std::string& relDir,
                                            const std::string& ext) const {
    std::string dir = relDir, e = ext;
    for (char& c : dir) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<std::string> out;
    for (const auto& kv : m_index) {
        if (kv.first.rfind(dir, 0) != 0) continue;
        if (kv.first.size() < e.size() ||
            kv.first.compare(kv.first.size() - e.size(), e.size(), e) != 0)
            continue;
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end());
    return out;
}

ResourceRoot::ResourceRoot(const std::string& assetsDir) : m_dir(assetsDir) {
    if (ft::VfsMounted()) {
        ScanResources("gui/", &m_index);
        ScanResources("locale/", &m_index);
        ScanResources("tables/", &m_index);
        return;
    }
    ScanDir(m_dir, "gui", &m_index);
    ScanDir(m_dir, "locale", &m_index);
}

std::string ResourceRoot::Resolve(const std::string& rel) const {
    std::string key = Lower(rel);
    for (char& ch : key) {
        if (ch == '\\') ch = '/';
    }
    auto it = m_index.find(key);
    if (it == m_index.end()) return std::string();
    return it->second;
}

const Art* ResourceRoot::Image(const std::string& rel) {
    auto cached = m_cache.find(rel);
    if (cached != m_cache.end()) return &cached->second;
    Art& art = m_cache[rel];
    std::string path = Resolve(rel);
    if (path.empty() && rel.size() > 4 && rel.substr(rel.size() - 4) != ".zar" &&
        rel.substr(rel.size() - 4) != ".png") {
        path = Resolve(rel + ".zar");
    }
    if (path.empty()) {
        m_missing.push_back(rel);
        return &art;
    }
    std::vector<uint8_t> file;
    std::string err;
    if (!ft::ReadWholeFile(path, &file, &err)) {
        m_missing.push_back(rel + ": " + err);
        return &art;
    }
    // MILESTONE 21: the format is SNIFFED, not taken off the extension.
    // `FormatManager::DetectImageFormatFromStream @ 006d2cb0` reads a header
    // buffer and asks each registered handler whether the signature is its
    // own; `ft::DetectImageFormat` (common/image.cpp) is the port's
    // equivalent.  This matters because the shipped tree disagrees with itself
    // about case and spelling -- `gui/back/title_screen.jpg` is
    // `title_screen.JPG` on disk and `wait5.JPG` sits among `wait1..7.jpg` --
    // so the extension decided nothing useful, and a JPEG reached the ZAR
    // reader and came back as "Unknown zar file type".
    const ft::ImageFormat fmt = ft::DetectImageFormat(file);
    switch (fmt) {
        case ft::ImageFormat::kPng:
            if (!ReadPng(file, &art, &err)) m_missing.push_back(rel + ": " + err);
            return &art;
        case ft::ImageFormat::kJpeg:
            // stb_image, compiled JPEG-only; see common/image.cpp.
            if (!ft::DecodeJpegToRGBA(file, &art.w, &art.h, &art.rgba, &err)) {
                m_missing.push_back(rel + ": " + err);
                art.w = art.h = 0;
                return &art;
            }
            art.ok = art.w > 0 && art.h > 0;
            return &art;
        case ft::ImageFormat::kZar:
            break;
        default:
            m_missing.push_back(rel + ": unsupported image format (" +
                                std::string(ft::ImageFormatName(fmt)) + ")");
            return &art;
    }
    ft::Reader stream(file);
    ft::ZarImage zar;
    if (!zar.Read(stream, &err)) {
        m_missing.push_back(rel + ": " + err);
        return &art;
    }
    // A standalone `gui/*.zar` carries its own palette; the fallback is only
    // used by the `.spr` path, where the palette lives in the collection.
    static const std::vector<ft::Color32> kNoPalette;
    if (!zar.DecodeToRGBA(kNoPalette, &art.rgba, &err)) {
        m_missing.push_back(rel + ": " + err);
        return &art;
    }
    art.w = zar.Width();
    art.h = zar.Height();
    art.ok = art.w > 0 && art.h > 0;
    return &art;
}

// ---------------------------------------------------------------------------
// Style -- gui/def_style.cfg, DefaultStyle::DefaultStyle @0072fed0.
// ---------------------------------------------------------------------------
bool Style::Load(ResourceRoot* res, std::string* errorOut) {
    const std::string cfgPath = res->Resolve("gui/def_style.cfg");
    std::map<std::string, std::string> kv;
    if (!cfgPath.empty()) {
        std::vector<uint8_t> file;
        std::string err;
        if (ft::ReadWholeFile(cfgPath, &file, &err)) {
            std::string text(file.begin(), file.end());
            size_t at = 0;
            while (at < text.size()) {
                size_t eol = text.find('\n', at);
                if (eol == std::string::npos) eol = text.size();
                std::string line = text.substr(at, eol - at);
                at = eol + 1;
                const size_t comment = line.find("//");
                if (comment != std::string::npos) line = line.substr(0, comment);
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                auto trim = [](std::string& s) {
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                        s.erase(s.begin());
                    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) ||
                                          s.back() == ';'))
                        s.pop_back();
                };
                trim(k);
                trim(v);
                if (!k.empty()) kv[k] = v;
            }
        }
    }
    auto colour = [&](const char* key, uint32_t fallback) {
        auto it = kv.find(key);
        if (it == kv.end()) return fallback;
        int r = 0, g = 0, b = 0;
        if (std::sscanf(it->second.c_str(), "{Color(%d,%d,%d)}", &r, &g, &b) != 3) return fallback;
        return (static_cast<uint32_t>(r & 0xFF) << 16) | (static_cast<uint32_t>(g & 0xFF) << 8) |
               static_cast<uint32_t>(b & 0xFF);
    };
    textTBD = colour("color_textTBD", textTBD);
    buttonYellow = colour("color_buttonYellow", buttonYellow);
    buttonText = colour("color_buttonText", buttonText);
    panelLight = colour("color_panelLight", panelLight);
    panelFace = colour("color_panelFace", panelFace);
    panelShadow = colour("color_panelShadow", panelShadow);
    panelDkShadow = colour("color_panelDkShadow", panelDkShadow);
    titlebarFill = colour("color_titlebarFill", titlebarFill);
    titlebarText = colour("color_titlebarText", titlebarText);
    windowFill = colour("color_windowFill", windowFill);
    windowText = colour("color_windowText", windowText);
    selectionFill = colour("color_selectionFill", selectionFill);
    selectionText = colour("color_selectionText", selectionText);

    auto path = [&](const char* key, const char* fallback) {
        auto it = kv.find(key);
        std::string v = it == kv.end() ? std::string(fallback) : it->second;
        if (!v.empty() && v.front() == '"') v.erase(v.begin());
        if (!v.empty() && v.back() == '"') v.pop_back();
        return res->Resolve(v);
    };
    std::string err;
    // The `spacing` arguments are FontManager::GetOrCreateFont's second
    // parameter, `DefaultStyle::DefaultStyle@0072fed0`: 0 everywhere except font.lite (-1)
    // and font.consoleOutline (-2).
    bool ok = true;
    ok &= normal.Load(path("font_normal", "gui/font/fnt_monitor.png"), 0, &err);
    ok &= console.Load(path("font_console", "gui/font/fnt_monitor.png"), 0, &err);
    consoleOutline.Load(path("font_consoleOutline", "gui/font/fnt_monitor_outline.png"), -2, &err);
    consoleLarge.Load(path("font_consoleLarge", "gui/font/fnt_monitor.png"), 0, &err);
    ok &= lite.Load(path("font_lite", "gui/font/fnt_lite.png"), -1, &err);
    ok &= medium.Load(path("font_medium", "gui/font/fnt_medium.png"), 0, &err);
    ok &= large.Load(path("font_large", "gui/font/fnt_large.png"), 0, &err);
    tallThin.Load(path("font_tallThin", "gui/font/fnt_tallthin.png"), 0, &err);
    if (!ok) *errorOut = "font: " + err;
    return ok;
}

// ---------------------------------------------------------------------------
// Locale.
// ---------------------------------------------------------------------------
// MILESTONE 10.  A mission string table: `KEY = {` ... `}` where the value may
// run over many lines and contains literal "\n" escapes for the paragraph
// breaks.  The shipped `MIS_01_Speech.txt` writes the debrief paragraphs that
// way.
void Locale::LoadMissionFile(const std::string& path) {
    std::vector<uint8_t> file;
    std::string err;
    if (!ft::ReadWholeFile(path, &file, &err)) return;
    const std::string t(file.begin(), file.end());
    std::vector<std::string> lines;
    {
        size_t at = 0;
        while (at <= t.size()) {
            size_t eol = t.find('\n', at);
            if (eol == std::string::npos) eol = t.size();
            std::string l = t.substr(at, eol - at);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            if (eol == t.size()) break;
            at = eol + 1;
        }
    }
    auto trim = [](std::string v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
            v.erase(v.begin());
        return v;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.compare(0, 2, "//") == 0) continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const size_t ob = l.find('{', eq);
        if (ob == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq));
        if (k.size() > 1 && k.front() == '{' && k.back() == '}') k = k.substr(1, k.size() - 2);
        if (k.empty() || k.find(' ') != std::string::npos) continue;
        // Collect from after '{' to the matching '}', which may be many lines
        // down -- the debrief paragraphs are written that way.
        std::string body = l.substr(ob + 1);
        size_t j = i;
        size_t cb = body.find('}');
        while (cb == std::string::npos && j + 1 < lines.size()) {
            ++j;
            body += " " + lines[j];
            cb = body.find('}');
        }
        if (cb != std::string::npos) body = body.substr(0, cb);
        i = j;
        // Squeeze the runs of whitespace the layout uses; the explicit "\n"
        // escapes stay for the drawing code to break on.
        std::string out;
        bool sp = false;
        for (char ch : body) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                sp = true;
                continue;
            }
            if (sp && !out.empty()) out.push_back(' ');
            sp = false;
            out.push_back(ch);
        }
        m_map[k] = out;
    }
}

void Locale::LoadDir(const ResourceRoot& res, const std::string& relDir) {
    // The shipped tables are `key = {value}` with `//` comments and CRLF ends.
    // Every file in the directory is folded into one map, which is what the
    // separate table objects (`LocalizedStringsObject`, `DAT_008bd8f4`, ...)
    // amount to for a reader that never writes.
    static const char* const kFiles[] = {
        // MILESTONE 19: `gui/help_text.txt` is the game's OWN "POPUP HELP"
        // table -- the tooltip for every game-bar widget, keyed `sentry1`..
        // `sentry3`, `overwatch`, `stand`/`crouch`/`prone`, `hand_*`,
        // `mmmap`/`mmtext`, `actionPoints`/`hitPoints`/`armorClass`,
        // `squadmember`, `end_turn`, and the five window buttons.  It was never
        // read, so every game-bar tooltip in this port was an invented string.
        "gui/gui_text.txt", "gui/help_text.txt", "gui/help.txt", "game/stat.txt", "game/skill.txt",
        "game/perk.txt",    "game/trait.txt", "game/weapon.txt", "game/items.txt",
        "game/misc.txt",    "game/combat.txt", "game/ammo.txt",  "game/category.txt",
        "game/race.txt",    "game/reason.txt", "game/chems.txt",
        // MILESTONE 12: the campaign's own location names
        // (`mission_name_01` = "Brahmin Wood", `mission_name_B01` =
        // "BOS Bunker Alpha") and the encounter strings.
        "game/generic_mission.txt", "game/encounters.txt", "game/characters.txt"};
    for (const char* f : kFiles) {
        const std::string path = res.Resolve(relDir + "/" + f);
        if (path.empty()) continue;
        std::vector<uint8_t> file;
        std::string err;
        if (!ft::ReadWholeFile(path, &file, &err)) continue;
        const std::string text(file.begin(), file.end());
        size_t at = 0;
        while (at < text.size()) {
            size_t eol = text.find('\n', at);
            if (eol == std::string::npos) eol = text.size();
            std::string line = text.substr(at, eol - at);
            at = eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t c = line.find("//");
            if (c == 0) continue;
            const size_t eq = line.find('=');
            const size_t ob = line.find('{', eq == std::string::npos ? 0 : eq);
            const size_t cb = line.rfind('}');
            if (eq == std::string::npos || ob == std::string::npos || cb == std::string::npos ||
                cb <= ob) {
                continue;
            }
            std::string k = line.substr(0, eq);
            // `weapon.txt` writes its keys braced: `{mode_single} = {Single}`.
            while (!k.empty() && (std::isspace(static_cast<unsigned char>(k.back())))) k.pop_back();
            while (!k.empty() && std::isspace(static_cast<unsigned char>(k.front())))
                k.erase(k.begin());
            if (k.size() > 1 && k.front() == '{' && k.back() == '}') k = k.substr(1, k.size() - 2);
            if (k.empty()) continue;
            m_map.emplace(k, line.substr(ob + 1, cb - ob - 1));
        }
    }
}

const std::string& Locale::Get(const std::string& key) const {
    auto it = m_map.find(key);
    if (it != m_map.end()) return it->second;
    m_scratch = key;
    return m_scratch;
}

bool Locale::Has(const std::string& key) const { return m_map.count(key) != 0; }

}  // namespace game
