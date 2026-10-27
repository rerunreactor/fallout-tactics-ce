// audio.cpp -- milestone 13.  See audio.h and NOTES.md section 69.
//
// Everything the original did through Miles is here: the sound cache
// (`SoundCache`, `SoundProxy::OpenSoundFile @00756d70`), the per-channel voice
// pool (`SoundManager::PlaySoundSampleWithPriority @0071d550`), the distance
// rolloff and pan (`SoundManager::UpdateSound3DPositionsAndVolumes @0071c9e0`),
// and the music stream (`OpenAndStartMusicStream @0071be10`).

#include "audio.h"

#include "file.h"
#include "mp3.h"
#include "vfs.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// MILESTONE 20.  MP3 is decoded by minimp3 (common/mp3.cpp) on every platform.
//
// It used to be decoded by macOS's AudioToolbox, which had two consequences
// this replaces: off Apple platforms -- including the Emscripten build the
// browser lane runs -- there was no decoder at all and every music track and
// every `loc-mis_*` speech line was silently skipped; and because
// `ExtAudioFileOpenURL` takes a *file URL*, even on macOS an MP3 that lives
// inside a `.bos` archive (which is all of the speech) could not be given to
// it.  minimp3 decodes from a byte buffer, so the archive members work too.
//
// The AudioToolbox path is kept, compiled out, for a macOS build that wants to
// A/B the two decoders: `make native CXXFLAGS+=-DFOT_MP3_AUDIOTOOLBOX=1`.
#if defined(FOT_MP3_AUDIOTOOLBOX) && defined(__APPLE__) && !defined(__EMSCRIPTEN__)
#define FOT_HAVE_AUDIOTOOLBOX 1
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace ft {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == Lower(std::string(suffix));
}

// --- the WAV reader -------------------------------------------------------
// RIFF/WAVE, format 1 (PCM, 8 or 16 bit) and 3 (float32); every file under
// sound/Game and sound/Ambient is one of those.  Written out rather than using
// SDL_LoadWAV so the offline (`--audio-dump`, no device) path and the wasm
// build need nothing from SDL but the device itself.
bool DecodeWav(const std::vector<uint8_t>& f, int* rate, int* channels,
               std::vector<int16_t>* out) {
    auto rd32 = [&](size_t o) -> uint32_t {
        return (uint32_t)f[o] | ((uint32_t)f[o + 1] << 8) | ((uint32_t)f[o + 2] << 16) |
               ((uint32_t)f[o + 3] << 24);
    };
    auto rd16 = [&](size_t o) -> uint16_t {
        return (uint16_t)((uint32_t)f[o] | ((uint32_t)f[o + 1] << 8));
    };
    if (f.size() < 44 || std::memcmp(f.data(), "RIFF", 4) != 0 ||
        std::memcmp(f.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t p = 12;
    int fmt = 0, bits = 0;
    *rate = 0;
    *channels = 0;
    while (p + 8 <= f.size()) {
        const uint32_t size = rd32(p + 4);
        const char* id = (const char*)f.data() + p;
        const size_t body = p + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= f.size()) {
            fmt = rd16(body);
            *channels = rd16(body + 2);
            *rate = (int)rd32(body + 4);
            bits = rd16(body + 14);
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t n = std::min<size_t>(size, f.size() - body);
            if (*channels <= 0 || *rate <= 0) {
                return false;
            }
            if (fmt == 1 && bits == 16) {
                out->resize(n / 2);
                for (size_t i = 0; i < out->size(); ++i) {
                    (*out)[i] = (int16_t)rd16(body + i * 2);
                }
            } else if (fmt == 1 && bits == 8) {
                out->resize(n);
                for (size_t i = 0; i < n; ++i) {
                    (*out)[i] = (int16_t)(((int)f[body + i] - 128) << 8);
                }
            } else if (fmt == 3 && bits == 32) {
                out->resize(n / 4);
                for (size_t i = 0; i < out->size(); ++i) {
                    uint32_t u = rd32(body + i * 4);
                    float v;
                    std::memcpy(&v, &u, 4);
                    v = std::max(-1.0f, std::min(1.0f, v));
                    (*out)[i] = (int16_t)std::lrint(v * 32767.0f);
                }
            } else {
                return false;
            }
            return true;
        }
        p = body + size + (size & 1);
    }
    return false;
}

#ifdef FOT_HAVE_AUDIOTOOLBOX
// --- the MP3 reader -------------------------------------------------------
// No MP3 decoder ships in this tree and none is vendored (the FO2 lane has
// stb_vorbis, which is Ogg only).  macOS's own AudioToolbox decodes MPEG-1
// layer III, and game/Makefile already links -framework AudioToolbox for
// SDL2's CoreAudio backend, so that is the decoder.  See NOTES.md 69.7 for the
// non-Apple story.
bool DecodeMp3(const std::string& path, int rate, std::vector<int16_t>* out) {
    CFStringRef cfp =
        CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    if (cfp == nullptr) {
        return false;
    }
    CFURLRef url =
        CFURLCreateWithFileSystemPath(nullptr, cfp, kCFURLPOSIXPathStyle, false);
    CFRelease(cfp);
    if (url == nullptr) {
        return false;
    }
    ExtAudioFileRef file = nullptr;
    OSStatus st = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (st != noErr || file == nullptr) {
        return false;
    }
    AudioStreamBasicDescription d;
    std::memset(&d, 0, sizeof(d));
    d.mSampleRate = rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    d.mChannelsPerFrame = 2;
    d.mBitsPerChannel = 16;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = 4;
    d.mBytesPerPacket = 4;
    if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                                sizeof(d), &d) != noErr) {
        ExtAudioFileDispose(file);
        return false;
    }
    std::vector<int16_t> buf(8192 * 2);
    for (;;) {
        AudioBufferList bl;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = 2;
        bl.mBuffers[0].mDataByteSize = (UInt32)(buf.size() * sizeof(int16_t));
        bl.mBuffers[0].mData = buf.data();
        UInt32 n = 8192;
        if (ExtAudioFileRead(file, &n, &bl) != noErr || n == 0) {
            break;
        }
        out->insert(out->end(), buf.begin(), buf.begin() + (size_t)n * 2);
    }
    ExtAudioFileDispose(file);
    return !out->empty();
}
#endif

