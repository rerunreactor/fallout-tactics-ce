// audio.h -- milestone 13: sound and music.
//
// The original used the Miles Sound System (`Mss32.dll`, the `_AIL_*` imports
// all over `SoundManager::*` in the Ghidra export).  Miles is gone, so this is
// SDL2's raw audio device plus the mixer below; every rule the mixer applies --
// the distance rolloff, the pan, the three-voices-per-channel cap, the volume
// buses -- is ported from `phoenix/sound/sound_manager.cpp` and cited at the
// point of use in audio.cpp.
//
// Nothing here talks to Scene; the caller passes world coordinates and a file
// name, exactly as `SoundManager::PlaySound @0071cfd0` does.

#ifndef FOTGAME_AUDIO_H
#define FOTGAME_AUDIO_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ft {

// `SoundManager::PlaySound`'s last argument.  The original is an int with no
// enumeration in any dump; these are the values the call sites in the dumps
// pass, named after what passes them:
//
//   0  Actor::PlayActorAttackSound's neighbours and every UI click
//        (`SoundManager::PlaySoundByFilename(..., 0, 0)`, `WWorldWindow::HandleClickAtPoint@006ba2a0`
//        l.22946) -- non-positional, sfx bus.
//   1  the music bus: `PlaySound` reads `this+0x3d` (the music volume) instead
//        of `this+0x41` (the sfx volume) when `channel == 1`.
//   3  Actor::PlayActorAttackSound.
//   5  ambient (`MaybePlayRandomAmbientSound`);
//        the one channel `PlaySoundSampleWithPriority` refuses to evict for.
//   6  speech (Actor::DoEngineThink's BrainDefault line).
//
// 2, 4 and 7 are not passed by anything in the dumps.  `PlaySound` looks the
// per-channel default (min, max) distance up in a table for every channel but
// 0 and 5, which pass their own; the table's contents are NOT in the dumps and
// the values in audio.cpp's kChannelDistance are a reconstruction.
enum SoundChannel {
    kChanDefault = 0,
    kChanMusic = 1,
    kChanAttack = 3,
    kChanAmbient = 5,
    kChanSpeech = 6,
    kChannelCount = 8,
};

// The four buses of `bos.cfg` -- `{sound.masterVolume}`, `{sound.musicVolume}`,
// `{sound.sfxVolume}`, `{sound.speechVolume}`, all 0..100.  They are
// SoundManager+0x39, +0x3d, +0x41 and +0x45, all initialised to 100 by the
// constructor.
struct Volumes {
    int master = 100;
    int music = 60;    // bos.cfg ships 60
    int sfx = 100;
    int speech = 100;
};

// One decoded clip, always 16-bit signed at the device rate.  `channels` is 1
// or 2; a mono clip is panned, a stereo one is played as-is.
struct AudioClip {
    int channels = 1;
    std::vector<int16_t> pcm;  // interleaved
    size_t Frames() const { return channels ? pcm.size() / (size_t)channels : 0; }
};

// A request, mirroring `SoundManager::PlaySound`'s argument list.
struct SoundRequest {
    std::string file;          // "sound/game/riflesingle1.wav", asset-relative
    int entityId = -1;         // PlaySound's `entity_id`; -1 == none
    bool positional = false;
    float x = 0, y = 0, z = 0; // world units
    float refDist = 0;         // PlaySound's `volume` argument -- see audio.cpp
    float cutoff = 0;          // PlaySound's `cutoff_distance`
    bool looping = false;
    int channel = kChanDefault;
    int baseVolume = -1;       // 0..100; -1 == the channel's bus volume
    const char* why = "";      // for --audio-log only
};

// The mixer.  One instance, owned by main.cpp.
class Audio {
  public:
    Audio();
    ~Audio();

    // `soundRoot` is the directory that holds `sound/` and `music/` -- the
    // same value as --assets.  Opening the device is optional: a headless run
    // with --audio-dump mixes into a buffer instead and never touches SDL.
    // `capture` makes a device-less run accumulate its mix for --audio-dump;
    // without it a headless run mixes nothing and costs nothing.
    bool Init(const std::string& assetRoot, bool openDevice, bool capture,
              std::string* err);
    void Shutdown();

    bool Enabled() const { return m_enabled; }
    void SetLog(bool on) { m_log = on; }
    void SetVolumes(const Volumes& v) { m_vol = v; }
    const Volumes& GetVolumes() const { return m_vol; }
    // bos.cfg's `{key} = {value}` lines.
    void LoadConfig(const std::string& path);

    // `SoundManager::Set3DListenerOrientation @0071e580`: the listener is the
    // camera.  `right` is the world-space direction that maps to +x on screen;
    // the pan is a dot product with it (see audio.cpp).
    void SetListener(float x, float y, float z, float rightX, float rightZ);