// Linear resample to the device rate.  The clips are short effects; the
// original let Miles do this and the method is not recoverable, so this is the
// simplest thing that does not alias audibly.
void Resample(std::vector<int16_t>* pcm, int channels, int from, int to) {
    if (from == to || from <= 0 || channels <= 0 || pcm->empty()) {
        return;
    }
    const size_t inFrames = pcm->size() / (size_t)channels;
    const size_t outFrames =
        (size_t)((double)inFrames * (double)to / (double)from + 0.5);
    if (outFrames == 0) {
        pcm->clear();
        return;
    }
    std::vector<int16_t> out(outFrames * (size_t)channels);
    for (size_t i = 0; i < outFrames; ++i) {
        const double src = (double)i * (double)from / (double)to;
        const size_t i0 = (size_t)src;
        const size_t i1 = std::min(i0 + 1, inFrames - 1);
        const double t = src - (double)i0;
        for (int c = 0; c < channels; ++c) {
            const double a = (*pcm)[i0 * (size_t)channels + (size_t)c];
            const double b = (*pcm)[i1 * (size_t)channels + (size_t)c];
            out[i * (size_t)channels + (size_t)c] =
                (int16_t)std::lrint(a + (b - a) * t);
        }
    }
    *pcm = std::move(out);
}

// MILESTONE 15.  This was its own fopen; it is now the shared reader, so a
// sound whose index entry is a `.bos` handle rather than a real path decodes
// exactly the same way.
bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    std::string err;
    return ft::ReadWholeFile(path, out, &err);
}

// The three-per-channel cap of `PlaySoundSampleWithPriority` (`SoundManager::PlaySoundSampleWithPriority@0071d550`
// l.4331: `if (2 < iVar8) ...`) and of `Play3DSoundSample` (l.4472).
constexpr int kMaxPerChannel = 3;
constexpr int kVoiceCount = 24;

// `SoundManager::PlaySound` reads the channel's default (min, max) distance out
// of a table for every channel except 0 and 5, which carry their own
// (`SoundManager::PlaySound@0071cfd0`).  The table is data the dumps do not contain.
// The units are the ones the ambient records use, which is the only place the
// shipped data states a distance: METRES, `_minDist` 5 and `_maxDist` 25 for a
// zone bed, 2..5 for a point source (script.h's AmbientSound).  These are a
// reconstruction on the same scale -- a rifle carries much further than a tent
// flap.  RECONSTRUCTION -- see NOTES.md 69.3.
struct ChannelDistance {
    float ref, cutoff;
};
constexpr ChannelDistance kChannelDistance[kChannelCount] = {
    {6, 40},       // 0: generic, when the caller passes nothing
    {0, 0},        // 1: music, non-positional
    {6, 40},       // 2: unused by any dump
    {10, 60},      // 3: attack
    {6, 40},       // 4: unused
    {0, 0},        // 5: ambient, carries its own
    {20, 120},     // 6: speech -- a line is meant to be heard, so it carries
    {6, 40},       // 7: unused
};

}  // namespace

Audio::Audio() { m_voices.resize(kVoiceCount); }
Audio::~Audio() { Shutdown(); }

uint32_t Audio::NextRandom() {
    // `Random__GetRandomValueInFloatRange`'s generator, seen inline at
    // `MaybePlayRandomAmbientSound@004dcac0`, `SoundManager::OpenRandomAmbientMusicStream@0071b820`: the MSVC LCG, taking bits 16..30.
    m_rand = m_rand * 0x343fdu + 0x269ec3u;
    return (m_rand >> 16) & 0x7fffu;
}

void Audio::IndexTree(const std::string& dir, const std::string& prefix) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string full = dir + "/" + name;
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        std::error_code sec;
        const std::filesystem::file_status st = std::filesystem::status(full, sec);
        if (sec) {
            continue;
        }
        if (std::filesystem::is_directory(st)) {
            IndexTree(full, rel);
        } else {
            // Later entries win, which is what makes the loose patch files in
            // data/core/sound/game override the archive copies once they are
            // extracted over the top.
            m_index[Lower(rel)] = full;
        }
    }
}

bool Audio::Init(const std::string& assetRoot, bool openDevice, bool capture,
                 std::string* err) {
    m_root = assetRoot;
    while (!m_root.empty() && m_root.back() == '/') {
        m_root.pop_back();
    }
    if (ft::VfsMounted()) {
        // The mounted `.bos` archives already carry `sound/`, `music/` and
        // `locale/` under exactly the names this index is keyed by.
        for (const char* prefix : {"sound/", "music/", "locale/"}) {
            ft::VfsEnumerate(prefix, "", [&](const std::string& key) {
                std::string handle;
                if (ft::VfsResolve(key, &handle)) m_index[key] = handle;
            });
        }
    } else {
        IndexTree(m_root + "/sound", "sound");
        IndexTree(m_root + "/music", "music");
        IndexTree(m_root + "/locale", "locale");
    }
    if (m_index.empty()) {
        if (err != nullptr) {
            *err = "no sound/ or music/ under " + m_root +
                   " (mount sound_0.bos and sound_amb.bos, or extract them and "
                   "copy data/core/music)";
        }
        return false;
    }
    if (openDevice) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            if (err != nullptr) {
                *err = std::string("SDL_InitSubSystem(AUDIO): ") + SDL_GetError();
            }
            return false;
        }
        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = &Audio::SDLCallback;
        want.userdata = this;
        SDL_AudioSpec have;
        m_devId = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (m_devId == 0) {
            if (err != nullptr) {
                *err = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
            }
            return false;
        }
        m_deviceOpen = true;
        SDL_PauseAudioDevice(m_devId, 0);
    } else {
        m_capture = capture;
    }
    m_enabled = true;
    return true;
}

void Audio::Shutdown() {
    if (m_deviceOpen) {
        SDL_CloseAudioDevice(m_devId);
        m_deviceOpen = false;
        m_devId = 0;
    }
    m_enabled = false;
}