    // `SoundManager::PlaySound` / `PlaySoundByFilename`.  Returns false if the
    // file could not be resolved or every slot of the channel was busy.
    bool Play(const SoundRequest& req);
    // Convenience for the non-positional case.
    bool PlayUi(const std::string& file, const char* why);

    void StopEntity(int entityId);

    // `SoundManager::OpenAndStartMusicStream @0071be10` -- `name` is relative
    // to the music directory, e.g. "battle/MX_BA_GEN.MP3".  loop < 0 loops for
    // ever.
    bool PlayMusic(const std::string& name, int loopCount, const char* why);
    // `SoundManager::OpenRandomAmbientMusicStream @0071b820`: pick a random
    // track out of `dir`, never the one that just played.
    bool PlayRandomMusic(const std::string& dir, const char* why);
    void StopMusic();
    const std::string& CurrentMusic() const { return m_musicName; }

    // `SoundManager::UpdateSound3DPositionsAndVolumes @0071c9e0`, plus the
    // stream pump.  `deltaMs` is the frame's own step, so a headless fixed-step
    // run and a live one behave the same.
    void Update(int64_t deltaMs);

    // Resolution, case-insensitively, against the asset tree.  Returns "" when
    // there is no such file.  Public because the sound-name reconstructions in
    // audio_names.cpp probe for variants.
    std::string Resolve(const std::string& relPath) const;
    // How many `<stem><n>.wav` variants exist, n from 1.  0 means only the
    // bare stem (or nothing).
    int CountVariants(const std::string& stem) const;
    // Deterministic pick of one variant, using the mixer's own LCG -- the
    // original's `seed = seed * 0x343fd + 0x269ec3; (seed >> 16) & 0x7fff`
    // (`SoundManager::OpenRandomAmbientMusicStream@0071b820`, `MaybePlayRandomAmbientSound@004dcac0`).
    std::string PickVariant(const std::string& stem);
    void SeedRandom(uint32_t s) { m_rand = s; }
    uint32_t NextRandom();

    // --audio-dump: mix `ms` more milliseconds into the capture buffer.  Only
    // meaningful when Init() was called with openDevice == false.
    void MixOffline(int64_t ms);
    bool WriteDumpWav(const std::string& path, std::string* err);
    int64_t DumpFrames() const { return (int64_t)m_dump.size() / 2; }
    // Onsets in the captured mix: a short-window RMS that crosses up through
    // the threshold.  The audio checks report this.
    int CountDumpOnsets() const;

    // The log, for the verifier.  Each entry is one PlaySound.
    struct LogEntry {
        int64_t tick = 0;
        std::string file;
        int volume = 0;
        float pan = 0;
        float dist = 0;
        int channel = 0;
        std::string why;
    };
    const std::vector<LogEntry>& Log() const { return m_logEntries; }
    void SetTick(int64_t t) { m_tick = t; }

    // Device rate.  Everything is resampled to it at load.
    static constexpr int kRate = 44100;

  private:
    struct Voice {
        std::shared_ptr<AudioClip> clip;
        size_t pos = 0;
        bool active = false;
        bool looping = false;
        int channel = 0;
        int entityId = -1;
        bool positional = false;
        float x = 0, y = 0, z = 0;
        float refDist = 0, cutoff = 0;
        int baseVolume = 0;   // 0..100, after the bus
        int volume = 0;       // 0..100 after the distance rolloff
        float pan = 0;        // -1 .. +1
        std::string file;
    };
    struct MusicStream {
        std::shared_ptr<AudioClip> clip;
        size_t pos = 0;
        int loopsLeft = 0;   // < 0 == forever
        bool active = false;
    };

    std::shared_ptr<AudioClip> LoadClip(const std::string& absPath);
    void IndexTree(const std::string& dir, const std::string& prefix);
    void MixInto(int16_t* out, int frames);
    static void SDLCallback(void* userdata, uint8_t* stream, int len);
    void ApplyDistance(Voice& v);
    int BusVolume(int channel) const;

    bool m_enabled = false;
    bool m_deviceOpen = false;
    bool m_log = false;
    std::string m_root;
    Volumes m_vol;
    uint32_t m_devId = 0;

    // lowercase asset-relative path -> real absolute path
    std::map<std::string, std::string> m_index;

    std::map<std::string, std::shared_ptr<AudioClip>> m_cache;
    size_t m_cacheBytes = 0;
    std::vector<Voice> m_voices;
    MusicStream m_music;
    std::string m_musicName;

    float m_lx = 0, m_ly = 0, m_lz = 0;
    float m_rx = 1, m_rz = 0;   // the listener's right vector, world space

    uint32_t m_rand = 1;
    int64_t m_tick = 0;
    std::vector<LogEntry> m_logEntries;
    std::vector<int16_t> m_dump;   // stereo, device rate
    bool m_capture = false;
};

}  // namespace ft

#endif  // FOTGAME_AUDIO_H