void Audio::LoadConfig(const std::string& path) {
    std::vector<uint8_t> raw;
    if (!ReadFile(path, &raw)) {
        return;
    }
    const std::string text((const char*)raw.data(), raw.size());
    // bos.cfg is a list of `{key} = {value}` lines.
    size_t p = 0;
    while (p < text.size()) {
        size_t eol = text.find('\n', p);
        if (eol == std::string::npos) {
            eol = text.size();
        }
        const std::string line = text.substr(p, eol - p);
        p = eol + 1;
        const size_t k0 = line.find('{');
        const size_t k1 = line.find('}', k0 == std::string::npos ? 0 : k0);
        if (k0 == std::string::npos || k1 == std::string::npos) {
            continue;
        }
        const size_t v0 = line.find('{', k1);
        const size_t v1 = line.find('}', v0 == std::string::npos ? k1 : v0);
        if (v0 == std::string::npos || v1 == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(k0 + 1, k1 - k0 - 1);
        const std::string val = line.substr(v0 + 1, v1 - v0 - 1);
        const int n = std::atoi(val.c_str());
        if (key == "sound.masterVolume") {
            m_vol.master = n;
        } else if (key == "sound.musicVolume") {
            m_vol.music = n;
        } else if (key == "sound.sfxVolume") {
            m_vol.sfx = n;
        } else if (key == "sound.speechVolume") {
            m_vol.speech = n;
        }
    }
}

std::string Audio::Resolve(const std::string& relPath) const {
    std::string key = Lower(relPath);
    for (char& c : key) {
        if (c == '\\') {
            c = '/';
        }
    }
    auto it = m_index.find(key);
    if (it != m_index.end()) {
        return it->second;
    }
    // The archive is inconsistent about ".WAV" vs ".wav" and some call sites
    // spell no extension at all; try both audio extensions.
    if (key.find('.') == std::string::npos ||
        (!EndsWith(key, ".wav") && !EndsWith(key, ".mp3"))) {
        for (const char* ext : {".wav", ".mp3"}) {
            it = m_index.find(key + ext);
            if (it != m_index.end()) {
                return it->second;
            }
        }
    }
    return std::string();
}

int Audio::CountVariants(const std::string& stem) const {
    int n = 0;
    while (n < 16 && !Resolve(stem + std::to_string(n + 1)).empty()) {
        ++n;
    }
    return n;
}

std::string Audio::PickVariant(const std::string& stem) {
    const int n = CountVariants(stem);
    if (n <= 0) {
        return Resolve(stem).empty() ? std::string() : stem;
    }
    // `MaybePlayRandomAmbientSound` picks with `(seed >> 16 & 0x7fff) % count`
    // (`MaybePlayRandomAmbientSound@004dcac0`); `Actor::PlayActorAttackSound` uses the same
    // generator for its Attack1..Attack4 pick.
    return stem + std::to_string((int)(NextRandom() % (uint32_t)n) + 1);
}

std::shared_ptr<AudioClip> Audio::LoadClip(const std::string& absPath) {
    auto it = m_cache.find(absPath);
    if (it != m_cache.end()) {
        return it->second;
    }
    auto clip = std::make_shared<AudioClip>();
    bool ok = false;
    if (EndsWith(absPath, ".mp3")) {
        // MILESTONE 20.  minimp3, from the bytes -- so a loose `core/music`
        // file and an MP3 member of a mounted `.bos` decode by the same path,
        // on macOS and in the browser alike.  This was the last placeholder in
        // the audio layer (NOTES.md 69.7, 78).
        std::vector<uint8_t> raw;
        int rate = 0, ch = 0;
        std::vector<int16_t> pcm;
        std::string err;
        if (ReadFile(absPath, &raw) && DecodeMp3Buffer(raw, &rate, &ch, &pcm, &err)) {
            const size_t frames = ch > 0 ? pcm.size() / (size_t)ch : 0;
            Resample(&pcm, ch, rate, kRate);
            clip->channels = ch;
            clip->pcm = std::move(pcm);
            ok = true;
            // One line per decoded track, once (a failed or successful load is
            // cached).  In the browser stdout is the only diagnostic there is,
            // and this is what the e2e asserts on to prove music is not silent.
            std::printf("audio: mp3 %s %d Hz %d ch %.2f s (%zu samples)\n",
                        absPath.c_str(), rate, ch, ch > 0 && rate > 0
                            ? (double)frames / (double)rate : 0.0,
                        clip->pcm.size());
            std::fflush(stdout);
        } else if (!raw.empty()) {
            std::fprintf(stderr, "audio: %s: %s\n", absPath.c_str(), err.c_str());
        }
#ifdef FOT_HAVE_AUDIOTOOLBOX
        if (!ok && !ft::IsVfsHandle(absPath)) {
            std::vector<int16_t> at;
            if (DecodeMp3(absPath, kRate, &at)) {
                clip->channels = 2;
                clip->pcm = std::move(at);
                ok = true;
            }
        }
#endif
    } else {
        std::vector<uint8_t> raw;
        int rate = 0, ch = 0;
        std::vector<int16_t> pcm;
        if (ReadFile(absPath, &raw) && DecodeWav(raw, &rate, &ch, &pcm)) {
            Resample(&pcm, ch, rate, kRate);
            clip->channels = ch;
            clip->pcm = std::move(pcm);
            ok = true;
        }
    }
    if (!ok) {
        clip.reset();
    }
    // A failed load is cached too, so a missing file is reported once.
    m_cache[absPath] = clip;
    m_cacheBytes += clip ? clip->pcm.size() * sizeof(int16_t) : 0;
    // `SoundCache` is refcounted and drops a sound when
    // the last proxy releases it; the eviction policy is not in the dump.  This
    // is the same effect by a simpler rule: over the budget, drop every cached
    // clip nothing is playing, keeping the one just loaded.  It matters because
    // a music track decodes to ~24 MB of PCM and a mission can walk through
    // several.
    constexpr size_t kCacheBudget = 192u * 1024u * 1024u;
    if (m_cacheBytes > kCacheBudget) {
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            bool live = it->second == clip || it->second == m_music.clip;
            if (!live && it->second) {
                for (const Voice& v : m_voices) {
                    if (v.active && v.clip == it->second) {
                        live = true;
                        break;
                    }
                }
            }
            if (!live && it->second && it->second.use_count() <= 1) {
                m_cacheBytes -= it->second->pcm.size() * sizeof(int16_t);
                it = m_cache.erase(it);
            } else {
                ++it;
            }
        }
    }
    return clip;
}

void Audio::SetListener(float x, float y, float z, float rightX, float rightZ) {
    m_lx = x;
    m_ly = y;
    m_lz = z;
    const float len = std::sqrt(rightX * rightX + rightZ * rightZ);
    if (len > 1e-6f) {
        m_rx = rightX / len;
        m_rz = rightZ / len;
    }
}

int Audio::BusVolume(int channel) const {
    // `PlaySound` takes the sfx bus at this+0x41, or the music bus at
    // this+0x3d when the channel is 1.  Speech is channel 6 and
    // rides SoundManager+0x45, the fourth bus bos.cfg names.
    int bus = m_vol.sfx;
    if (channel == kChanMusic) {
        bus = m_vol.music;
    } else if (channel == kChanSpeech) {
        bus = m_vol.speech;
    }
    // `PlaySoundSampleWithPriority`: `vol = perSound * bus * 0.01`
    // (`SoundManager::PlaySoundSampleWithPriority@0071d550`).  The master bus multiplies again.
    return bus * m_vol.master / 100;
}

void Audio::ApplyDistance(Voice& v) {
    if (!v.positional) {
        v.volume = v.baseVolume;
        v.pan = 0;
        return;
    }
    const float dx = v.x - m_lx, dy = v.y - m_ly, dz = v.z - m_lz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    // `UpdateSound3DPositionsAndVolumes` l.3843-3855: when the source is
    // farther than the reference distance the volume is scaled by ref/dist,
    // otherwise it is the base volume; that is `PlaySound`'s own rule too
    // (l.4128-4131), with `volume` being the reference distance, not a gain.
    if (v.refDist > 0 && v.refDist <= d && d > 0) {
        v.volume = (int)std::lround((v.refDist / d) * (float)v.baseVolume);
    } else {
        v.volume = v.baseVolume;
    }
    // l.3776: `pan = dot(unit(source - listener), R) * S + C`, R being the
    // listener's basis vector built at l.3760.  R's components are runtime
    // globals the dumps do not carry, so `R` here is the camera's world-space
    // screen-right vector and the pan is the raw dot product in -1..+1.
    // RECONSTRUCTION -- NOTES.md 69.3.
    if (d > 1e-3f) {
        v.pan = std::max(-1.0f, std::min(1.0f, (dx / d) * m_rx + (dz / d) * m_rz));
    } else {
        v.pan = 0;
    }
    // l.4132-4137: past the cutoff a one-shot is dropped and a loop goes silent.
    if (v.cutoff > 0 && d > v.cutoff) {
        if (v.looping) {
            v.volume = 0;
        } else {
            v.active = false;
        }
    }
}

bool Audio::Play(const SoundRequest& req) {
    if (!m_enabled || req.file.empty()) {
        return false;
    }
    const std::string abs = Resolve(req.file);
    if (abs.empty()) {
        // `SoundManager::PlaySoundByFilename`'s "File not found: %s"
        // (`SoundManager::PlaySoundByFilename@0071ccc0`).
        if (m_log) {
            std::fprintf(stderr, "audio: file not found: %s (%s)\n",
                         req.file.c_str(), req.why);
        }
        return false;
    }
    auto clip = LoadClip(abs);
    if (!clip) {
        if (m_log) {
            std::fprintf(stderr, "audio: could not decode %s\n", abs.c_str());
        }
        return false;
    }

    if (m_deviceOpen) {
        SDL_LockAudioDevice(m_devId);
    }
    // `PlaySoundSampleWithPriority`: count the channel's live samples, and
    // above three either refuse (channel 5) or evict the quietest.
    int live = 0, evict = -1, free = -1;
    for (size_t i = 0; i < m_voices.size(); ++i) {
        Voice& v = m_voices[i];
        if (!v.active) {
            if (free < 0) {
                free = (int)i;
            }
            continue;
        }
        if (v.channel == req.channel) {
            ++live;
            if (evict < 0 || v.volume < m_voices[(size_t)evict].volume) {
                evict = (int)i;
            }
        }
    }
    int slot = free;
    if (live >= kMaxPerChannel) {
        if (req.channel == kChanAmbient || evict < 0) {
            if (m_deviceOpen) {
                SDL_UnlockAudioDevice(m_devId);
            }
            return false;  // "Too many sounds playing..."
        }
        slot = evict;
    }
    if (slot < 0) {
        if (m_deviceOpen) {
            SDL_UnlockAudioDevice(m_devId);
        }
        return false;
    }

    Voice& v = m_voices[(size_t)slot];
    v = Voice();
    v.clip = clip;
    v.active = true;
    v.looping = req.looping;
    v.channel = req.channel;
    v.entityId = req.entityId;
    v.positional = req.positional;
    v.x = req.x;
    v.y = req.y;
    v.z = req.z;
    v.refDist = req.refDist;
    v.cutoff = req.cutoff;
    if (v.positional && v.refDist <= 0 && req.channel < kChannelCount) {
        v.refDist = kChannelDistance[req.channel].ref;
        v.cutoff = kChannelDistance[req.channel].cutoff;
    }
    v.baseVolume = req.baseVolume >= 0 ? req.baseVolume * BusVolume(req.channel) / 100
                                       : BusVolume(req.channel);
    v.file = req.file;
    ApplyDistance(v);
    const bool started = v.active;
    const int volume = v.volume;
    const float pan = v.pan;
    const float dist = v.positional
                           ? std::sqrt((v.x - m_lx) * (v.x - m_lx) +
                                       (v.y - m_ly) * (v.y - m_ly) +
                                       (v.z - m_lz) * (v.z - m_lz))
                           : 0.0f;
    if (m_deviceOpen) {
        SDL_UnlockAudioDevice(m_devId);
    }
    if (!started) {
        if (m_log) {
            std::fprintf(stderr,
                         "audio: DROPPED (%.1f > cutoff %.1f) %s (%s) src(%.1f,%.1f,%.1f) lis(%.1f,%.1f,%.1f)\n",
                         dist, req.cutoff > 0 ? req.cutoff : kChannelDistance[req.channel].cutoff,
                         req.file.c_str(), req.why, req.x, req.y, req.z, m_lx, m_ly, m_lz);
        }
        return false;   // out of range, and not a loop
    }
    if (m_log) {
        LogEntry e;
        e.tick = m_tick;
        e.file = req.file;
        e.volume = volume;
        e.pan = pan;
        e.dist = dist;
        e.channel = req.channel;
        e.why = req.why;
        m_logEntries.push_back(e);
        std::printf("audio %6lld  %-44s vol=%3d pan=%+0.2f dist=%7.1f ch=%d  %s\n",
                    (long long)m_tick, req.file.c_str(), volume, pan, dist,
                    req.channel, req.why);
    }
    return true;
}

bool Audio::PlayUi(const std::string& file, const char* why) {
    SoundRequest r;
    r.file = file;
    r.channel = kChanDefault;
    r.why = why;
    return Play(r);
}

void Audio::StopEntity(int entityId) {
    if (!m_enabled) {
        return;
    }
    if (m_deviceOpen) {
        SDL_LockAudioDevice(m_devId);
    }
    for (Voice& v : m_voices) {
        if (v.active && v.entityId == entityId) {
            v.active = false;
        }
    }
    if (m_deviceOpen) {
        SDL_UnlockAudioDevice(m_devId);
    }
}

bool Audio::PlayMusic(const std::string& name, int loopCount, const char* why) {
    if (!m_enabled) {
        return false;
    }
    const std::string abs = Resolve("music/" + name);
    if (abs.empty()) {
        if (m_log) {
            std::fprintf(stderr, "audio: no music %s\n", name.c_str());
        }
        return false;
    }
    auto clip = LoadClip(abs);
    if (!clip) {
        return false;
    }
    if (m_deviceOpen) {
        SDL_LockAudioDevice(m_devId);
    }
    m_music.clip = clip;
    m_music.pos = 0;
    m_music.loopsLeft = loopCount;
    m_music.active = true;
    m_musicName = name;
    if (m_deviceOpen) {
        SDL_UnlockAudioDevice(m_devId);
    }
    if (m_log) {
        LogEntry e;
        e.tick = m_tick;
        e.file = "music/" + name;
        e.volume = BusVolume(kChanMusic);
        e.channel = kChanMusic;
        e.why = why;
        m_logEntries.push_back(e);
        std::printf("audio %6lld  %-44s vol=%3d pan=+0.00 dist=    0.0 ch=1  %s\n",
                    (long long)m_tick, e.file.c_str(), e.volume, why);
    }
    return true;
}

bool Audio::PlayRandomMusic(const std::string& dir, const char* why) {
    // `OpenRandomAmbientMusicStream`: enumerate the directory, pick with the
    // LCG, and if the pick is the track that just played walk forward until it
    // is not.
    std::vector<std::string> names;
    const std::string prefix = Lower("music/" + dir + "/");
    for (const auto& kv : m_index) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0 &&
            kv.first.find('/', prefix.size()) == std::string::npos &&
            (EndsWith(kv.first, ".mp3") || EndsWith(kv.first, ".wav"))) {
            names.push_back(kv.first.substr(6));  // drop "music/"
        }
    }
    if (names.empty()) {
        return false;
    }
    size_t i = (size_t)(NextRandom() % (uint32_t)names.size());
    for (size_t tries = 0; tries < names.size() && Lower(m_musicName) == names[i];
         ++tries) {
        i = (i + 1) % names.size();
    }
    return PlayMusic(names[i], 1, why);
}

void Audio::StopMusic() {
    if (m_deviceOpen) {
        SDL_LockAudioDevice(m_devId);
    }
    m_music.active = false;
    m_music.clip.reset();
    m_musicName.clear();
    if (m_deviceOpen) {
        SDL_UnlockAudioDevice(m_devId);
    }
}

void Audio::Update(int64_t deltaMs) {
    if (!m_enabled) {
        return;
    }
    if (m_deviceOpen) {
        SDL_LockAudioDevice(m_devId);
    }
    // `UpdateSound3DPositionsAndVolumes`: recompute every live source, then
    // drop the ones that finished.
    for (Voice& v : m_voices) {
        if (v.active) {
            ApplyDistance(v);
        }
    }
    if (m_deviceOpen) {
        SDL_UnlockAudioDevice(m_devId);
    } else {
        MixOffline(deltaMs);
    }
}

void Audio::SDLCallback(void* userdata, uint8_t* stream, int len) {
    Audio* self = (Audio*)userdata;
    std::memset(stream, 0, (size_t)len);
    self->MixInto((int16_t*)stream, len / 4);
}

void Audio::MixInto(int16_t* out, int frames) {
    std::vector<float> acc((size_t)frames * 2, 0.0f);

    for (Voice& v : m_voices) {
        if (!v.active || !v.clip || v.volume <= 0) {
            if (v.active && v.clip && v.volume <= 0 && !v.looping) {
                // A silenced one-shot still runs its clock out.
                v.pos += (size_t)frames * (size_t)v.clip->channels;
                if (v.pos >= v.clip->pcm.size()) {
                    v.active = false;
                }
            }
            continue;
        }
        const AudioClip& c = *v.clip;
        // Equal-power pan; the original handed the pan straight to Miles, whose
        // curve is not recoverable.
        const float g = (float)v.volume / 100.0f;
        const float ang = (v.pan + 1.0f) * 0.25f * (float)M_PI;
        const float gl = v.clip->channels == 2 ? g : g * std::cos(ang) * 1.4142f;
        const float gr = v.clip->channels == 2 ? g : g * std::sin(ang) * 1.4142f;
        for (int i = 0; i < frames; ++i) {
            if (v.pos + (size_t)c.channels > c.pcm.size()) {
                if (v.looping) {
                    v.pos = 0;
                } else {
                    v.active = false;
                    break;
                }
            }
            if (c.channels == 2) {
                acc[(size_t)i * 2] += (float)c.pcm[v.pos] * gl;
                acc[(size_t)i * 2 + 1] += (float)c.pcm[v.pos + 1] * gr;
                v.pos += 2;
            } else {
                const float s = (float)c.pcm[v.pos];
                acc[(size_t)i * 2] += s * gl;
                acc[(size_t)i * 2 + 1] += s * gr;
                v.pos += 1;
            }
        }
    }

    if (m_music.active && m_music.clip) {
        const AudioClip& c = *m_music.clip;
        const float g = (float)BusVolume(kChanMusic) / 100.0f;
        for (int i = 0; i < frames; ++i) {
            if (m_music.pos + (size_t)c.channels > c.pcm.size()) {
                if (m_music.loopsLeft < 0 || --m_music.loopsLeft > 0) {
                    m_music.pos = 0;
                } else {
                    m_music.active = false;
                    break;
                }
            }
            acc[(size_t)i * 2] += (float)c.pcm[m_music.pos] * g;
            acc[(size_t)i * 2 + 1] +=
                (float)c.pcm[m_music.pos + (c.channels == 2 ? 1 : 0)] * g;
            m_music.pos += (size_t)c.channels;
        }
    }

    for (size_t i = 0; i < acc.size(); ++i) {
        const float s = std::max(-32768.0f, std::min(32767.0f, acc[i]));
        out[i] = (int16_t)std::lrint(s);
    }
}

void Audio::MixOffline(int64_t ms) {
    if (!m_capture || ms <= 0) {
        return;
    }
    const int frames = (int)((ms * kRate) / 1000);
    if (frames <= 0) {
        return;
    }
    const size_t at = m_dump.size();
    m_dump.resize(at + (size_t)frames * 2);
    MixInto(m_dump.data() + at, frames);
}

bool Audio::WriteDumpWav(const std::string& path, std::string* err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        if (err != nullptr) {
            *err = "cannot write " + path;
        }
        return false;
    }
    const uint32_t dataBytes = (uint32_t)(m_dump.size() * 2);
    auto w32 = [&](uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                        (uint8_t)(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };
    auto w16 = [&](uint16_t v) {
        uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        std::fwrite(b, 1, 2, f);
    };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(2);
    w32(kRate);
    w32((uint32_t)kRate * 4);
    w16(4);
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    std::fwrite(m_dump.data(), 1, dataBytes, f);
    std::fclose(f);
    return true;
}

int Audio::CountDumpOnsets() const {
    // A 10 ms RMS envelope, and an onset wherever it jumps well above the
    // immediately preceding 80 ms and above a floor taken from the run's own
    // quiet quarter.  Comparing against the RECENT past rather than a peak or
    // a long average is what lets this find footsteps and shots under a
    // continuous ambient bed and a music stream.
    const size_t win = (size_t)(kRate / 100) * 2;   // 10 ms, stereo interleaved
    if (m_dump.size() < win * 16) {
        return 0;
    }
    std::vector<float> env;
    for (size_t i = 0; i + win <= m_dump.size(); i += win) {
        double e = 0;
        for (size_t j = 0; j < win; ++j) {
            const double s = m_dump[i + j];
            e += s * s;
        }
        env.push_back((float)std::sqrt(e / (double)win));
    }
    std::vector<float> sorted = env;
    std::sort(sorted.begin(), sorted.end());
    const float floorLevel = sorted[sorted.size() / 4];
    if (floorLevel < 1.0f && sorted.back() < 1.0f) {
        return 0;   // silence
    }
    const size_t look = 8;   // 80 ms
    int onsets = 0;
    bool armed = true;
    for (size_t i = look; i < env.size(); ++i) {
        double before = 0;
        for (size_t j = i - look; j < i; ++j) {
            before += env[j];
        }
        before /= (double)look;
        const float rise = (float)(before > 1.0 ? env[i] / before : 0.0);
        if (armed && rise > 1.8f && env[i] > floorLevel * 1.5f) {
            ++onsets;
            armed = false;
        } else if (!armed && rise < 1.2f) {
            armed = true;
        }
    }
    return onsets;
}

}  // namespace ft
