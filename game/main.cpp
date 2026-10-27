// main.cpp -- fotgame: milestone 6, the first interactive build.
//
//   ./fotgame <mis> [--tiles d] [--sprites d] [--ents d] [--inside x,z] [--time HH:MM]
//             [--frames N --screenshot out.png] [--centre x,z] [--fps]
//
// A native SDL2 window (and, built with `make wasm`, the same thing in a
// browser canvas).  Arrow keys and the mouse at the window edge scroll; sprites
// animate in place; a click selects an entity and draws the ground ring.

#include <SDL.h>

#include <algorithm>
#include <memory>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "image.h"
#include "png.h"
#include "hudfont.h"
#include "stream.h"

#include "campaign.h"
#include "campaign_ui.h"
#include "ent.h"
#include "esh.h"
#include "scene.h"
#include "vfs.h"
#include "ui.h"
#include "launch.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

using namespace game;

namespace {

// Without --assets, the game's own `core` folder in the current directory:
// run fotgame from the Fallout Tactics folder and it finds the archives.
const char* const kDefaultAssets = "core";

struct App {
    // MILESTONE 12: the scene is re-created on every mission change, so it is
    // owned by a pointer rather than embedded.
    std::unique_ptr<Scene> sceneOwn;
    Scene* scenep = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int W = 1024, H = 768;
    std::vector<uint8_t> fb;

    int64_t camX = 0, camY = 0;
    double camFx = 0, camFy = 0;
    int64_t bounds[4] = {0, 0, 0, 0};

    bool keyLeft = false, keyRight = false, keyUp = false, keyDown = false;
    int mouseX = -1, mouseY = -1;
    bool mouseIn = false;
    bool running = true;
    bool headless = false;
    bool showFps = false;

    uint32_t lastTicks = 0;
    int64_t frameNo = 0;
    double fpsAccum = 0;
    int fpsFrames = 0;
    double fps = 0;
    bool bench = false;
    uint64_t renderTicks = 0;
    int64_t renderFrames = 0;
    RenderStats lastStats;

    // --frames N --screenshot path
    int64_t stopAfter = -1;
    std::string shot;
    std::vector<std::string> shotAt;   // "N:path"
    // Synthetic input for headless verification.
    std::vector<std::string> script;   // "N:click:x,y" / "N:scroll:dx,dy"

    // --- milestone 7 -----------------------------------------------------
    int64_t shotEvery = 0;              // --shot-every T:<prefix>, T in ticks
    std::string shotPrefix;
    int64_t lastShotTick = -1;
    int64_t traceEvery = 0;             // --trace-every N: movement dump every N frames
    bool follow = false;                // --follow: keep the selection centred
    bool drawPath = false;             // --path: overlay the computed route
    bool runByDefault = false;         // the run/walk toggle (shift, or --run)
    // MILESTONE 18: `game.runMode` and `game.formationMode` out of `bos.cfg`.
    //
    // `optionsGameHelp_runMode` says it outright -- "Sets the default move
    // behavior, which can be inverted by holding shift" -- and the three
    // values are `optionsGame_runMode_walk` / `_smart` / `_run`, with
    // `optionsGameHelp_runMode_smart` = "Run only when standing and not
    // sneaking".  The binary is `DAT_008bda18`, and the two arms are:
    //
    //   the click handler (`WGameWorld::ProcessCombatTargetingAndTriggers
    //   @0049c730`) packs shift into bit 0x800 and
    //   then `if (DAT_008bda18 == 2) mods ^= 0x800` -- ALWAYS RUN inverts the
    //   shift bit;
    //
    //   every order site (fourteen of them, e.g. @0049a7a8, @0049ca7e,
    //   @0049ac8c, @004d14fa) then does
    //       run = (mods >> 11) & 1;
    //       if (!(mods & 0x800) && DAT_008bda18 == 1
    //           && IsStanding && (!IsHidden || perks.silentrunning)) run = 1;
    //   -- SMART RUN, exactly the help string, including "and not sneaking"
    //   unless you have Silent Running.
    //
    // 0 = Always Walk, 1 = Smart Run, 2 = Always Run.  The shipped bos.cfg is
    // `{game.runMode} = {0}`.
    int32_t runMode = 0;
    int32_t formationMode = 0;         // `game.formationMode`, DAT_008bda1c
    std::vector<std::string> orders;   // "N:move:x,z" / "N:select:i" / "N:posture:P"

    // --- milestone 16: the band box ---------------------------------------
    // WMapWindow @0x0065a440 (WGameWorld vtable slot 51) records the press
    // point on button 1 and a "dragging" byte; the release decides whether it
    // was a click or a band.
    bool bandActive = false;
    int32_t bandX0 = 0, bandY0 = 0, bandX1 = 0, bandY1 = 0;

    // --- milestone 8 -----------------------------------------------------
    bool traceCombat = false;          // --trace-combat
    bool traceAi = false;              // --trace-ai (milestone 11)
    std::vector<std::string> hud;      // the last few combat messages, for the overlay
    bool showHud = false;              // --hud
    bool showLog = true;               // the console line
    int64_t logAfter = -1;             // print the combat log at the end

    // --- milestone 9: the interface --------------------------------------
    Ui ui;
    bool uiOn = true;                  // --no-ui turns it off
    std::string guiDir;                // --gui <dir>, defaults to --assets
    std::vector<std::string> uiScript; // "N:screen:inventory", "N:drag:x0,y0,x1,y1", ...

    // --- milestone 10: mission scripting ---------------------------------
    bool scriptLog = false;            // --script-log: echo the trigger log live
    size_t scriptLogSeen = 0;

    // --- milestone 13: sound ---------------------------------------------
    Audio audio;
    bool audioOn = true;               // --no-sound turns it off
    bool audioLog = false;             // --audio-log
    std::string audioDump;             // --audio-dump <p.wav>
    Volumes audioVolumes;              // --audio-volumes m,mu,sfx,sp
    bool haveAudioVolumes = false;

    // --- milestone 12: the campaign --------------------------------------
    bool campaignMode = false;
    CampaignState camp;
    std::unique_ptr<CampaignUi> camUi;
    CampaignScreen camScreen = CampaignScreen::None;
    CampaignScreen camReturn = CampaignScreen::WorldMap;
    std::string assetsDir;
    std::string saveDir;
    // MILESTONE 15: extra `.bos` archives named with --archive, and whether to
    // print one line per mount.
    std::vector<std::string> extraArchives;
    bool verboseResources = false;
    std::string resourceManifest;
    std::string spriteManifest;        // --sprite-manifest <f>
    std::vector<std::string> camScript;    // "N:verb[:args]"
    bool camDump = false;
    // MILESTONE 21: `--image-dump <rel>[:<out.png>]`, repeatable.
    std::vector<std::string> imageDump;
    // The mission the campaign is currently inside, and the world-map point to
    // come back to.
    std::string camMission;
    // MILESTONE 22: a `<world>` 70 image out of a `.sav`, waiting for the next
    // `CampaignEnterMission` to load the map under it.  Empty for a fresh
    // mission entry.
    std::vector<uint8_t> pendingSaveWorld;
    int32_t camMissionCore = -1;
    EncounterRoll camPending;
    int32_t camCountdown = 0;
    SceneOptions camOpt;
};

App g;

// ---------------------------------------------------------------------------
// The scroll model.  See NOTES.md "the viewport": the engine keeps a screen
// offset and WGameWorld::Update adds a per-frame delta to it while the mouse is
// inside an edge band or an arrow key is down.  The band width and the speed
// are not recoverable from the dumps; these are labelled placeholders.
// ---------------------------------------------------------------------------
constexpr int kEdgeBand = 8;             // PLACEHOLDER (px)
constexpr int kRingLift = 4;             // PLACEHOLDER (px above the sprite's foot)
constexpr double kScrollPxPerSec = 640;  // PLACEHOLDER

void ClampCamera() {
    const int64_t w = g.bounds[2] - g.bounds[0];
    const int64_t h = g.bounds[3] - g.bounds[1];
    const int64_t lo0 = g.bounds[0] - 64, hi0 = g.bounds[0] + std::max<int64_t>(0, w - g.W) + 64;
    const int64_t lo1 = g.bounds[1] - 64, hi1 = g.bounds[1] + std::max<int64_t>(0, h - g.H) + 64;
    if (g.camFx < lo0) g.camFx = lo0;
    if (g.camFx > hi0) g.camFx = hi0;
    if (g.camFy < lo1) g.camFy = lo1;
    if (g.camFy > hi1) g.camFy = hi1;
    // UpdateScreenOffsetFromWorldPosition @006e13d0
    // masks bit 0 off both components of the screen offset -- the camera can
    // only ever sit on an even pixel, which keeps the iso lattice in phase.
    g.camX = static_cast<int64_t>(std::lround(g.camFx)) & ~1LL;
    g.camY = static_cast<int64_t>(std::lround(g.camFy)) & ~1LL;
}

// ---------------------------------------------------------------------------
// The selection marker.  See NOTES.md "the selection marker": the game draws a
// ground ring under the selected actor.  No sprite for it survives in the data
// and no rasteriser for it is in the dumps, so this is a labelled
// reconstruction: an iso-projected ellipse on the entity's own ground cell,
// with the projection's own 2:1 axis ratio (screenX = 6*(x-z),
// screenY = -3*(x+z)).
//
// MILESTONE 22 (VIDEO_GAPS M5) replaces the invented size and colour with the
// capture's own.  On f0161 each of the three squad rings measures 32 x 15 px
// with a near-white warm core (240, 228, 212); on f0348 the hostile's ring
// measures 35 x 16 px in (200, 40, 40).  Both are the same ground ellipse the
// shipped `gui/cursor/Move.zar` (30 x 16) draws for the ground cursor, so the
// whole family is one 2:1 ellipse of the same radius -- the squad's drawn three
// pixels thick, a hostile's two, which is what the capture shows at 1:1.
// ---------------------------------------------------------------------------
constexpr double kRingRx = 16.0;   // MEASURED off f0161 / f0348 (32-35 px wide)
constexpr double kRingRy = 7.5;    // MEASURED (15-16 px high)

void DrawSelectionRing(const LiveEntity& e, uint8_t r, uint8_t gg, uint8_t b, int thickness) {
    // The centre is the bottom-centre of the entity's own drawn rect, not the
    // projection of its world cell: the two differ by ~13 px for a standing
    // figure, because the .spr anchor (Sprite::offsetY) is not the contact
    // point, and the ring has to sit where the sprite meets the ground.
    const int32_t cx = (e.rect[0] + e.rect[2]) / 2;
    const int32_t cy = e.rect[3] - static_cast<int32_t>(kRingLift);
    const int kSteps = 256;
    for (int i = 0; i < kSteps; ++i) {
        const double a = i * 2.0 * 3.14159265358979 / kSteps;
        for (int t = 0; t < thickness; ++t) {
            const double rr = kRingRx - (thickness - 1) * 0.5 + t;
            const double rrz = kRingRy - (thickness - 1) * 0.25 + t * 0.5;
            const int64_t px = static_cast<int64_t>(std::lround(cx + rr * std::cos(a))) - g.camX;
            const int64_t py = static_cast<int64_t>(std::lround(cy + rrz * std::sin(a))) - g.camY;
            if (px < 0 || py < 0 || px >= g.W || py >= g.H) {
                continue;
            }
            uint8_t* d = g.fb.data() + (static_cast<size_t>(py) * g.W + px) * 4;
            d[0] = r;
            d[1] = gg;
            d[2] = b;
            d[3] = 255;
        }
    }
}

// ParseInts is in engine/shared.h.

// --path: the computed route, drawn as a projected polyline with a diamond on
// every waypoint.  A debug overlay, not anything the game draws.
void DrawPixel(int64_t px, int64_t py, uint8_t r, uint8_t gg, uint8_t b) {
    if (px < 0 || py < 0 || px >= g.W || py >= g.H) {
        return;
    }
    uint8_t* d = g.fb.data() + (static_cast<size_t>(py) * g.W + px) * 4;
    d[0] = r;
    d[1] = gg;
    d[2] = b;
    d[3] = 255;
}

void DrawPath(int32_t index) {
    const std::vector<PathPoint>* p = g.scenep->PathOf(index);
    if (p == nullptr || p->empty()) {
        return;
    }
    auto project = [&](const PathPoint& q, int64_t* sx, int64_t* sy) {
        int32_t x = 0, y = 0;
        ProjectWorldToScreen(q.x, q.y, q.z, &x, &y);
        *sx = x - g.camX;
        *sy = y - g.camY;
    };
    for (size_t i = 0; i + 1 < p->size(); ++i) {
        int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        project((*p)[i], &x0, &y0);
        project((*p)[i + 1], &x1, &y1);
        const int64_t steps = std::max<int64_t>(std::abs(x1 - x0), std::abs(y1 - y0)) + 1;
        for (int64_t t = 0; t <= steps; ++t) {
            const int64_t px = x0 + (x1 - x0) * t / steps;
            const int64_t py = y0 + (y1 - y0) * t / steps;
            DrawPixel(px, py, 255, 230, 60);
            DrawPixel(px, py + 1, 255, 230, 60);
        }
    }
    for (const PathPoint& q : *p) {
        int64_t sx = 0, sy = 0;
        project(q, &sx, &sy);
        for (int d = -3; d <= 3; ++d) {
            const int w = 3 - std::abs(d);
            for (int k = -w; k <= w; ++k) {
                DrawPixel(sx + k * 2, sy + d, 255, 90, 90);
            }
        }
    }
}


bool gTrace = false;
bool gTraceMove = false;
// MILESTONE 14: render-side interpolation (on unless --no-interp), and the
// per-frame screen-position trace the smoothness check reads.
bool gInterp = true;
std::string gTraceScreen;
std::vector<int32_t> gTraceScreenIds;

// The headless driver for the interface: `--ui-screen N:<name>`,
// `--ui-click N:x,y`, `--ui-hover N:x,y` and `--ui-drag N:x0,y0,x1,y1`.
void RunUiScript() {
    for (const std::string& s : g.uiScript) {
        if (std::atoll(s.c_str()) != g.frameNo) continue;
        const size_t c1 = s.find(':');
        const size_t c2 = s.find(':', c1 + 1);
        const std::string what = s.substr(c1 + 1, c2 - c1 - 1);
        const std::string arg = c2 == std::string::npos ? std::string() : s.substr(c2 + 1);
        int64_t v[4] = {0, 0, 0, 0};
        if (what == "screen") {
            if (arg == "none") g.ui.SetScreen(UiScreen::None);
            else if (arg == "inventory") g.ui.SetScreen(UiScreen::Inventory);
            else if (arg == "character") g.ui.SetScreen(UiScreen::Character);
            else if (arg == "pipboy") g.ui.SetScreen(UiScreen::Pipboy);
            else if (arg == "skilldex") g.ui.SetScreen(UiScreen::Skilldex);
            else if (arg == "menu") g.ui.SetScreen(UiScreen::Menu);
            std::printf("frame %lld: ui screen -> %s\n", (long long)g.frameNo, arg.c_str());
        } else if (what == "action") {
            const bool ok = g.ui.Action(g.scenep, arg);
            std::printf("frame %lld: ui action %s: %s\n", (long long)g.frameNo, arg.c_str(),
                        ok ? "ok" : "unhandled");
        } else if (what == "hover" && ParseInts(arg.c_str(), 2, v)) {
            g.ui.MouseMove(static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]));
        } else if (what == "click" && ParseInts(arg.c_str(), 2, v)) {
            g.ui.MouseMove(static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]));
            g.ui.MouseDown(g.scenep, static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]), false);
            g.ui.MouseUp(g.scenep, static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]), false);
            std::printf("frame %lld: ui click (%lld,%lld)\n", (long long)g.frameNo,
                        (long long)v[0], (long long)v[1]);
        } else if (what == "press" && ParseInts(arg.c_str(), 2, v)) {
            g.ui.MouseMove(static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]));
            g.ui.MouseDown(g.scenep, static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]), false);
        } else if (what == "release" && ParseInts(arg.c_str(), 2, v)) {
            g.ui.MouseMove(static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]));
            g.ui.MouseUp(g.scenep, static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]), false);
        } else if (what == "drag" && ParseInts(arg.c_str(), 4, v)) {
            g.ui.SyntheticDrag(g.scenep, static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]),
                               static_cast<int32_t>(v[2]), static_cast<int32_t>(v[3]));
            std::printf("frame %lld: ui drag (%lld,%lld) -> (%lld,%lld)\n", (long long)g.frameNo,
                        (long long)v[0], (long long)v[1], (long long)v[2], (long long)v[3]);
        }
    }
}

void SaveShot(const std::string& path) {
    if (g.uiOn && g.ui.Ready()) {
        std::printf("%s", g.ui.TraceLayout().c_str());
    }
    if (gTrace) {
        std::printf("--- frame %lld ---\n%s", (long long)g.frameNo,
                    g.scenep->TraceAnimation().c_str());
    }
    if (gTraceMove) {
        std::printf("--- move, frame %lld (tick %lld) ---\n%s", (long long)g.frameNo,
                    (long long)g.scenep->Ticks(), g.scenep->TraceMovement().c_str());
    }
    if (g.traceAi) {
        std::printf("--- ai, frame %lld (tick %lld) ---\n%s", (long long)g.frameNo,
                    (long long)g.scenep->Ticks(), g.scenep->TraceAI().c_str());
    }
    std::string err;
    if (!ft::WritePng(path, g.W, g.H, g.fb, &err)) {
        std::fprintf(stderr, "screenshot %s: %s\n", path.c_str(), err.c_str());
    } else {
        std::printf("wrote %s (%dx%d) at frame %lld\n", path.c_str(), g.W, g.H,
                    (long long)g.frameNo);
    }
}

// ===========================================================================
// MILESTONE 16: the mouse, as the shipped game documents it.
//
// `locale/gui/help.txt` is the game's OWN key/mouse reference, shipped
// uncompressed in `locale_0.bos`, and it settles every question milestone 6-15
// left open by reconstruction:
//
//   MOVEMENT   Left Click on Ground: Move
//              Ctrl-Left Click: Force Move to same level.
//              Shift-Left Click: Run
//              W/Backspace: Stop
//   ACTIONS    Left Click on Object: Use
//   COMBAT     Left Click on Target: Fire Weapon
//              Right Click: Fire        Ctrl-Right Click: Force Fire
//   SQUAD      F1-F6: Select Squad Member       F12: Select All Squad Members
//              F7-F11 / Ctrl-F7-F11: recall / set selection groupings
//
// So LEFT click is the universal "do the thing under the cursor" button and
// there is no right-click move order at all -- which is exactly the binding
// milestones 6-8 got backwards, and why the player could not move anybody.
//
// Selecting a squad member is NOT a world click in that list: it is F1-F6 or
// the game bar's own tab, which goes through `WSquadMember::HandleClickAtPoint
// @004d16b0` -> `HandleEntityClickForSkillOrSelect @004d13a0` ->
// `WGameWorld::ToggleEntitySelection @0049fb40`.  A world click on one of your
// own members reaches the same `HandleEntityClickForSkillOrSelect`, whose
// default arm is that same ToggleEntitySelection, so clicking a member in the
// world selects him too; `<shift>F1..F6` are keys.cfg's `modSelect1..6`, the
// add/remove form (ToggleEntitySelection with exclusive == 0), which is what
// shift-click does here.
//
// Band selection: `WGameWorld` vtable slot 51 (@0049d4e0) forwards to a
// WMapWindow entry at 0x0065a440 that, on button 1, sets a "dragging" byte at
// +0x1b5 and stores the press point at +0x1b6/+0x1ba -- the band box's origin.
// `WGameWorld::GetSelectionCursorIcon @0065b300` hardcodes 15 px as the
// click-versus-drag limit, which is the threshold
// used here; `DAT_00826664`, the same limit inside `WGameWorld::Update`, is
// not resolved in the dumps.
//
// `WGameWorld::GetSelectionCursorIcon @0065b300`
// hardcodes 15 px as the click-versus-drag limit.
constexpr int32_t kBandThreshold = 15;

// The band box.  Every one of the local player's live members whose drawn
// screen rect meets the box joins the selection.
void BandSelect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool add) {
    if (g.scenep == nullptr) return;
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const std::vector<LiveEntity>& ents = g.scenep->Entities();
    const int32_t mine = g.scenep->SpawnPlayer();
    std::vector<int32_t> picked;
    if (add) picked = g.scenep->Selection();
    for (size_t i = 0; i < ents.size(); ++i) {
        const LiveEntity& e = ents[i];
        if (!e.isCombatant || e.dead || e.playerIndex != mine) continue;
        const int32_t rx0 = static_cast<int32_t>(e.rect[0] - g.camX);
        const int32_t ry0 = static_cast<int32_t>(e.rect[1] - g.camY);
        const int32_t rx1 = static_cast<int32_t>(e.rect[2] - g.camX);
        const int32_t ry1 = static_cast<int32_t>(e.rect[3] - g.camY);
        if (rx1 < x0 || rx0 > x1 || ry1 < y0 || ry0 > y1) continue;
        bool have = false;
        for (int32_t p : picked) have |= p == static_cast<int32_t>(i);
        if (!have) picked.push_back(static_cast<int32_t>(i));
    }
    if (!picked.empty()) g.scenep->SelectMany(picked);
    std::printf("band (%d,%d)-(%d,%d) -> %zu selected\n", x0, y0, x1, y1, picked.size());
}

// MILESTONE 18.  The run bit, recovered whole; see `G::runMode` above.
//
//   `shiftBit`  is the 0x800 the click handler packs the shift key into, AFTER
//               `if (runMode == 2) mods ^= 0x800`.
//   the SMART arm then runs at every order site.
//
// `Actor::IsStanding` is `posture == "Stand"` here, `Entity::IsHidden` is the
// sneak mode, and `perks.silentrunning` is the perk that lets you do both --
// none of the shipped Brahmin Wood actors has it, so it is read and always 0.
bool RunFlagFor(const game::LiveEntity& e, bool shift) {
    bool shiftBit = shift;
    if (g.runMode == 2) shiftBit = !shiftBit;      // Always Run inverts shift
    bool run = shiftBit;
    if (!shiftBit && g.runMode == 1) {             // Smart Run
        if (e.posture == "Stand" && (!e.sneaking || e.attr.silentRunning != 0)) run = true;
    }
    return run;
}

// Returns true when the click was consumed.
bool WorldClick(int32_t x, int32_t y, bool right, bool shift, bool ctrl) {
    if (g.scenep == nullptr) return false;
    const int32_t hit = g.scenep->Pick(g.camX, g.camY, x, y);
    const std::vector<LiveEntity>& ents = g.scenep->Entities();
    const int32_t mine = g.scenep->SpawnPlayer();
    const bool hitOurs = hit >= 0 && ents[static_cast<size_t>(hit)].isCombatant &&
                         !ents[static_cast<size_t>(hit)].dead &&
                         ents[static_cast<size_t>(hit)].playerIndex == mine;

    // MILESTONE 18.  A right click cancels an armed skill cursor -- that is
    // `FUN_0049e120`, "clear `0x1d3` and `0x1d5` and refresh the cursor",
    // which every cancelled interaction in `ProcessCombatTargetingAndTriggers`
    // ends at.
    if (right && !g.ui.SkillCursor().empty()) {
        std::printf("click (%d,%d) -> %s cancelled\n", x, y, g.ui.SkillCursor().c_str());
        g.ui.SetSkillCursor(std::string());
        return true;
    }

    // MILESTONE 18.  The SKILL cursor arm, and it comes FIRST.
    // `WGameWorld::ProcessCombatTargetingAndTriggers @0049c730` tests
    // `field_0x1d3` (skill mode) before every other arm and, when an entity is
    // under the cursor, hands it to `WGameWorld::ResolveStealSkillTarget
    // @0049a650` -- which picks the selected member with the HIGHEST value of
    // that skill -- then calls `FUN_004a2c20` (the cursor refresh) and CLEARS
    // `0x1d3`.  So the skill cursor is one-shot.
    if (!right && !g.ui.SkillCursor().empty()) {
        const std::string skill = g.ui.SkillCursor();
        g.ui.SetSkillCursor(std::string());
        if (hit < 0) {
            std::printf("click (%d,%d) -> %s cancelled\n", x, y, skill.c_str());
            return true;
        }
        const int32_t who = g.scenep->BestSkilledSelected(skill);
        std::string why;
        const bool ok = who >= 0 && g.scenep->UseSkill(who, skill, hit, &why);
        std::printf("click (%d,%d) -> %s on %s: %s\n", x, y, skill.c_str(),
                    ents[static_cast<size_t>(hit)].ShownName().c_str(),
                    ok ? why.c_str() : (why.empty() ? "refused" : why.c_str()));
        if (g.uiOn && g.ui.Ready() && !why.empty()) g.ui.PushMessage(why);
        return true;
    }

    if (!right && hitOurs) {
        // HandleEntityClickForSkillOrSelect's default arm.
        if (shift) g.scenep->SelectToggle(hit);
        else g.scenep->Select(hit);
        std::printf("click (%d,%d) -> select %s%s\n", x, y,
                    ents[static_cast<size_t>(hit)].ShownName().c_str(),
                    shift ? " (add)" : "");
        return true;
    }

    // MILESTONE 18.  TALK.  `WGameWorld::ProcessCombatTargetingAndTriggers
    // @0049c730`, the `field_0x1d6` branch: after the "it is one of mine ->
    // select" arm and BEFORE the collectable and attack arms, it calls
    // `IsEntityValidTalkTarget @004a3070` and, when that is true,
    // `WGameWorld::LogClosestPlayerToPosition @0049aaf0`, which walks the
    // selection, picks the member CLOSEST to the target and posts order 0x17
    // (`LogRecordFieldChange17 @006315b0`) with the run bit.
    //
    // So a left click on a friendly NPC that has a `Click Speech` node TALKS
    // TO IT -- it is not an attack and not a "use".  That is the whole rule,
    // and it is why the villagers and captives of Brahmin Wood, every one of
    // which carries a Click Speech string in the `.mis`, answer a click.
    if (!right && g.scenep->TalkTarget(hit)) {
        const int32_t who = g.scenep->NearestSelected(hit);
        std::string why;
        const bool ok = who >= 0 && g.scenep->OrderTalk(who, hit, &why);
        std::printf("click (%d,%d) -> talk to %s: %s\n", x, y,
                    ents[static_cast<size_t>(hit)].ShownName().c_str(),
                    ok ? "speaking" : why.c_str());
        return true;
    }

    // "Left Click on Target: Fire Weapon" / "Right Click: Fire".
    if (hit >= 0 && ents[static_cast<size_t>(hit)].isCombatant &&
        !ents[static_cast<size_t>(hit)].dead && !hitOurs) {
        std::string why;
        bool any = false;
        for (int32_t s : g.scenep->Selection()) {
            any |= g.scenep->OrderAttack(s, hit, &why);
        }
        std::printf("click (%d,%d) -> attack %s: %s\n", x, y,
                    ents[static_cast<size_t>(hit)].ShownName().c_str(),
                    any ? "firing" : why.c_str());
        return true;
    }
    if (right) {
        // Right click with nothing to fire at does nothing in FT.
        return false;
    }

    // MILESTONE 17.  `locale/gui/help.txt`, the game's own reference:
    // "ACTIONS  Left Click on Object: Use".  The object is a container, a
    // corpse, an item on the ground or a door, and the handler is
    // `HandleActivateOrLootCommand @00549120` (`Scene::UseEntity`).  The actor
    // is the selected squad member; when it is out of reach the original walks
    // to it first (`use_code |= 0x100` then `vtable+0x368`), which is a move
    // order to the object's cell here.
    if (hit >= 0) {
        const LiveEntity& obj = ents[static_cast<size_t>(hit)];
        const bool usable = obj.isContainer || obj.isCollectable || obj.isDoor ||
                            (obj.isCombatant && obj.dead);
        if (usable) {
            int32_t actor = g.scenep->Selected();
            if (actor < 0 && !g.scenep->Selection().empty()) actor = g.scenep->Selection().front();
            std::string why;
            bool opened = false;
            if (actor >= 0 && g.scenep->UseEntity(actor, hit, &why, &opened)) {
                if (opened && g.uiOn && g.ui.Ready()) g.ui.SetScreen(game::UiScreen::Loot);
                std::printf("click (%d,%d) -> use %s%s\n", x, y, obj.label.c_str(),
                            opened ? " (loot)" : "");
                return true;
            }
            // Out of range: walk to it, as the original's `use_code |= 0x100`
            // arm does before retrying.
            if (actor >= 0 && why == "Out of range.") {
                g.scenep->OrderMove(actor, obj.world[0], obj.world[2], shift);
                std::printf("click (%d,%d) -> %s is out of reach: walking to it\n", x, y,
                            obj.label.c_str());
                return true;
            }
            std::printf("click (%d,%d) -> use %s: %s\n", x, y, obj.label.c_str(), why.c_str());
            return true;
        }
    }

    // "Left Click on Ground: Move", "Shift-Left Click: Run".
    int32_t wx = 0, wz = 0;
    const bool onGround = g.scenep->ScreenToGround(g.camX, g.camY, x, y, &wx, &wz);
    if (!onGround) {
        std::printf("click (%d,%d) -> no walkable ground\n", x, y);
        return false;
    }
    // MILESTONE 18: the run bit is now per member, because Smart Run depends
    // on the member's own posture and sneak state.  `--run` keeps forcing it.
    const bool forceRun = shift || g.runByDefault;
    // The whole selection walks.  `WGameWorld::ComputePlayerGroupCentroid
    // @0049b770` "computes the centroid of visible player entities and offsets
    // each player's target position relative to it" -- the formation spread --
    // but its body is not in the dumps, so the offsets here are a RING around
    // the clicked cell, the same eight-slot ring `Scene::ResolveEntities` uses
    // for the spawn formation.  Labelled placeholder.
    static const int kRing[8][2] = {{0, 0}, {2, -2}, {-2, 2}, {2, 2},
                                    {-2, -2}, {4, 0}, {-4, 0}, {0, 4}};
    int moved = 0, k = 0;
    bool anyRun = false;
    for (int32_t s : g.scenep->Selection()) {
        const int slot = k % 8;
        const bool run = RunFlagFor(ents[static_cast<size_t>(s)], forceRun);
        anyRun |= run;
        if (g.scenep->OrderMove(s, wx + kRing[slot][0], wz + kRing[slot][1], run)) ++moved;
        ++k;
    }
    const bool run = anyRun;
    (void)ctrl;   // "Ctrl-Left Click: Force Move to same level" -- this port's
                  // nav grid is single-level, so the modifier is a no-op.
    std::printf("click (%d,%d) -> %s to world (%d,%d): %d of %zu moving\n", x, y,
                run ? "run" : "walk", wx, wz, moved, g.scenep->Selection().size());
    return moved > 0;
}

// The move-order half of the script, so a whole walk can be driven headlessly.
void RunOrders() {
    for (const std::string& s : g.orders) {
        if (std::atoll(s.c_str()) != g.frameNo) continue;
        const size_t c1 = s.find(':');
        const size_t c2 = s.find(':', c1 + 1);
        const std::string what = s.substr(c1 + 1, c2 - c1 - 1);
        const std::string arg = s.substr(c2 + 1);
        int64_t v[2] = {0, 0};
        // MILESTONE 12: the vehicle verbs.
        if (what == "mount" || what == "dismount" || what == "drive" || what == "fire") {
            int64_t a[3] = {0, 0, 0};
            std::string why;
            bool ok = false;
            if (what == "mount" && ft::ParseInts(arg.c_str(), 2, a)) {
                ok = g.scenep->VehicleMount(static_cast<int32_t>(a[0]),
                                            static_cast<int32_t>(a[1]), &why);
            } else if (what == "dismount") {
                ok = g.scenep->VehicleDismount(static_cast<int32_t>(std::atoll(arg.c_str())), &why);
            } else if (what == "drive" && ft::ParseInts(arg.c_str(), 3, a)) {
                ok = g.scenep->VehicleDrive(static_cast<int32_t>(a[0]), static_cast<int32_t>(a[1]),
                                            static_cast<int32_t>(a[2]), &why);
            } else if (what == "fire" && ft::ParseInts(arg.c_str(), 2, a)) {
                ok = g.scenep->VehicleFire(static_cast<int32_t>(a[0]), static_cast<int32_t>(a[1]),
                                           &why);
            }
            std::printf("frame %lld: vehicle %s %s: %s\n", (long long)g.frameNo, what.c_str(),
                        arg.c_str(), ok ? "ok" : why.c_str());
            continue;
        }
        if (what == "sentry") {
            // "i,off|scout|on" -- the game bar's sentry column, SentryAI+0x1ad.
            const size_t comma = arg.find(',');
            const int32_t idx = static_cast<int32_t>(std::atoll(arg.c_str()));
            const std::string m = comma == std::string::npos ? std::string() : arg.substr(comma + 1);
            const game::SentryMode mode = m == "off"  ? game::kSentryOff
                                          : m == "on" ? game::kSentryOn
                                                      : game::kSentryScout;
            g.scenep->SetSentry(idx, mode);
            std::printf("frame %lld: entity %d sentry -> %s\n", (long long)g.frameNo, idx,
                        m.c_str());
        } else if (what == "posture-at") {
            const size_t comma = arg.find(',');
            const int32_t idx = static_cast<int32_t>(std::atoll(arg.c_str()));
            const std::string pn =
                comma == std::string::npos ? std::string("Stand") : arg.substr(comma + 1);
            const bool ok = g.scenep->SetPosture(idx, pn);
            std::printf("frame %lld: entity %d posture -> %s (%s)\n", (long long)g.frameNo, idx,
                        pn.c_str(), ok ? "ok" : "refused");
        } else if (what == "use") {
            // MILESTONE 17.  `locale/gui/help.txt`: "Left Click on Object: Use".
            // `HandleActivateOrLootCommand @00549120` with the selection as the
            // actor and entity `i` as the target.
            const int32_t i = g.scenep->Selected();
            const int32_t target = static_cast<int32_t>(std::atoll(arg.c_str()));
            std::string why;
            bool opened = false;
            const bool ok = g.scenep->UseEntity(i, target, &why, &opened);
            if (ok && opened && g.uiOn && g.ui.Ready()) g.ui.SetScreen(game::UiScreen::Loot);
            std::printf("frame %lld: use %d -> %d: %s%s\n", (long long)g.frameNo, i, target,
                        ok ? "ok" : why.c_str(), opened ? " (loot window open)" : "");
        } else if (what == "give") {
            // "<entity>,<ent path>[,count]" -- MILESTONE 18's test hook, the
            // same `Scene::GiveItemByPath` the quartermaster and `--issue` use.
            const size_t c1b = arg.find(',');
            const size_t c2b = arg.find(',', c1b + 1);
            const int32_t idx = static_cast<int32_t>(std::atoll(arg.c_str()));
            const std::string path = arg.substr(c1b + 1, c2b == std::string::npos
                                                             ? std::string::npos
                                                             : c2b - c1b - 1);
            const int32_t n = c2b == std::string::npos
                                  ? 1
                                  : static_cast<int32_t>(std::atoll(arg.c_str() + c2b + 1));
            std::string why;
            const bool ok = g.scenep->GiveItemByPath(idx, path, -1, n, &why);
            std::printf("frame %lld: give %d %s x%d: %s\n", (long long)g.frameNo, idx,
                        path.c_str(), n, ok ? "ok" : why.c_str());
        } else if (what == "hurt") {
            // "<entity>,<hp>" -- set an actor's hit points, so the healing
            // skills have something to heal.
            const size_t c1b = arg.find(',');
            const int32_t idx = static_cast<int32_t>(std::atoll(arg.c_str()));
            const int32_t hp = c1b == std::string::npos
                                   ? 1
                                   : static_cast<int32_t>(std::atoll(arg.c_str() + c1b + 1));
            const bool ok = g.scenep->SetHitPoints(idx, hp);
            std::printf("frame %lld: hurt %d -> %d hp: %s\n", (long long)g.frameNo, idx, hp,
                        ok ? "ok" : "no such entity");
        } else if (what == "cursor") {
            // MILESTONE 18's verification hook: put the pointer over entity
            // `i`'s drawn rect (or a screen point "x,y") and report which
            // cursor `WGameWorld::GetSelectionStatusMessage @004a2c40`'s rule
            // picks.  `--follow`-free: the camera is wherever it is, and the
            // entity's rect is the one the renderer just used.
            int32_t mx = 0, my = 0;
            if (arg.find(',') != std::string::npos) {
                int64_t v2[2] = {0, 0};
                ft::ParseInts(arg.c_str(), 2, v2);
                mx = static_cast<int32_t>(v2[0]);
                my = static_cast<int32_t>(v2[1]);
            } else {
                const int32_t i = static_cast<int32_t>(std::atoll(arg.c_str()));
                const std::vector<game::LiveEntity>& es = g.scenep->Entities();
                if (i < 0 || static_cast<size_t>(i) >= es.size()) continue;
                mx = static_cast<int32_t>((es[static_cast<size_t>(i)].rect[0] +
                                           es[static_cast<size_t>(i)].rect[2]) / 2 - g.camX);
                my = static_cast<int32_t>((es[static_cast<size_t>(i)].rect[1] +
                                           es[static_cast<size_t>(i)].rect[3]) / 2 - g.camY);
            }
            g.ui.MouseMove(mx, my);
            g.ui.SetCamera(g.camX, g.camY);
            std::printf("frame %lld: cursor at (%d,%d) over '%s' -> %s\n", (long long)g.frameNo,
                        mx, my, arg.c_str(), g.ui.CursorAt(g.scenep, mx, my).c_str());
        } else if (what == "talk") {
            // MILESTONE 18.  `IsEntityValidTalkTarget @004a3070` +
            // `WGameWorld::LogClosestPlayerToPosition @0049aaf0`: the SELECTED
            // member nearest the target does the talking.
            const int32_t target = static_cast<int32_t>(std::atoll(arg.c_str()));
            const int32_t who = g.scenep->NearestSelected(target);
            std::string why;
            const bool ok = who >= 0 && g.scenep->OrderTalk(who, target, &why);
            std::printf("frame %lld: talk %d -> %d: %s\n", (long long)g.frameNo, who, target,
                        ok ? "speaking" : why.c_str());
        } else if (what == "skill") {
            // "<name>[,i]" -- `WGameWorld::EvaluateSneakSkillCondition
            // @0049de10` then `ResolveStealSkillTarget @0049a650`: the member
            // with the HIGHEST value of that skill performs it.
            const size_t comma = arg.find(',');
            const std::string skill = arg.substr(0, comma);
            const int32_t target = comma == std::string::npos
                                       ? -1
                                       : static_cast<int32_t>(std::atoll(arg.c_str() + comma + 1));
            const int32_t who = g.scenep->BestSkilledSelected(skill);
            std::string why;
            const bool ok = who >= 0 && g.scenep->UseSkill(who, skill, target, &why);
            std::printf("frame %lld: skill %s by %d on %d: %s%s\n", (long long)g.frameNo,
                        skill.c_str(), who, target, ok ? "ok " : "refused ", why.c_str());
            if (g.uiOn && g.ui.Ready() && !why.empty()) g.ui.PushMessage(why);
        } else if (what == "loot-take") {
            // One row of the open loot window's left list into the actor's own.
            const int32_t idx = static_cast<int32_t>(std::atoll(arg.c_str()));
            std::string why;
            const bool ok = g.scenep->TransferItem(g.scenep->LootTarget(), g.scenep->LootActor(),
                                                   idx, 0, &why);
            std::printf("frame %lld: loot-take %d: %s\n", (long long)g.frameNo, idx,
                        ok ? "ok" : why.c_str());
        } else if (what == "loot-put") {
            // The other direction, which is the same drag with the two sides
            // swapped.
            const int32_t idx = static_cast<int32_t>(std::atoll(arg.c_str()));
            std::string why;
            const bool ok = g.scenep->TransferItem(g.scenep->LootActor(), g.scenep->LootTarget(),
                                                   idx, 0, &why);
            std::printf("frame %lld: loot-put %d: %s\n", (long long)g.frameNo, idx,
                        ok ? "ok" : why.c_str());
        } else if (what == "loot-all") {
            std::string why;
            const int32_t n = g.scenep->TakeAll(g.scenep->LootTarget(), g.scenep->LootActor(),
                                                &why);
            std::printf("frame %lld: take all: %d items%s%s\n", (long long)g.frameNo, n,
                        why.empty() ? "" : ", stopped: ", why.c_str());
        } else if (what == "loot-close") {
            g.scenep->CloseLoot();
            if (g.uiOn && g.ui.Ready() && g.ui.Screen() == game::UiScreen::Loot) {
                g.ui.SetScreen(game::UiScreen::None);
            }
            std::printf("frame %lld: loot window closed\n", (long long)g.frameNo);
        } else if (what == "select") {
            g.scenep->Select(static_cast<int32_t>(std::atoll(arg.c_str())));
            const int32_t i = g.scenep->Selected();
            std::printf("frame %lld: select %d%s\n", (long long)g.frameNo, i,
                        i >= 0 ? (" " + g.scenep->Entities()[i].label).c_str() : "");
        } else if ((what == "move" || what == "run") && ParseInts(arg.c_str(), 2, v)) {
            const int32_t i = g.scenep->Selected();
            const bool ok = g.scenep->OrderMove(i, static_cast<int32_t>(v[0]),
                                              static_cast<int32_t>(v[1]), what == "run");
            int32_t gs = 0;
            bool gb = false;
            const bool inGrid = g.scenep->CellState(static_cast<int32_t>(v[0]),
                                                  static_cast<int32_t>(v[1]), &gs, &gb);
            std::printf("frame %lld: %s %d -> world (%lld,%lld) [goal cell %s surface %d %s]: %s",
                        (long long)g.frameNo, what.c_str(), i, (long long)v[0], (long long)v[1],
                        inGrid ? "in grid" : "OFF GRID", gs, gb ? "BLOCKED" : "free",
                        ok ? (g.scenep->LastPathExact() ? "exact path " : "PARTIAL path ") : "NO PATH");
            std::printf("(%zu nodes expanded) ", g.scenep->LastPathExpanded());
            if (ok) {
                const std::vector<PathPoint>* p = g.scenep->PathOf(i);
                std::printf("%zu waypoints:", p->size());
                for (const PathPoint& q : *p) std::printf(" (%d,%d,%d)", q.x, q.y, q.z);
            }
            std::printf("\n");
        } else if (what == "attack") {
            const int32_t i = g.scenep->Selected();
            const int32_t target = static_cast<int32_t>(std::atoll(arg.c_str()));
            game::Scene::HitTerms terms;
            g.scenep->ComputeHitChance(i, target, &terms);
            std::string why;
            const bool ok = g.scenep->OrderAttack(i, target, &why);
            std::printf("frame %lld: attack %d -> %d (%s): chance %d%% "
                        "[skill %d acc %d rangeClass %d rangePen %d close %d ac %d dodge %d "
                        "cover %d down %d big %d dist %.2f]%s%s\n",
                        (long long)g.frameNo, i, target,
                        (target >= 0 && target < (int32_t)g.scenep->Entities().size())
                            ? g.scenep->Entities()[target].label.c_str() : "?",
                        terms.total, terms.skill, terms.accuracyBonus, terms.rangeClass,
                        terms.rangePenalty, terms.closeBonus, terms.armourClass, terms.meleeDodge,
                        terms.coverPenalty, terms.targetDown, terms.bigTarget, terms.distance,
                        ok ? "" : "  REFUSED: ", ok ? "" : why.c_str());
        } else if (what == "place") {
            int64_t w[3] = {0, 0, 0};
            if (ParseInts(arg.c_str(), 3, w)) {
                const bool ok = g.scenep->PlaceEntity(static_cast<int32_t>(w[0]),
                                                    static_cast<int32_t>(w[1]),
                                                    static_cast<int32_t>(w[2]));
                std::printf("frame %lld: place %lld at world (%lld,%lld): %s\n",
                            (long long)g.frameNo, (long long)w[0], (long long)w[1],
                            (long long)w[2], ok ? "ok" : "refused");
            }
        } else if (what == "hittest") {
            int64_t w[2] = {0, 0};
            if (ParseInts(arg.c_str(), 2, w)) {
                const int32_t i = g.scenep->Selected();
                game::Scene::HitTerms t;
                int64_t hits = 0, shots = 0;
                const bool ok = g.scenep->HitRateTest(i, static_cast<int32_t>(w[0]),
                                                    static_cast<int32_t>(w[1]), &t, &hits, &shots);
                std::printf("HITTEST attacker=%d target=%lld n=%lld dist=%.2fm chance=%d%% "
                            "shots=%lld hits=%lld observed=%.2f%% "
                            "[skill %d acc %d rc %d rangePen %d close %d ac %d dodge %d cover %d "
                            "down %d big %d] %s\n",
                            i, (long long)w[0], (long long)w[1], t.distance, t.total,
                            (long long)shots, (long long)hits,
                            shots > 0 ? 100.0 * (double)hits / (double)shots : 0.0, t.skill,
                            t.accuracyBonus, t.rangeClass, t.rangePenalty, t.closeBonus,
                            t.armourClass, t.meleeDodge, t.coverPenalty, t.targetDown, t.bigTarget,
                            ok ? "" : ("REFUSED: " + t.reason).c_str());
            }
        } else if (what == "mode") {
            const int32_t i = g.scenep->Selected();
            const bool ok = g.scenep->SetWeaponMode(i, arg);
            std::printf("frame %lld: weapon mode %d -> %s: %s\n", (long long)g.frameNo, i,
                        arg.c_str(), ok ? "ok" : "refused");
        } else if (what == "turnbased") {
            g.scenep->SetTurnBased(arg != "off" && arg != "0");
        } else if (what == "endturn") {
            g.scenep->EndTurn();
            const int32_t a = g.scenep->ActiveTurnEntity();
            std::printf("frame %lld: end turn -> now %d%s\n", (long long)g.frameNo, a,
                        a >= 0 ? (" " + g.scenep->Entities()[a].label).c_str() : "");
        } else if (what == "seed") {
            g.scenep->SeedRng(static_cast<uint32_t>(std::atoll(arg.c_str())));
        } else if (what == "kill") {
            const int32_t i = g.scenep->FindByTag(arg);
            const bool ok = g.scenep->ScriptKill(i);
            std::printf("frame %lld: kill '%s' (entity %d): %s\n", (long long)g.frameNo,
                        arg.c_str(), i, ok ? "dead" : "refused");
        } else if (what == "speech") {
            const size_t eq = arg.find('=');
            const std::string tag = arg.substr(0, eq);
            const std::string node = eq == std::string::npos ? "" : arg.substr(eq + 1);
            const int32_t i = g.scenep->FindByTag(tag);
            g.scenep->Script().PlaySpeechNode(*g.scenep, i, node);
            std::printf("frame %lld: speech '%s' by '%s' (entity %d)\n", (long long)g.frameNo,
                        node.c_str(), tag.c_str(), i);
        } else if (what == "setvar") {
            const size_t eq = arg.find('=');
            if (eq != std::string::npos) {
                g.scenep->Script().SetVar(arg.substr(0, eq), arg.substr(eq + 1));
                std::printf("frame %lld: %s = %s\n", (long long)g.frameNo,
                            arg.substr(0, eq).c_str(), arg.substr(eq + 1).c_str());
            }
        } else if (what == "teleport") {
            // TEST AFFORDANCE, like --place: put a tagged entity on the ground
            // at a world (x, z) so a zone or exit-grid condition can be driven
            // without walking the whole map.
            const size_t c = arg.find(',');
            const std::string tag = arg.substr(0, c);
            int64_t w[2] = {0, 0};
            const int32_t i = g.scenep->FindByTag(tag);
            bool ok = false;
            if (c != std::string::npos && ParseInts(arg.c_str() + c + 1, 2, w)) {
                ok = g.scenep->PlaceEntity(i, static_cast<int32_t>(w[0]), static_cast<int32_t>(w[1]));
            }
            std::printf("frame %lld: teleport '%s' (entity %d) to (%lld,%lld): %s\n",
                        (long long)g.frameNo, tag.c_str(), i, (long long)w[0], (long long)w[1],
                        ok ? "ok" : "refused");
        } else if (what == "posture") {
            const int32_t i = g.scenep->Selected();
            const bool ok = g.scenep->SetPosture(i, arg);
            std::printf("frame %lld: posture %d -> %s: %s\n", (long long)g.frameNo, i,
                        arg.c_str(), ok ? "ok" : "refused");
        }
    }
}

void RunScript() {
    for (const std::string& s : g.script) {
        const size_t c1 = s.find(':');
        if (c1 == std::string::npos) continue;
        if (std::atoll(s.c_str()) != g.frameNo) continue;
        const size_t c2 = s.find(':', c1 + 1);
        const std::string what = s.substr(c1 + 1, c2 - c1 - 1);
        const std::string arg = c2 == std::string::npos ? "" : s.substr(c2 + 1);
        int64_t v[2] = {0, 0};
        int64_t b[4] = {0, 0, 0, 0};
        // MILESTONE 16: `--click N:x,y[,shift][,ctrl][,right]` goes down the
        // same `WorldClick` the SDL button event does -- the harness and the
        // player press the same code now.
        if (what == "click") {
            std::string coords = arg;
            const bool shift = coords.find("shift") != std::string::npos;
            const bool ctrl = coords.find("ctrl") != std::string::npos;
            const bool right = coords.find("right") != std::string::npos;
            const size_t cut = coords.find_first_not_of("0123456789-,");
            if (cut != std::string::npos) coords = coords.substr(0, cut);
            while (!coords.empty() && coords.back() == ',') coords.pop_back();
            if (ParseInts(coords.c_str(), 2, v)) {
                std::printf("frame %lld: ", (long long)g.frameNo);
                if (g.uiOn && g.ui.Ready() &&
                    g.ui.MouseDown(g.scenep, static_cast<int32_t>(v[0]),
                                   static_cast<int32_t>(v[1]), right)) {
                    g.ui.MouseUp(g.scenep, static_cast<int32_t>(v[0]),
                                 static_cast<int32_t>(v[1]), right);
                    std::printf("interface took click (%lld,%lld)\n", (long long)v[0],
                                (long long)v[1]);
                } else {
                    WorldClick(static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]), right,
                               shift, ctrl);
                }
            }
        } else if (what == "band" && ParseInts(arg.c_str(), 4, b)) {
            BandSelect(static_cast<int32_t>(b[0]), static_cast<int32_t>(b[1]),
                       static_cast<int32_t>(b[2]), static_cast<int32_t>(b[3]), false);
        } else if (what == "scroll" && ParseInts(arg.c_str(), 2, v)) {
            g.camFx += v[0];
            g.camFy += v[1];
            ClampCamera();
            std::printf("frame %lld: scroll by (%lld,%lld) -> cam (%lld,%lld)\n",
                        (long long)g.frameNo, (long long)v[0], (long long)v[1],
                        (long long)g.camX, (long long)g.camY);
        }
    }
}

// ===========================================================================
// MILESTONE 12: the campaign layer.
//
// The mission the campaign is playing is still the same `Scene` milestones
// 6-11 built; what is new is the shell around it.  Entering a mission is
// `WCampaignWindow::EnterMission @006b6580`, which
// writes the mission path into `Campaign+0x10` and a mode into `Campaign+0x14`
// (1 core, 2 random/special, 3 morphed special, 4 vehicle); leaving is
// `ExitMissionToWorldMap @006a2410`.  Between the two, the squad's state has
// to survive, and `Campaign::SaveMissionToCampaign @006a0020` is how the
// original does it -- it writes the whole world out to the campaign scratch
// directory through `World::Save` and reads it back if you return.  What this
// port carries instead is the ROSTER (see campaign.h): the squad's HP, XP,
// level, equipped slots and pack, which is the part of that blob a player
// notices, and the part whose fields are all recoverable.
// ===========================================================================

std::string CampaignAssetPath(const std::string& rel) {
    const std::string root = g.assetsDir.empty() ? std::string(kDefaultAssets) : g.assetsDir;
    return root + "/" + rel;
}

// MILESTONE 15: push the save directory out to IndexedDB.
//
// The browser lane mounts IDBFS over `--save-dir`, and an IDBFS write only
// reaches IndexedDB when something calls `FS.syncfs`. fallout1-ce and
// fallout2-ce do that themselves after every save, which is why the shell can
// simply wrap `FS.syncfs` and treat it as a persistence hook
// (shell/src/engine.ts). This port did not, so a campaign saved in the browser
// was written to MEMFS, looked saved, and vanished on reload.
//
// Called after every successful save; a no-op natively.
void PersistSaves() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof FS !== 'undefined' && FS.syncfs) {
            FS.syncfs(false, function (err) {
                if (err) console.error('[fotgame] syncfs failed', err);
            });
        }
    });
#endif
}

void RefreshSaveSlots() {
    g.camUi->m_slots.clear();
    // The browser lists the save directory and reads each file's `<saveh>`
    // block from offset 0 without loading it
    // (`WSaveGameItem::EnsureSaveGameHeaderLoaded @004bf560`).  The original's
    // directory is `user\save\` (`s_user_save__0089a2c4`, `WGameMain::PerformQuickSave@00478340`
    // l.23082); this port takes it from `--save-dir`.
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(g.saveDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.size() > 4 && n.substr(n.size() - 4) == ".sav") names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        SaveSlot s;
        s.file = n;
        s.path = g.saveDir + "/" + n;
        std::string e;
        CampaignState::ReadHeaderOnly(s.path, &s.header, &e);
        g.camUi->m_slots.push_back(s);
    }
    if (g.camUi->m_slotSel >= static_cast<int>(g.camUi->m_slots.size())) g.camUi->m_slotSel = 0;
}

// Copy the roster's carried state onto the entities the map just spawned.
void ApplyRosterToScene() {
    const std::vector<std::string> deployed = g.camp.DeployedPaths();
    std::vector<LiveEntity>& ents = g.scenep->EntitiesMut();
    const std::vector<int32_t>& squad = g.scenep->Squad();
    for (size_t i = 0; i < squad.size() && i < deployed.size(); ++i) {
        LiveEntity& e = ents[static_cast<size_t>(squad[i])];
        for (RosterMember& m : g.camp.roster) {
            if (m.entPath != deployed[i] || !m.inSquad) continue;
            if (m.maxHp > 0) {
                e.maxHp = m.maxHp;
                e.hp = std::max(1, m.hp);
            } else {
                m.maxHp = e.maxHp;
                m.hp = e.hp;
            }
            e.xp = m.xp;
            e.level = m.level;
            if (m.displayName.empty()) m.displayName = e.displayName;
            if (m.rank.empty()) m.rank = e.rank;
            // The quartermaster's issue, and anything carried out of the last
            // mission.
            std::string why;
            if (!m.hand1.empty()) g.scenep->GiveItemByPath(squad[i], m.hand1, 0, 1, &why);
            if (!m.hand2.empty()) g.scenep->GiveItemByPath(squad[i], m.hand2, 1, 1, &why);
            if (!m.armour.empty()) g.scenep->GiveItemByPath(squad[i], m.armour, 2, 1, &why);
            for (const RosterItem& it : m.inventory) {
                g.scenep->GiveItemByPath(squad[i], it.entPath, -1, it.count, &why);
            }
            break;
        }
    }
}

// The other direction, at the debrief.
void HarvestRosterFromScene() {
    const std::vector<std::string> deployed = g.camp.DeployedPaths();
    const std::vector<LiveEntity>& ents = g.scenep->Entities();
    const std::vector<int32_t>& squad = g.scenep->Squad();
    // `ActionAddExperience` credits the player, not a unit, so the mission's
    // total is shared out evenly -- which is what the original's own debrief
    // screen shows as a per-member figure.
    const int32_t total = g.scenep->Script().Experience();
    const int32_t share = deployed.empty() ? 0 : total / static_cast<int32_t>(deployed.size());
    for (size_t i = 0; i < squad.size() && i < deployed.size(); ++i) {
        const LiveEntity& e = ents[static_cast<size_t>(squad[i])];
        for (RosterMember& m : g.camp.roster) {
            if (m.entPath != deployed[i] || !m.inSquad) continue;
            m.hp = e.dead ? 0 : e.hp;
            m.maxHp = e.maxHp;
            m.dead = e.dead;
            m.xp = e.xp + share;
            m.displayName = e.displayName;
            m.rank = e.rank;
            m.hand1 = e.weapon.valid ? e.weapon.path : std::string();
            m.hand2 = e.weapon2.valid ? e.weapon2.path : std::string();
            m.armour = e.armour.path;
            m.ammo = e.ammo;
            m.inventory.clear();
            for (const LiveEntity::InvItem& it : e.inventory) {
                RosterItem r;
                r.cls = it.cls;
                r.displayName = it.displayName;
                r.itemType = it.itemType;
                r.entPath = it.entPath;
                r.spritePath = it.spritePath;
                r.count = it.count;
                m.inventory.push_back(r);
            }
            // `Level` is not a serialized entity property anywhere in the
            // dumps; the campaign's own level curve is `Campaign+0x18`
            // (10 in the shipped file).  1,000 XP a level is this port's.
            m.level = std::max(1, 1 + m.xp / 1000);
            break;
        }
    }
    g.camp.reputation += g.scenep->Script().Reputation();
    for (const auto& kv : g.scenep->Script().CVars()) {
        g.camp.SetVar(kv.first, kv.second);
        // campaign.txt's recruit and quartermaster rows are gated by a mission
        // tag; a completed mission opens its own tag and its `a` variant.
    }
}

bool CampaignEnterMission(const std::string& misPath, int32_t coreIndex,
                          const std::vector<std::string>& extraSpawn);

void CampaignLeaveMission();

void ApplyMissionTags(const std::string& misPath) {
    // `{ mission01 ; add ; ... }`: the tag is the mission's own base name.
    std::string base = misPath;
    const size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    g.camp.ApplyTag(base);
    g.camp.ApplyTag(base + "a");
}

// The bunker quartermaster.  `WQuarterMaster` has no body in the dumps -- only
// `ActionQuarterMaster::Serialize @00689860` (a `Tagname` and three flags) and
// `WBarterTable::PrepareBarterTransactionData @004558a0` on the trade side --
// so what the screen looks like is not recoverable.  What IS recoverable is the
// STOCK: campaign.txt's `quarterMaster = { tag ; count ; path }` rows, unlocked
// by mission tag, which is the list below.  Issuing is "arm whoever is unarmed
// from what is in stock", which is this port's policy.
// The stock take, shared by the bunker screen and by `--issue`.
std::string QmTake(std::map<std::string, int32_t>* stock, const std::string& want, int32_t n) {
    for (auto& kv : *stock) {
        if (kv.second < n) continue;
        if (kv.first.find(want) == std::string::npos) continue;
        kv.second -= n;
        return kv.first;
    }
    return std::string();
}

// The FIRST ISSUE, in one place so the bunker and `--issue` cannot drift: a
// weapon in hand one (rifle, then SMG, then pistol, then a blade), a set of
// armour, and fifty rounds.  The order is the stock's own -- there is no
// `WQuarterMaster` in any dump at all, so this is a POLICY, and it is the one
// the shipped stock makes obvious: the 24 rows unlocked at `mission01` are
// exactly a starting kit for six.
struct FirstIssue {
    std::string hand1, armour, ammo;
    int32_t ammoCount = 0;
};
FirstIssue PlanFirstIssue(std::map<std::string, int32_t>* stock, bool wantWeapon,
                          bool wantArmour, bool wantAmmo) {
    FirstIssue out;
    if (wantWeapon) {
        for (const char* d : {"weapons/Rifles/", "weapons/SMG/", "weapons/Pistols/",
                              "weapons/Blades/"}) {
            out.hand1 = QmTake(stock, d, 1);
            if (!out.hand1.empty()) break;
        }
    }
    if (wantArmour) out.armour = QmTake(stock, "items/armor/", 1);
    if (wantAmmo) {
        out.ammo = QmTake(stock, "items/ammo/", 50);
        if (!out.ammo.empty()) out.ammoCount = 50;
    }
    return out;
}

void CampaignQuartermaster() {
    auto take = [&](const std::string& want, int32_t n) -> std::string {
        return QmTake(&g.camp.stock, want, n);
    };
    int issued = 0;
    for (RosterMember& m : g.camp.roster) {
        if (!m.inSquad || m.dead) continue;
        if (m.hand1.empty()) {
            std::string w = take("weapons/Rifles/", 1);
            if (w.empty()) w = take("weapons/SMG/", 1);
            if (w.empty()) w = take("weapons/Pistols/", 1);
            if (w.empty()) w = take("weapons/Blades/", 1);
            if (!w.empty()) {
                m.hand1 = w;
                ++issued;
            }
        }
        if (m.armour.empty()) {
            const std::string a = take("items/armor/", 1);
            if (!a.empty()) m.armour = a;
        }
        bool hasAmmo = false;
        for (const RosterItem& it : m.inventory) hasAmmo |= it.entPath.find("ammo") != std::string::npos;
        if (!hasAmmo) {
            const std::string am = take("items/ammo/", 50);
            if (!am.empty()) {
                RosterItem it;
                it.entPath = am;
                it.cls = "Ammo";
                it.count = 50;
                m.inventory.push_back(it);
            }
        }
    }
    g.camUi->SetStatus("quartermaster: issued equipment to " + std::to_string(issued) +
                       " unarmed squad members");
    std::printf("campaign: quartermaster issued to %d members\n", issued);
}

// ===========================================================================
// MILESTONE 20: the front end.
//
// Three screens the capture has and this port did not: the SINGLE PLAYER menu,
// CHARACTER SELECTION and the Pip-Boy BRIEFING.  The drawing is in
// campaign_ui.cpp, which names every source; what lives here is the DATA each
// of them shows and what each control does.
// ===========================================================================

// The pre-made characters.  `campaigns/bos.cam` carries a `prefabs` vector
// (Campaign+0x1bd) of five `.ent` paths; each one is read with the same
// `EntProtoCache` the scene uses, its `Attributes` block through the same
// `ReadAttributes`, and its `Display Name` is the key into
// `locale/game/characters.txt` for both the name and the biography.
void LoadPrefabs() {
    g.camUi->m_chars.clear();
    ft::PathIndex index(g.camOpt.entDir, "entities/", ".ent");
    ft::EntProtoCache cache(&index);
    for (const std::string& path : g.camp.file.prefabs) {
        PrefabChar pc;
        pc.entPath = path;
        std::string err;
        const ft::EntProto* proto = cache.Get(path, &err);
        if (proto == nullptr) {
            std::fprintf(stderr, "prefab %s: %s\n", path.c_str(), err.c_str());
            g.camUi->m_chars.push_back(pc);
            continue;
        }
        const ft::Esh& esh = proto->block.esh;
        esh.GetString("Display Name", &pc.displayName);
        ft::Esh block;
        if (esh.GetNested("Attributes", &block, &err)) ReadAttributes(block, &pc.attr, false);
        // MILESTONE 20 applied `Attributes::SumFinalAttributes @006088d0`'s two
        // visible rows here, and only here.  MILESTONE 21 moved the whole
        // derivation into `Attributes::Derive` (combat.cpp), where every live
        // combatant goes through it too, so this screen and the game now read
        // the same numbers off the same code.  Mick's Armor Class 8, Melee
        // Damage 3, Action Points 9 and Hit Points 37 on f0036 are unchanged.
        pc.attr.Derive();
        pc.maxHp = pc.attr.maxHitPoints;
        pc.hp = pc.maxHp;
        ft::Esh cur;
        if (esh.GetNested("Current Attributes", &cur, &err)) {
            int32_t hp = 0;
            if (cur.GetInt("hitPoints", &hp) && hp > 0) pc.hp = std::min(hp, pc.maxHp);
        }
        const std::string key = pc.displayName;
        pc.name = g.ui.Ready() ? g.ui.Strings().Get("name_" + key) : key;
        if (pc.name == "name_" + key) pc.name = key;
        pc.story = g.ui.Ready() ? g.ui.Strings().Get("desc_" + key) : std::string();
        if (pc.story == "desc_" + key) pc.story.clear();
        pc.ok = true;
        g.camUi->m_chars.push_back(pc);
    }
    std::printf("campaign: %zu pre-made characters\n", g.camUi->m_chars.size());
}

// TUTORIALS.  The four shipped tutorial maps are
// `campaigns/missions/tutorials/tut1.mis`, `tut2a`, `tut2b` and `tut2c`, and
// the four shipped captions are `demoMenu_tut1`, `_tut2a`, `_tut2b`, `_tut2c`
// (BASIC / AMBUSH / STEALTH / SNIPER) -- the stems and the key suffixes are the
// same four tokens, which is what pins the pairing, and BASIC/AMBUSH/STEALTH/
// SNIPER is the capture's own order at f0359.
void BuildTutorialList() {
    static const char* const kStems[4] = {"tut1", "tut2a", "tut2b", "tut2c"};
    g.camUi->m_list.clear();
    g.camUi->m_listTitle = g.ui.Strings().Get("demoMenu_tutorials");
    for (int32_t i = 0; i < 4; ++i) {
        const std::string rel =
            std::string("campaigns/missions/tutorials/") + kStems[i] + ".mis";
        std::string handle;
        if (!ft::VfsResolve(rel, &handle)) continue;
        g.camUi->m_list.emplace_back(g.ui.Strings().Get(std::string("demoMenu_") + kStems[i]),
                                     rel);
    }
    if (g.camUi->m_list.empty()) g.camUi->SetStatus("no tutorial maps in this install");
}

// CUSTOM CAMPAIGN / CUSTOM MISSION: the shipped `campaigns/*.cam` and the
// shipped `campaigns/missions/**.mis`, as the two pickers list them.  The
// mounted namespace is the authority (`ft::VfsEnumerate`), so a mod's own maps
// appear here too.
void BuildCustomList(bool campaigns) {
    g.camUi->m_list.clear();
    g.camUi->m_listTitle = g.ui.Strings().Get(campaigns ? "singleMenu_customCampaign"
                                                        : "singleMenu_customMission");
    std::vector<std::string> found;
    ft::VfsEnumerate("campaigns/", campaigns ? ".cam" : ".mis",
                     [&](const std::string& key) { found.push_back(key); });
    std::sort(found.begin(), found.end());
    for (const std::string& rel : found) {
        std::string caption = rel;
        const size_t sl = caption.find_last_of('/');
        if (sl != std::string::npos) caption = caption.substr(sl + 1);
        const size_t dot = caption.find_last_of('.');
        if (dot != std::string::npos) caption = caption.substr(0, dot);
        for (char& ch : caption) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        g.camUi->m_list.emplace_back(caption, rel);
        if (g.camUi->m_list.size() >= 10) break;
    }
    std::printf("campaign: %s picker, %zu of %zu entries\n",
                campaigns ? "custom campaign" : "custom mission", g.camUi->m_list.size(),
                found.size());
}

// The briefing.  The body is the mission's OWN locale table under
// `MISSION_BRIEF` -- every shipped `locale/missions/<mission>/*_Speech.txt`
// defines it, and the capture's Barnaky text is `mission01`'s verbatim.
bool OpenBriefing(const std::string& misPath, int32_t core) {
    BriefingData b;
    b.debrief = false;
    b.misPath = misPath;
    b.core = core;
    std::string base = misPath;
    const size_t sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    b.missionKey = base;
    if (g.ui.Ready()) {
        g.ui.LoadMissionStrings(misPath);
        b.brief = g.ui.Strings().Get("MISSION_BRIEF");
        if (b.brief == "MISSION_BRIEF") b.brief.clear();
        // The MAP tab: the mission's own `M##_MiniMap_*` annotations, which is
        // the only map text the shipped locale carries.
        int32_t num = 0;
        for (char ch : base) {
            if (ch >= '0' && ch <= '9') num = num * 10 + (ch - '0');
        }
        for (int32_t i = 1; i <= 20; ++i) {
            char key[64];
            std::snprintf(key, sizeof(key), "M%02d_MiniMap_%02d", num, i);
            if (g.ui.Strings().Has(key)) b.map.push_back(g.ui.Strings().Get(key));
        }
    }
    if (b.brief.empty()) {
        b.brief = "No briefing ships for " + base + ".";
    }
    b.title = base;
    g.camUi->m_brief = b;
    g.camUi->m_pipTab = 0;
    g.camUi->m_briefScroll = 0;
    g.camScreen = CampaignScreen::Briefing;
    g.camUi->SetStatus("");
    std::printf("campaign: briefing for %s (%zu chars)\n", misPath.c_str(), b.brief.size());
    return true;
}

void CampaignBoot(const std::string& assetsDir) {
    std::string err;
    if (!g.camp.New(assetsDir, &err)) {
        std::fprintf(stderr, "campaign: %s\n", err.c_str());
        g.running = false;
        return;
    }
    std::printf("campaign: %s\n", g.camp.file.info.c_str());
    g.camp.rng = 1;
    LoadPrefabs();
    if (g.uiOn && g.ui.Ready()) g.ui.SetCampaignShell(true);
    // MILESTONE 20: the executable's own first screen is `WMainMenu`
    // (@006bff50 / @006c00f0), whose SINGLE PLAYER entry builds the submenu the
    // capture opens on.
    g.camScreen = CampaignScreen::TitleMenu;
    g.camUi->SetStatus("");
}

void CampaignNewGame() {
    std::string err;
    g.camp.New(g.assetsDir, &err);
    // `cores = { "...mission01.mis;...;visited;false" }` -- mission01 is the
    // only location the campaign starts with, and bunker01 is where its own
    // `Set Mission State` sends you (mission01.mis's trigger sets
    // `mission_name_B01` to Visited).
    // MILESTONE 16: arm the starting squad.
    //
    // Every shipped recruit `.ent` -- `entities/recruits/hm01.ent` and its
    // fifty-odd siblings -- carries an Armour block and an EMPTY `Inventory`
    // block, and no hand slot at all, so a recruit spawned straight out of its
    // prototype has nothing in either hand and the game bar's two `WIIHand`
    // panels are legitimately blank.  In the shipped game the bunker's
    // quartermaster is what fills them, off `tables/campaign.txt`'s
    // `quarterMaster = { tag ; count ; path }` stock, and a new campaign is
    // expected to start with its squad kitted.  So a New Game issues from that
    // stock before the first mission instead of walking into Brahmin Wood
    // unarmed.
    CampaignQuartermaster();
    g.camp.SetMissionState("mission_name_01", MissionState::UnCovered);
    g.camp.RevealFog(g.camp.posX, g.camp.posY);
    g.camScreen = CampaignScreen::WorldMap;
    g.camUi->SetStatus("New campaign. Travel to a location and click it to enter.");
    std::printf("campaign: new game, squad %zu, pool %zu, stock %zu\n",
                g.camp.DeployedPaths().size(), g.camp.pool.size(), g.camp.stock.size());
}

// ---------------------------------------------------------------------------
// MILESTONE 16: everything a freshly loaded mission owes the player.
//
// Two things that were missing and that made a squad spawned by `--squad` or
// by the campaign look like it had not been constructed at all:
//
//  1. NAMES.  `GetLocalizedEntityName @0064f5d0` builds `"name_" + <Display
//     Name>` and looks it up in the game's string tables, then in the
//     mission's own, and falls back to the raw Display Name.  A recruit's
//     Display Name IS the key -- `hm01.ent` carries "CORE_rec_hm01" and
//     `locale/game/characters.txt` has `name_CORE_rec_hm01 = {Robin}` -- so
//     without the lookup every tab reads "CORE_rec_h" (the raw key, trimmed to
//     the 80 px tab).  The lookup is the UI's, because the UI owns the
//     `Locale`; the answer is pushed onto the entity so the game bar, the
//     character sheet, the inventory and the tooltips all agree.
//
//  2. SELECTION.  Nothing was selected at mission start, and the game bar's
//     portrait, hands, action points, character sheet, skilldex and inventory
//     are all "the selected member's", so all five were blank.  The original
//     always has one: `WGameWorld::SetSpeechTargetFromPlayerListOrDefault
//     @0049f600` walks the local player's entity list for a flagged member and
//     "otherwise falls back to the first list entity and selects it".  That is
//     what this does -- squad member 1, in roster order.
// ---------------------------------------------------------------------------
// MILESTONE 17: `--issue`.
// ---------------------------------------------------------------------------
// The player's report was "there's attack but I can't attack back, no weapons
// in inventory", playing `fotgame <mission>.mis --campaign-start --squad ...`.
// That path is right: every shipped recruit `.ent` carries an Armour block, an
// EMPTY `Inventory` and NO hand slot, so a squad spawned straight out of its
// prototypes is unarmed.  Under `--campaign` the bunker's quartermaster fills
// them in before the first mission (section 57); a mission started directly had
// nothing equivalent.
//
// So `--issue` runs the SAME `PlanFirstIssue` against a fresh campaign's stock
// -- `tables/campaign.txt`'s 232 `quarterMaster = { tag ; count ; path }` rows
// with the `mission00` and `mission01` tags applied, which is exactly what the
// bunker has in stock at the start of a new game -- and puts the weapon in
// HAND ONE, so the game bar's left `WIIHand` panel shows it.  It is on by
// default with `--campaign-start` and off with `--no-issue`.
void IssueStartingEquipment() {
    if (g.scenep == nullptr) return;
    const std::vector<int32_t>& squad = g.scenep->Squad();
    if (squad.empty()) return;
    CampaignState fresh;
    std::string err;
    if (!fresh.New(g.assetsDir.empty() ? std::string(kDefaultAssets) : g.assetsDir, &err)) {
        std::fprintf(stderr, "issue: %s (squad stays unarmed)\n", err.c_str());
        return;
    }
    int issued = 0;
    for (int32_t idx : squad) {
        if (idx < 0 || static_cast<size_t>(idx) >= g.scenep->Entities().size()) continue;
        const LiveEntity& e = g.scenep->Entities()[static_cast<size_t>(idx)];
        // An unarmed hand holds the race's punch, which is not a real weapon
        // and does not count as armed.
        const bool needWeapon = !e.weapon.valid || e.unarmedHand[0];
        bool haveAmmo = false;
        for (const LiveEntity::InvItem& it : e.inventory) {
            haveAmmo |= it.cls == "Ammo";
        }
        const FirstIssue plan =
            PlanFirstIssue(&fresh.stock, needWeapon, !e.armour.valid, !haveAmmo);
        std::string why;
        if (!plan.hand1.empty() && g.scenep->GiveItemByPath(idx, plan.hand1, 0, 1, &why)) {
            ++issued;
        }
        if (!plan.armour.empty()) g.scenep->GiveItemByPath(idx, plan.armour, 2, 1, &why);
        if (!plan.ammo.empty()) {
            g.scenep->GiveItemByPath(idx, plan.ammo, -1, plan.ammoCount, &why);
        }
        std::printf("issue: %s <- %s%s%s%s\n", e.ShownName().c_str(),
                    plan.hand1.empty() ? "(nothing)" : plan.hand1.c_str(),
                    plan.armour.empty() ? "" : ", ", plan.armour.c_str(),
                    plan.ammo.empty() ? "" : (", " + plan.ammo).c_str());
    }
    // The magazine and the spare-round count both come off the pack, exactly as
    // a mission load does: `Weapon::FindSuitableAmmoInInventory @005dae40`
    // matches on `Ammo Type`.
    g.scenep->RefreshAmmoReserves();
    std::printf("issue: armed %d of %zu squad members from the quartermaster's stock\n", issued,
                squad.size());
}

void MissionStarted() {
    if (g.scenep == nullptr) return;
    if (g.uiOn && g.ui.Ready()) {
        const Locale& loc = g.ui.Strings();
        std::vector<LiveEntity>& ents = g.scenep->EntitiesMut();
        for (LiveEntity& e : ents) {
            if (e.displayName.empty()) continue;
            const std::string key = "name_" + e.displayName;
            if (loc.Has(key)) e.name = loc.Get(key);
            else if (e.tag.empty()) e.name = e.displayName;
        }
    }
    if (g.scenep->Selected() < 0) {
        const std::vector<int32_t>& sq = g.scenep->Squad();
        if (!sq.empty()) {
            g.scenep->Select(sq.front());
        } else {
            // No `--squad`: the map's own actors in the spawn point's player
            // slot are the squad (mapview/NOTES.md 24.5).
            const std::vector<LiveEntity>& ents = g.scenep->Entities();
            int32_t sp[3] = {0, 0, 0};
            (void)sp;
            for (size_t i = 0; i < ents.size(); ++i) {
                if (ents[i].isCombatant && !ents[i].dead && ents[i].playerIndex >= 0 &&
                    ents[i].playerIndex == g.scenep->SpawnPlayer()) {
                    g.scenep->Select(static_cast<int32_t>(i));
                    break;
                }
            }
        }
    }
}

// MILESTONE 21: the wait screen, painted while the map loads.
//
// `gui/wait/wait%d.jpg` is the exe's own format string and the archive ships
// wait1..wait7, so the picture is one of seven per load.  WHICH one is not
// recovered -- nothing in the dumps shows the draw -- so this port hashes the
// mission path, which keeps a run reproducible and still varies by map.
//
// The load itself is synchronous here, as it is in the original (the wait
// screen exists precisely because it blocks), so the page is painted and
// presented directly rather than through a frame of the main loop: going
// through the loop would shift every frame number the verify scripts key on.
void PresentFrame(int64_t deltaMs);

void PaintLoadingScreen(const std::string& misPath, float progress) {
    if (g.camUi == nullptr || !g.uiOn || !g.ui.Ready() || g.fb.empty()) return;
    uint32_t h = 2166136261u;
    for (char ch : misPath) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    char name[32];
    std::snprintf(name, sizeof(name), "gui/wait/wait%d.jpg", 1 + static_cast<int>(h % 7));
    g.camUi->SetLoading(name, progress);
    const CampaignScreen was = g.camScreen;
    g.camScreen = CampaignScreen::Loading;
    Canvas c;
    c.fb = g.fb.data();
    c.w = g.W;
    c.h = g.H;
    c.SetClip(0, 0, g.W, g.H);
    g.camUi->DrawLoading(&c, g.W, g.H);
    PresentFrame(0);
    g.camScreen = was;
}

bool CampaignEnterMission(const std::string& misPath, int32_t coreIndex,
                          const std::vector<std::string>& extraSpawn) {
    SceneOptions opt = g.camOpt;
    // A restored mission brings its own entities -- the squad among them, in
    // the positions the save recorded -- so the map's SpawnPoint must NOT be
    // used and the roster must NOT be re-applied.
    const bool restoring = !g.pendingSaveWorld.empty();
    std::vector<uint8_t> saveWorld;
    saveWorld.swap(g.pendingSaveWorld);
    if (restoring) {
        opt.saveWorld = &saveWorld;
    } else {
        opt.squad = g.camp.DeployedPaths();
        opt.campaignStart = true;
    }
    const std::string full = misPath.rfind("campaigns/", 0) == 0 || misPath.rfind('/', 0) == 0
                                 ? CampaignAssetPath(misPath)
                                 : misPath;
    PaintLoadingScreen(misPath, 0.1f);
    std::unique_ptr<Scene> next(new Scene());
    next->SetAudio(g.audio.Enabled() ? &g.audio : nullptr);
    std::string err;
    if (!next->Load(full, opt, &err)) {
        std::fprintf(stderr, "enter mission %s: %s\n", full.c_str(), err.c_str());
        g.camUi->SetStatus("cannot load " + misPath + ": " + err);
        return false;
    }
    g.sceneOwn = std::move(next);
    g.scenep = g.sceneOwn.get();
    g.scenep->SeedRng(g.camp.rng ? g.camp.rng : 1);
    if (!restoring) ApplyRosterToScene();
    (void)extraSpawn;   // the encounter force is spawned by the map's own script
    g.camMission = misPath;
    g.camMissionCore = coreIndex;
    g.camp.currentMission = misPath;
    g.camScreen = CampaignScreen::None;
    g.scenep->ScreenBounds(g.bounds);
    int32_t sp[3] = {0, 0, 0};
    if (g.scenep->SpawnPoint(sp)) {
        int32_t cx = 0, cy = 0;
        ProjectWorldToScreen(sp[0], sp[1], sp[2], &cx, &cy);
        g.camFx = cx - g.W / 2.0;
        g.camFy = cy - g.H / 2.0;
    }
    ClampCamera();
    if (g.uiOn && g.ui.Ready()) {
        // The previous mission's debrief window belongs to the previous
        // mission; `WGameMain` tears its screens down with the world.
        g.ui.SetScreen(UiScreen::None);
        g.ui.LoadMissionStrings(full);
    }
    PaintLoadingScreen(misPath, 1.0f);
    ApplyMissionTags(misPath);
    MissionStarted();
    std::printf("campaign: entered %s (squad %zu)\n", misPath.c_str(), opt.squad.size());
    // MILESTONE 15: the scene stats, which until now only the command-line
    // mission path printed. In the browser stdout is the only diagnostic there
    // is -- there is no terminal to re-run this in -- so a mission entered from
    // the campaign has to say what it loaded too.
    std::printf("campaign: scene %s\n", g.scenep->Info().c_str());
    return true;
}

// The Pip-Boy debrief page.  `lines` is what the mission script collected.
void OpenDebrief(const BriefingData* data) {
    if (data == nullptr || data->brief.empty()) return;
    g.camReturn = g.camScreen;
    g.camUi->m_brief = *data;
    g.camUi->m_pipTab = 0;
    g.camUi->m_briefScroll = 0;
    g.camScreen = CampaignScreen::Briefing;
    std::printf("campaign: debrief page (%zu chars)\n", data->brief.size());
}

void CampaignLeaveMission() {
    if (g.scenep == nullptr) return;
    const MissionEnd end = g.scenep->Script().Ended();
    HarvestRosterFromScene();
    // Collected while the scene is still alive.
    BriefingData debrief;
    {
        const MissionScript& sc = g.scenep->Script();
        debrief.debrief = true;
        debrief.missionKey = g.camMission;
        std::string body;
        for (const std::pair<int32_t, std::string>& kv : sc.DebriefLines()) {
            const std::string t = g.ui.Ready() ? g.ui.Strings().Get(kv.second) : kv.second;
            if (!body.empty()) body += "\\n\\n";
            body += t;
        }
        debrief.brief = body;
        for (const Objective& o : sc.Objectives()) {
            debrief.archives.push_back(std::string(o.complete ? "[X] " : "[ ] ") +
                                       (g.ui.Ready() ? g.ui.Strings().Get(o.key) : o.key));
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "EXPERIENCE %d    REPUTATION %+d", sc.Experience(),
                      sc.Reputation());
        debrief.dialogue.push_back(buf);
    }
    // The mission's own elapsed time joins the campaign clock.  `World.gametime`
    // (World+0x2fb) advances 70 ms per tick (NOTES.md 2) and `Campaign+0x48` is
    // the campaign's int64 clock; nothing in the dumps carries one into the
    // other, so this addition is this port's.
    g.camp.timeMs += g.scenep->Ticks() * 70;
    if (g.camMissionCore >= 0 &&
        g.camMissionCore < static_cast<int32_t>(g.camp.cores.size())) {
        g.camp.cores[g.camMissionCore].state = MissionState::Visited;
        // The squad comes back out where it went in.
        g.camp.posX = g.camp.cores[g.camMissionCore].locX;
        g.camp.posY = g.camp.cores[g.camMissionCore].locY;
    }
    g.camp.destX = g.camp.posX;
    g.camp.destY = g.camp.posY;
    g.camp.travelling = false;
    g.camp.currentMission.clear();
    const bool bunker = g.camMissionCore >= 0 && g.camp.cores[g.camMissionCore].isBunker;
    g.camMission.clear();
    g.camReturn = bunker ? CampaignScreen::Recruit : CampaignScreen::WorldMap;
    g.camScreen = g.camReturn;
    g.camUi->SetStatus(bunker ? "Bunker. Pick your squad, then take the world map."
                              : "Back on the world map.");
    // MILESTONE 20: the debrief is the SAME Pip-Boy page as the briefing, with
    // the text `ActionAddDebriefText::Vtable15 @00687c30` collected and
    // `ActionDisplayDebrief::Vtable15 @00687f50` sorted, and `genericGui_done`
    // in place of `pipboy_startMission`.  It replaces milestone 10's own box.
    OpenDebrief(&debrief);
    // MILESTONE 21: `ActionEndMission`'s `Exit to` argument names one of three
    // destinations and script.cpp already parses all three; two of them are
    // pictures, which until the JPEG decoder landed could not be drawn.
    // f0357 is `gui/back/DEATHscreen.JPG` measured (campaign_ui.cpp).  The
    // FailScreen pair is picked by the player character's sex in the original
    // -- that is what the two filenames say -- and this port has no sex on a
    // character, so it takes the male plate.
    if (end == MissionEnd::DeathScreen || end == MissionEnd::FailScreen) {
        g.camUi->SetEndScreen(end == MissionEnd::DeathScreen ? "gui/back/DEATHscreen.JPG"
                                                             : "gui/back/FailScreenMale.JPG");
        g.camScreen = CampaignScreen::EndScreen;
        std::printf("campaign: end screen %s\n", g.camUi->EndImage().c_str());
    }
    std::printf("campaign: left mission, reputation %d\n", g.camp.reputation);
}

// `ActionSetMissionState`'s effect, drained out of the trigger engine's log
// each frame: the script layer records the request and the campaign applies it.
void DrainMissionStateRequests() {
    for (const auto& r : g.scenep->Script().MissionStateRequests()) {
        g.camp.SetMissionState(r.first, static_cast<MissionState>(r.second));
    }
    g.scenep->Script().ClearMissionStateRequests();
}



// ---------------------------------------------------------------------------
// The mid-mission snapshot.  The original writes the whole world here through
// `Campaign::SaveMissionToCampaign @006a0020` -> `World::Save @004ea450`, whose
// section order NOTES.md records; what it cannot give is the per-entity payload,
// because `ResolveEntityRefAndFlagDirty` -- the callee `World::
// SaveEntityRecordsWithProgress @004e5860` hands each entity to -- has no body
// in any dump.  So the snapshot below carries the fields a resumed mission
// visibly needs, under this port's own tag, and NOTES.md says so.
// ---------------------------------------------------------------------------
struct SnapWriter {
    std::vector<uint8_t> b;
    void U8(uint8_t v) { b.push_back(v); }
    void I32(int32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void F32(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        I32(static_cast<int32_t>(u));
    }
    void Str(const std::string& s) {
        I32(static_cast<int32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};

// MILESTONE 24: everything the shipped save's `<saveh>` block carries that
// only this layer can resolve -- the player character's name, the mission's
// DISPLAY name, the squad's portraits and their hit points.  The player's test
// of a save written without them showed an absolute Mac path in the real
// game's Load Game list where the shipped save shows
// "Mick / Brahmin Wood / Jan 1 2197.  07:45", and no faces.
void PrepareSaveHeaderFields() {
    // +0x09.  `LoadPrefabs` already resolves `name_<Display Name>` out of
    // `locale/game/characters.txt`; `CORE_prefab1` is "Mick".
    g.camp.playerName.clear();
    if (g.camUi != nullptr) {
        if (const PrefabChar* pc = g.camUi->SelectedChar()) {
            if (!pc->name.empty()) g.camp.playerName = pc->name;
        }
    }
    if (g.camp.playerName.empty()) {
        for (const RosterMember& m : g.camp.roster) {
            if (!m.inSquad || m.displayName.empty()) continue;
            const std::string k = "name_" + m.displayName;
            std::string n = g.ui.Ready() ? g.ui.Strings().Get(k) : m.displayName;
            if (n == k) n = m.displayName;
            g.camp.playerName = n;
            break;
        }
    }

    // +0x0d.  The mission MARKER's own locale name, which is what the shipped
    // header holds ("Brahmin Wood", not `mission01.mis`).  Empty on the world
    // map, which is what the browser shows as no mission at all.
    g.camp.missionName.clear();
    g.camp.missionNameKey.clear();
    if (!g.camMission.empty()) {
        std::string key;
        for (const CoreMission& c : g.camp.cores) {
            if (c.misPath == g.camMission) {
                key = c.nameKey;
                break;
            }
        }
        g.camp.missionNameKey = key;
        if (!key.empty() && g.ui.Ready()) {
            const std::string n = g.ui.Strings().Get(key);
            if (n != key) g.camp.missionName = n;
        }
        if (g.camp.missionName.empty()) {
            std::string base = g.camMission;
            const size_t sl = base.find_last_of('/');
            if (sl != std::string::npos) base = base.substr(sl + 1);
            const size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base = base.substr(0, dot);
            g.camp.missionName = base;
        }
    }

    // +0x21 and +0x39.  The shipped header's portraits are 25 x 33, the size of
    // `gui/char/<Display Name>.zar`, and its six floats are the members' hit
    // points (30 / 20 / 27 for the player's own three-member squad).
    g.camp.portraits.clear();
    for (const RosterMember& m : g.camp.roster) {
        if (!m.inSquad) continue;
        if (g.camp.portraits.size() >= 6) break;
        CampaignState::SavePortrait p;
        p.hp = static_cast<float>(m.dead ? 0 : m.hp);
        std::string key = m.displayName;
        if (key.empty()) {
            key = m.entPath;
            const size_t sl = key.find_last_of('/');
            if (sl != std::string::npos) key = key.substr(sl + 1);
            const size_t dot = key.find_last_of('.');
            if (dot != std::string::npos) key = key.substr(0, dot);
            key = "CORE_rec_" + key;
        }
        if (g.ui.Ready()) {
            const Art* a = g.ui.Resources()->Image("gui/char/" + key + ".zar");
            if (a != nullptr && a->ok && a->w > 0 && a->h > 0) {
                p.w = 25;
                p.h = 33;
                p.rgba.assign(static_cast<size_t>(25) * 33 * 4, 0);
                for (int32_t y = 0; y < 33; ++y) {
                    for (int32_t x = 0; x < 25; ++x) {
                        const int32_t sx = x * a->w / 25, sy = y * a->h / 33;
                        std::memcpy(&p.rgba[(static_cast<size_t>(y) * 25 + x) * 4],
                                    &a->rgba[(static_cast<size_t>(sy) * a->w + sx) * 4], 4);
                    }
                }
            }
        }
        g.camp.portraits.push_back(std::move(p));
    }
}

void SaveMissionSnapshot() {
    g.camp.missionBlob.clear();
    g.camp.missionBlobName.clear();
    if (g.scenep == nullptr || g.camMission.empty()) return;
    std::string err;
    if (!g.scenep->WriteWorldSave(g.camMission, &g.camp.missionBlob, &err)) {
        std::fprintf(stderr, "save: cannot write the mission world: %s\n", err.c_str());
        g.camp.missionBlob.clear();
        return;
    }
    // `Campaign::SaveMissionToCampaign @006a0020` names the blob
    // `<missionBaseName>.sav` in the campaign scratch directory, and
    // `Campaign::MatchesMissionName @006a0a10` is what accepts it back.
    std::string base = g.camMission;
    const size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    g.camp.missionBlobName = "user/$$current$$/" + base + ".sav";
}

bool RestoreMissionSnapshot() {
    if (g.camp.missionBlob.empty()) return false;
    // The world names the map it was taken on; `World::Save` writes that path
    // as the first thing in the stream and `World::Load` re-reads the map from
    // it, which is what makes a save a few hundred kilobytes rather than three
    // megabytes.
    World probe;
    std::string err;
    if (!probe.LoadFromMemory(g.camp.missionBlob, "mission save", &err)) {
        std::fprintf(stderr, "load: bad mission world: %s\n", err.c_str());
        return false;
    }
    if (!probe.isSaveForm || probe.saveMapPath.empty()) {
        std::fprintf(stderr, "load: mission world names no map\n");
        return false;
    }
    g.pendingSaveWorld = g.camp.missionBlob;
    int32_t core = -1;
    for (size_t i = 0; i < g.camp.cores.size(); ++i) {
        if (g.camp.cores[i].misPath == probe.saveMapPath) {
            core = static_cast<int32_t>(i);
            break;
        }
    }
    if (!CampaignEnterMission(probe.saveMapPath, core, {})) {
        g.pendingSaveWorld.clear();
        return false;
    }
    return true;
}

// MILESTONE 15: the campaign probe.
//
// The shell drives this engine through a canvas it cannot read, so a browser
// test has no way to find a button. This exports the campaign UI's own hot-rect
// list -- the very list `HitTest` consults -- as JSON, so `scripts/
// e2e-fot-boot.mjs` can click the centre of the real "New Game" rect instead of
// a coordinate copied out of the source and silently wrong the moment the
// backdrop art changes size.
//
// Inspection only: it moves nothing and changes no state. The equivalent in the
// Fallout lanes is `rr_resize_status`.
extern "C" {

const char* rr_campaign_state() {
    static std::string out;
    const char* screen = "none";
    switch (g.camScreen) {
        case CampaignScreen::TitleMenu: screen = "title"; break;
        case CampaignScreen::MainMenu:  screen = "menu"; break;
        case CampaignScreen::Tutorials: screen = "tutorials"; break;
        case CampaignScreen::Custom:    screen = "custom"; break;
        case CampaignScreen::CharSelect: screen = "charselect"; break;
        case CampaignScreen::Briefing:  screen = "briefing"; break;
        case CampaignScreen::WorldMap:  screen = "map"; break;
        case CampaignScreen::Recruit:   screen = "recruit"; break;
        case CampaignScreen::SaveGame:  screen = "save"; break;
        case CampaignScreen::LoadGame:  screen = "load"; break;
        case CampaignScreen::Loading:   screen = "loading"; break;
        case CampaignScreen::EndScreen: screen = "endscreen"; break;
        default: break;
    }
    out = "{\"screen\":\"";
    out += screen;
    out += "\",\"mission\":\"";
    for (char ch : g.camMission) {
        if (ch == '\\' || ch == '"') out += '\\';
        out += ch;
    }
    out += "\",\"status\":\"";
    for (char ch : (g.camUi ? g.camUi->Status() : std::string())) {
        if (ch == '\\' || ch == '"') out += '\\';
        if (ch == '\n' || ch == '\r') { out += ' '; continue; }
        out += ch;
    }
    out += "\",\"hot\":[";
    if (g.camUi) {
        bool first = true;
        for (const CampaignUi::Hot& h : g.camUi->Hots()) {
            if (!first) out += ',';
            first = false;
            out += "{\"id\":" + std::to_string(h.id) +
                   ",\"cx\":" + std::to_string((h.r[0] + h.r[2]) / 2) +
                   ",\"cy\":" + std::to_string((h.r[1] + h.r[3]) / 2) + "}";
        }
        int32_t sx = 0, sy = 0;
        g.camUi->SquadScreen(&sx, &sy);
        out += "],\"squad\":[" + std::to_string(sx) + "," + std::to_string(sy) + "]";
    } else {
        out += "],\"squad\":[0,0]";
    }
    // The in-mission menu, when it is up. `screen` is "none" then (no campaign
    // screen is showing), so this is how a caller tells "in a mission with the
    // menu open" from "in a mission".
    out += ",\"menu\":[";
    if (g.uiOn && g.ui.Ready()) {
        bool first = true;
        for (int32_t i = 0; i < 6; ++i) {
            int32_t a = 0, b = 0, c2 = 0, d = 0;
            if (!g.ui.MenuButtonRect(i, &a, &b, &c2, &d)) continue;
            if (!first) out += ',';
            first = false;
            out += "{\"i\":" + std::to_string(i) + ",\"cx\":" + std::to_string((a + c2) / 2) +
                   ",\"cy\":" + std::to_string((b + d) / 2) + "}";
        }
    }
    out += "]}";
    return out.c_str();
}

}  // extern "C"

// A click on whichever campaign screen is up.
void CampaignClick(int32_t x, int32_t y) {
    g.camUi->SetMouse(x, y);
    const int cmd = g.camUi->HitTest(x, y);
    switch (g.camScreen) {
        // MILESTONE 20: `WMainMenu` -- the executable's own top-level menu.
        case CampaignScreen::TitleMenu:
            if (cmd == CampaignUi::kCmdSinglePlayer) {
                g.camScreen = CampaignScreen::MainMenu;
            } else if (cmd == CampaignUi::kCmdExit || cmd == CampaignUi::kCmdQuit) {
                g.running = false;
            } else if (cmd == CampaignUi::kCmdIntro) {
                // `WMainMenu::PlayIntroMovieAndInitChildControls @006c1890` plays
                // `movie/intro.bik` then `movie/ss00.bik`.  Bink is out of scope
                // (VIDEO_GAPS L3); the request is logged and the menu stays up.
                g.camUi->SetStatus("intro movie: movie/intro.bik (Bink, not decoded)");
                std::printf("campaign: intro movie requested (movie/intro.bik)\n");
            } else if (cmd == CampaignUi::kCmdMultiplayer) {
                g.camUi->SetStatus("multiplayer is not part of this port");
            } else if (cmd == CampaignUi::kCmdOptions || cmd == CampaignUi::kCmdCredits) {
                g.camUi->SetStatus("not implemented on the front end");
            }
            return;
        // The SINGLE PLAYER submenu, which is the screen the capture opens on.
        case CampaignScreen::MainMenu:
            if (cmd == CampaignUi::kCmdNewGame) {
                // NEW goes to CHARACTER SELECTION, not straight into the
                // campaign -- f0026 -> f0030 in the capture.
                CampaignNewGame();
                g.camScreen = CampaignScreen::CharSelect;
                g.camUi->SetStatus("");
            } else if (cmd == CampaignUi::kCmdLoadGame) {
                RefreshSaveSlots();
                g.camReturn = CampaignScreen::MainMenu;
                g.camScreen = CampaignScreen::LoadGame;
            } else if (cmd == CampaignUi::kCmdTutorials) {
                BuildTutorialList();
                g.camScreen = CampaignScreen::Tutorials;
            } else if (cmd == CampaignUi::kCmdCustomCampaign) {
                BuildCustomList(true);
                g.camScreen = CampaignScreen::Custom;
            } else if (cmd == CampaignUi::kCmdCustomMission) {
                BuildCustomList(false);
                g.camScreen = CampaignScreen::Custom;
            } else if (cmd == CampaignUi::kCmdBack || cmd == CampaignUi::kCmdQuit) {
                g.camScreen = CampaignScreen::TitleMenu;
            }
            return;
        // MILESTONE 21: the mission-end art is a bare picture -- f0357 has no
        // control on it -- so any click leaves it, for the debrief page if the
        // mission collected one and the world map otherwise.
        case CampaignScreen::EndScreen:
            (void)cmd;
            g.camScreen = g.camUi->m_brief.debrief && !g.camUi->m_brief.brief.empty()
                              ? CampaignScreen::Briefing
                              : g.camReturn;
            return;
        // The loading screen is not interactive: the load is synchronous and
        // the screen is gone before the next click is read.
        case CampaignScreen::Loading:
            return;
        case CampaignScreen::Tutorials:
        case CampaignScreen::Custom: {
            if (cmd == CampaignUi::kCmdBack) {
                g.camScreen = CampaignScreen::MainMenu;
                return;
            }
            if (cmd >= CampaignUi::kCmdListPick) {
                const size_t i = static_cast<size_t>(cmd - CampaignUi::kCmdListPick);
                if (i >= g.camUi->m_list.size()) return;
                const std::string path = g.camUi->m_list[i].second;
                if (path.size() > 4 && path.substr(path.size() - 4) == ".cam") {
                    g.camUi->SetStatus("custom campaigns beyond bos.cam are not loaded");
                    return;
                }
                // A tutorial or a custom mission is a one-off: the squad is the
                // campaign's, and there is no world map to come back to.
                OpenBriefing(path, -1);
            }
            return;
        }
        case CampaignScreen::CharSelect: {
            const size_t n = g.camUi->m_chars.size();
            if (cmd == CampaignUi::kCmdCharPrev && n > 0) {
                g.camUi->m_charSel = static_cast<int32_t>((g.camUi->m_charSel + n - 1) % n);
            } else if (cmd == CampaignUi::kCmdCharNext && n > 0) {
                g.camUi->m_charSel = static_cast<int32_t>((g.camUi->m_charSel + 1) % n);
            } else if (cmd == CampaignUi::kCmdCharTake) {
                const PrefabChar* p = g.camUi->SelectedChar();
                if (p != nullptr && p->ok) {
                    // TAKE puts the chosen pre-made at the head of the squad --
                    // it is the player character `mission00`'s starting roster
                    // is built around.
                    RosterMember m;
                    m.entPath = p->entPath;
                    m.displayName = p->displayName;
                    m.inSquad = true;
                    m.hp = p->hp;
                    m.maxHp = p->maxHp;
                    g.camp.roster.insert(g.camp.roster.begin(), m);
                    CampaignQuartermaster();
                    g.camScreen = CampaignScreen::WorldMap;
                    g.camUi->SetStatus(p->name +
                                       " joins the squad. Travel to a location and click it.");
                    std::printf("campaign: took prefab %s (%s)\n", p->name.c_str(),
                                p->entPath.c_str());
                }
            } else if (cmd == CampaignUi::kCmdCharModify ||
                       cmd == CampaignUi::kCmdCharCreate) {
                // `WCharacter` in create/modify mode (@00458b40, backdrops
                // `gui/back/CharCreate.zar` / `CharReview.zar`) is a screen of
                // its own -- 20 kB of spinner, tag-skill and trait constructors
                // -- and is a milestone, not a pass.  VIDEO_GAPS L8.
                g.camUi->SetStatus("character creation (WCharacter) is not implemented");
            } else if (cmd == CampaignUi::kCmdBack) {
                g.camScreen = CampaignScreen::MainMenu;
            }
            return;
        }
        case CampaignScreen::Briefing: {
            if (cmd >= CampaignUi::kCmdPipTab && cmd < CampaignUi::kCmdPipTab + 5) {
                const int32_t tab = cmd - CampaignUi::kCmdPipTab;
                if (tab == 4) {
                    // DONE.
                    if (g.camUi->m_brief.debrief) {
                        g.camScreen = g.camReturn;
                    } else {
                        CampaignEnterMission(g.camUi->m_brief.misPath, g.camUi->m_brief.core, {});
                    }
                } else {
                    g.camUi->m_pipTab = tab;
                    g.camUi->m_briefScroll = 0;
                }
                return;
            }
            if (cmd == CampaignUi::kCmdStartMission) {
                if (g.camUi->m_brief.debrief) {
                    g.camScreen = g.camReturn;
                } else {
                    CampaignEnterMission(g.camUi->m_brief.misPath, g.camUi->m_brief.core, {});
                }
            }
            return;
        }
        case CampaignScreen::WorldMap: {
            if (cmd == CampaignUi::kCmdSaveGame || cmd == CampaignUi::kCmdLoadGame) {
                RefreshSaveSlots();
                g.camReturn = CampaignScreen::WorldMap;
                g.camScreen = cmd == CampaignUi::kCmdSaveGame ? CampaignScreen::SaveGame
                                                              : CampaignScreen::LoadGame;
                return;
            }
            if (cmd == CampaignUi::kCmdMainMenu) {
                g.camScreen = CampaignScreen::MainMenu;
                return;
            }
            if (cmd >= CampaignUi::kCmdGotoCore) {
                const int32_t core = cmd - CampaignUi::kCmdGotoCore;
                if (core < static_cast<int32_t>(g.camp.cores.size())) {
                    const CoreMission& m = g.camp.cores[static_cast<size_t>(core)];
                    const double dx = g.camp.posX - m.locX, dy = g.camp.posY - m.locY;
                    if (dx * dx + dy * dy <= static_cast<double>(m.radius) * m.radius) {
                        // MILESTONE 20: the briefing comes first (f0102-f0110).
                        OpenBriefing(m.misPath, core);
                    } else {
                        g.camp.SetDestination(m.locX, m.locY);
                        g.camUi->SetStatus("travelling to " +
                                           g.ui.Strings().Get(m.nameKey));
                    }
                }
                return;
            }
            double px = 0, py = 0;
            if (g.camUi->MapPointAt(x, y, &px, &py)) g.camp.SetDestination(px, py);
            return;
        }
        case CampaignScreen::Recruit: {
            if (cmd == CampaignUi::kCmdWorldMap) {
                g.camScreen = CampaignScreen::WorldMap;
            } else if (cmd == CampaignUi::kCmdQuartermaster) {
                CampaignQuartermaster();
            } else if (cmd == CampaignUi::kCmdSaveGame) {
                RefreshSaveSlots();
                g.camReturn = CampaignScreen::Recruit;
                g.camScreen = CampaignScreen::SaveGame;
            } else if (cmd >= CampaignUi::kCmdDropRecruit) {
                const size_t i = static_cast<size_t>(cmd - CampaignUi::kCmdDropRecruit);
                if (i < g.camp.roster.size()) {
                    g.camp.pool.push_back(g.camp.roster[i].entPath);
                    g.camp.roster.erase(g.camp.roster.begin() + static_cast<long>(i));
                }
            } else if (cmd >= CampaignUi::kCmdTakeRecruit) {
                int32_t n = 0;
                for (const RosterMember& m : g.camp.roster) n += (m.inSquad && !m.dead) ? 1 : 0;
                const size_t i = static_cast<size_t>(cmd - CampaignUi::kCmdTakeRecruit);
                if (n < 6 && i < g.camp.pool.size()) {
                    RosterMember m;
                    m.entPath = g.camp.pool[i];
                    m.inSquad = true;
                    g.camp.roster.push_back(m);
                    g.camp.pool.erase(g.camp.pool.begin() + static_cast<long>(i));
                } else if (n >= 6) {
                    g.camUi->SetStatus("the squad is full");
                }
            }
            return;
        }
        case CampaignScreen::SaveGame:
        case CampaignScreen::LoadGame: {
            if (cmd == CampaignUi::kCmdBack) {
                g.camScreen = g.camReturn;
                return;
            }
            if (cmd >= CampaignUi::kCmdSlot) {
                g.camUi->m_slotSel = cmd - CampaignUi::kCmdSlot;
                return;
            }
            std::string e;
            if (cmd == CampaignUi::kCmdDoSave) {
                char name[64];
                std::snprintf(name, sizeof(name), "slot%02d.sav",
                              static_cast<int>(g.camUi->m_slots.size()) + 1);
                SaveMissionSnapshot();
                PrepareSaveHeaderFields();
                if (g.camp.Save(g.saveDir + "/" + name, name, &e)) {
                    g.camUi->SetStatus(std::string("saved ") + name);
                    std::printf("campaign: saved %s\n", name);
                    PersistSaves();
                    RefreshSaveSlots();
                } else {
                    g.camUi->SetStatus("save failed: " + e);
                    std::fprintf(stderr, "campaign: save failed: %s\n", e.c_str());
                }
            } else if (cmd == CampaignUi::kCmdDoLoad &&
                       g.camUi->m_slotSel < static_cast<int>(g.camUi->m_slots.size())) {
                if (g.camp.LoadSave(g.camUi->m_slots[g.camUi->m_slotSel].path, &e)) {
                    g.camScreen = CampaignScreen::WorldMap;
                    g.camUi->SetStatus("loaded");
                } else {
                    g.camUi->SetStatus("load failed: " + e);
                }
            }
            return;
        }
        default:
            return;
    }
}

// One frame with a campaign screen up: no world, no mission scene.
void CampaignFrame(int64_t deltaMs) {
    if (g.camScreen == CampaignScreen::WorldMap && g.camp.travelling) {
        const EncounterRoll r = g.camp.Step(static_cast<double>(deltaMs));
        if (r.fired) {
            g.camPending = r;
            g.camCountdown = CampaignState::kEncounterCountdown;
            g.camUi->m_encounterAt = r.forceName;
            g.camUi->m_encounterX = g.camp.posX;
            g.camUi->m_encounterY = g.camp.posY;
            g.camUi->SetStatus("encounter: " + r.forceName);
        } else if (!g.camp.travelling) {
            const int32_t core = g.camp.file.CoreAtPixel(static_cast<int32_t>(g.camp.posX),
                                                         static_cast<int32_t>(g.camp.posY));
            if (core >= 0) {
                g.camUi->SetStatus("arrived at " +
                                   g.ui.Strings().Get(g.camp.cores[core].nameKey) +
                                   " -- click it to enter");
            }
        }
    }
    // `WWorldWindow__ProcessWorldMapTick @006bcaa0`: the marker blinks for
    // eight ticks and the mission is entered on the eighth (l.24237).
    if (g.camCountdown > 0) {
        if (--g.camCountdown == 0) {
            g.camUi->m_encounterAt.clear();
            if (!g.camPending.mapPath.empty()) {
                CampaignEnterMission(g.camPending.mapPath, -1, g.camPending.spawn);
            }
            g.camPending = EncounterRoll();
        }
    }
    for (const std::string& l : g.camp.log) std::printf("CAMPAIGN %s\n", l.c_str());
    g.camp.log.clear();

    Canvas c;
    c.fb = g.fb.data();
    c.w = g.W;
    c.h = g.H;
    c.SetClip(0, 0, g.W, g.H);
    g.camUi->SetMouse(g.mouseX, g.mouseY);
    switch (g.camScreen) {
        case CampaignScreen::TitleMenu: g.camUi->DrawTitleMenu(&c, g.W, g.H); break;
        case CampaignScreen::MainMenu: g.camUi->DrawMainMenu(&c, g.W, g.H); break;
        case CampaignScreen::Tutorials:
        case CampaignScreen::Custom: g.camUi->DrawListMenu(&c, g.W, g.H); break;
        case CampaignScreen::CharSelect: g.camUi->DrawCharSelect(&c, g.W, g.H); break;
        case CampaignScreen::Briefing: g.camUi->DrawBriefing(&c, g.W, g.H); break;
        case CampaignScreen::WorldMap: g.camUi->DrawWorldMap(&c, g.W, g.H, g.camp); break;
        case CampaignScreen::Recruit: g.camUi->DrawRecruit(&c, g.W, g.H, g.camp); break;
        case CampaignScreen::SaveGame: g.camUi->DrawSaveLoad(&c, g.W, g.H, true); break;
        case CampaignScreen::LoadGame: g.camUi->DrawSaveLoad(&c, g.W, g.H, false); break;
        // MILESTONE 21.
        case CampaignScreen::Loading: g.camUi->DrawLoading(&c, g.W, g.H); break;
        case CampaignScreen::EndScreen: g.camUi->DrawEndScreen(&c, g.W, g.H); break;
        default: break;
    }
    // The pointer, last, over everything. The OS cursor is hidden for the whole
    // run (see SDL_ShowCursor), and these screens aren't drawn by `Ui::Draw`,
    // which draws the mission's own cursor. Outside the window the OS draws
    // its own, so ours goes rather than sitting at the edge.
    if (g.mouseIn) g.ui.DrawPointer(&c, g.mouseX, g.mouseY);
}

void RunCamScript() {
    for (size_t i = 0; i < g.camScript.size();) {
        const std::string& s = g.camScript[i];
        const size_t c = s.find(':');
        if (c == std::string::npos || std::atoll(s.c_str()) != g.frameNo) {
            ++i;
            continue;
        }
        std::string rest = s.substr(c + 1);
        std::string verb = rest;
        std::string arg;
        const size_t c2 = rest.find(':');
        if (c2 != std::string::npos) {
            verb = rest.substr(0, c2);
            arg = rest.substr(c2 + 1);
        }
        if (verb == "newgame") {
            CampaignNewGame();
        // MILESTONE 21: the two picture pages, for the verification harness --
        // `loading:<1..7>,<percent>` and `endscreen:<resource path>`.  In a
        // real run the wait plate is picked by `PaintLoadingScreen` and the end
        // plate by `ActionEndMission`'s own `Exit to`.
        } else if (verb == "loading") {
            const size_t comma = arg.find(',');
            const int which = std::atoi(arg.c_str());
            const float pct = comma == std::string::npos
                                  ? 0.0f
                                  : static_cast<float>(std::atof(arg.c_str() + comma + 1));
            char name[32];
            std::snprintf(name, sizeof(name), "gui/wait/wait%d.jpg",
                          which >= 1 && which <= 7 ? which : 1);
            g.camUi->SetLoading(name, pct / 100.0f);
            g.camScreen = CampaignScreen::Loading;
        } else if (verb == "endscreen") {
            g.camUi->SetEndScreen(arg.empty() ? "gui/back/DEATHscreen.JPG" : arg);
            g.camScreen = CampaignScreen::EndScreen;
        } else if (verb == "screen") {
            g.camScreen = arg == "title"    ? CampaignScreen::TitleMenu
                          : arg == "menu"   ? CampaignScreen::MainMenu
                          : arg == "tutorials" ? CampaignScreen::Tutorials
                          : arg == "custom" ? CampaignScreen::Custom
                          : arg == "charselect" ? CampaignScreen::CharSelect
                          : arg == "briefing" ? CampaignScreen::Briefing
                          : arg == "map"    ? CampaignScreen::WorldMap
                          : arg == "recruit" ? CampaignScreen::Recruit
                          : arg == "save"   ? CampaignScreen::SaveGame
                          : arg == "load"   ? CampaignScreen::LoadGame
                          : arg == "loading" ? CampaignScreen::Loading
                          : arg == "endscreen" ? CampaignScreen::EndScreen
                                            : CampaignScreen::None;
            if (g.camScreen == CampaignScreen::SaveGame ||
                g.camScreen == CampaignScreen::LoadGame) {
                RefreshSaveSlots();
            }
            // MILESTONE 20: the three new screens need their content built,
            // exactly as the click that opens them does.
            if (g.camScreen == CampaignScreen::Tutorials) BuildTutorialList();
            if (g.camScreen == CampaignScreen::Custom) BuildCustomList(false);
            if (g.camScreen == CampaignScreen::CharSelect && g.camUi->m_chars.empty()) {
                LoadPrefabs();
    if (g.uiOn && g.ui.Ready()) g.ui.SetCampaignShell(true);
            }
            if (g.camScreen == CampaignScreen::Briefing && g.camUi->m_brief.brief.empty()) {
                const int32_t core = g.camp.file.CoreAtPixel(static_cast<int32_t>(g.camp.posX),
                                                             static_cast<int32_t>(g.camp.posY));
                OpenBriefing(core >= 0 ? g.camp.cores[static_cast<size_t>(core)].misPath
                                       : std::string("campaigns/missions/core/mission01.mis"),
                             core);
            }
        } else if (verb == "take") {
            const int k = std::atoi(arg.c_str());
            if (k >= 0 && k < static_cast<int>(g.camp.pool.size())) {
                RosterMember m;
                m.entPath = g.camp.pool[k];
                m.inSquad = true;
                g.camp.roster.push_back(m);
                g.camp.pool.erase(g.camp.pool.begin() + k);
            }
        } else if (verb == "drop") {
            const int k = std::atoi(arg.c_str());
            if (k >= 0 && k < static_cast<int>(g.camp.roster.size())) {
                g.camp.pool.push_back(g.camp.roster[k].entPath);
                g.camp.roster.erase(g.camp.roster.begin() + k);
            }
        } else if (verb == "travel") {
            const int32_t core = g.camp.FindCore(arg);
            if (core >= 0) {
                g.camp.SetDestination(g.camp.cores[core].locX, g.camp.cores[core].locY);
            } else {
                int64_t v[2] = {0, 0};
                if (ft::ParseInts(arg.c_str(), 2, v)) {
                    g.camp.SetDestination(static_cast<double>(v[0]), static_cast<double>(v[1]));
                }
            }
        } else if (verb == "dump") {
            // The verification surface: everything a save has to round-trip.
            std::printf("STATE mission=%s pos=%.1f,%.1f time=%lld rep=%d vars=%zu\n",
                        g.camp.currentMission.c_str(), g.camp.posX, g.camp.posY,
                        (long long)g.camp.timeMs, g.camp.reputation, g.camp.vars.size());
            for (size_t k = 0; k < g.camp.roster.size(); ++k) {
                const RosterMember& m = g.camp.roster[k];
                std::printf("STATE roster %zu %s hp=%d/%d xp=%d lvl=%d dead=%d squad=%d "
                            "h1=%s arm=%s inv=%zu\n",
                            k, m.entPath.c_str(), m.hp, m.maxHp, m.xp, m.level, m.dead ? 1 : 0,
                            m.inSquad ? 1 : 0, m.hand1.c_str(), m.armour.c_str(),
                            m.inventory.size());
            }
            for (size_t k = 0; k < g.camp.cores.size(); ++k) {
                if (g.camp.cores[k].state == MissionState::Covered) continue;
                std::printf("STATE core %zu %s %d\n", k, g.camp.cores[k].nameKey.c_str(),
                            static_cast<int>(g.camp.cores[k].state));
            }
            std::printf("STATE pool %zu stock %zu\n", g.camp.pool.size(), g.camp.stock.size());
            if (g.scenep != nullptr && !g.camMission.empty()) {
                const std::vector<LiveEntity>& es = g.scenep->Entities();
                const std::vector<int32_t>& sq = g.scenep->Squad();
                for (size_t k = 0; k < sq.size(); ++k) {
                    const LiveEntity& e = es[static_cast<size_t>(sq[k])];
                    std::printf("STATE unit %zu %s at %d,%d,%d hp=%d dead=%d ammo=%d w=%s\n", k,
                                e.label.c_str(), e.world[0], e.world[1], e.world[2], e.hp,
                                e.dead ? 1 : 0, e.ammo,
                                e.weapon.valid ? e.weapon.path.c_str() : "-");
                }
                std::printf("STATE objectives %zu exitgrids %d\n",
                            g.scenep->Script().Objectives().size(),
                            g.scenep->Script().ExitGridsOn() ? 1 : 0);
            }
        } else if (verb == "encounters") {
            // `ActionSetRandomEncounterEnabled::Vtable15 @0068a770` sets the
            // per-force enabled byte; this is the campaign-wide switch the
            // same flag amounts to.
            g.camp.encountersEnabled = arg != "off";
            std::printf("campaign: random encounters %s\n",
                        g.camp.encountersEnabled ? "on" : "off");
        } else if (verb == "pip") {
            // MILESTONE 20: pick a Pip-Boy tab (0..4) on the briefing page.
            g.camUi->m_pipTab = std::max(0, std::min(4, std::atoi(arg.c_str())));
            g.camUi->m_briefScroll = 0;
        } else if (verb == "pipscroll") {
            g.camUi->ScrollBriefing(std::atoi(arg.c_str()));
        } else if (verb == "char") {
            if (!g.camUi->m_chars.empty()) {
                g.camUi->m_charSel = std::max(0, std::atoi(arg.c_str())) %
                                     static_cast<int32_t>(g.camUi->m_chars.size());
            }
        } else if (verb == "debrief") {
            // MILESTONE 20, test hook: open the DEBRIEF form of the Pip-Boy
            // page on an explicit comma-separated key list, so the screen can
            // be verified without playing a mission to its end.  The live path
            // is `CampaignLeaveMission`, which takes the same keys from
            // `MissionScript::DebriefLines()`.
            BriefingData d;
            d.debrief = true;
            d.missionKey = g.camMission;
            size_t p0 = 0;
            while (p0 <= arg.size()) {
                const size_t p1 = std::min(arg.find(',', p0), arg.size());
                const std::string key = arg.substr(p0, p1 - p0);
                if (!key.empty()) {
                    if (!d.brief.empty()) d.brief += "\\n\\n";
                    d.brief += g.ui.Strings().Get(key);
                }
                p0 = p1 + 1;
            }
            OpenDebrief(&d);
        } else if (verb == "brief") {
            OpenBriefing(arg.empty() ? std::string("campaigns/missions/core/mission01.mis") : arg,
                         g.camp.FindCore(arg));
        } else if (verb == "quartermaster") {
            CampaignQuartermaster();
        } else if (verb == "enter") {
            const int32_t core = arg.empty()
                                     ? g.camp.file.CoreAtPixel(static_cast<int32_t>(g.camp.posX),
                                                               static_cast<int32_t>(g.camp.posY))
                                     : g.camp.FindCore(arg);
            if (core >= 0) {
                CampaignEnterMission(g.camp.cores[core].misPath, core, {});
            } else {
                g.camUi->SetStatus("no mission here");
            }
        } else if (verb == "exit") {
            CampaignLeaveMission();
        } else if (verb == "save") {
            std::string e;
            // The screenshot the `<saveh>` block carries.
            g.camp.thumbW = 160;
            g.camp.thumbH = 120;
            g.camp.thumb.assign(static_cast<size_t>(160) * 120 * 4, 0);
            for (int32_t y = 0; y < 120; ++y) {
                for (int32_t x = 0; x < 160; ++x) {
                    const int32_t sxp = x * g.W / 160, syp = y * g.H / 120;
                    std::memcpy(&g.camp.thumb[(static_cast<size_t>(y) * 160 + x) * 4],
                                &g.fb[(static_cast<size_t>(syp) * g.W + sxp) * 4], 4);
                }
            }
            SaveMissionSnapshot();
            PrepareSaveHeaderFields();
            if (!g.camp.Save(arg, "slot", &e)) {
                std::fprintf(stderr, "save %s: %s\n", arg.c_str(), e.c_str());
            } else {
                std::printf("campaign: saved %s\n", arg.c_str());
                PersistSaves();
            }
        } else if (verb == "load") {
            std::string e;
            if (!g.camp.LoadSave(arg, &e)) {
                std::fprintf(stderr, "load %s: %s\n", arg.c_str(), e.c_str());
            } else {
                std::printf("campaign: loaded %s\n", arg.c_str());
                if (!g.camp.missionBlob.empty() && RestoreMissionSnapshot()) {
                    std::printf("campaign: resumed mission %s\n", g.camp.currentMission.c_str());
                } else {
                    g.camScreen = CampaignScreen::WorldMap;
                }
            }
        } else if (verb == "click") {
            int64_t v[2] = {0, 0};
            if (ft::ParseInts(arg.c_str(), 2, v)) {
                // A real click happens where the pointer is, so the scripted
                // one moves it there too (and a capture shows it there).
                g.mouseX = static_cast<int>(v[0]);
                g.mouseY = static_cast<int>(v[1]);
                g.mouseIn = true;
                CampaignClick(static_cast<int32_t>(v[0]), static_cast<int32_t>(v[1]));
            }
        }
        g.camScript.erase(g.camScript.begin() + static_cast<long>(i));
    }
}


void PresentFrame(int64_t deltaMs);
void CampaignClick(int32_t x, int32_t y);
void CampaignFrame(int64_t deltaMs);
void RunCamScript();

void Tick() {
    // --- time ---
    const uint32_t now = SDL_GetTicks();
    int64_t deltaMs = g.headless ? 33 : static_cast<int64_t>(now - g.lastTicks);
    g.lastTicks = now;
    if (deltaMs < 0) deltaMs = 0;
    if (deltaMs > 250) deltaMs = 250;

    // --- input ---
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                g.running = false;
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                const bool down = ev.type == SDL_KEYDOWN;
                switch (ev.key.keysym.sym) {
                    case SDLK_LEFT: g.keyLeft = down; break;
                    case SDLK_RIGHT: g.keyRight = down; break;
                    case SDLK_UP: g.keyUp = down; break;
                    case SDLK_DOWN: g.keyDown = down; break;
                    case SDLK_LSHIFT:
                    case SDLK_RSHIFT: g.runByDefault = down; break;
                    default: break;
                }
                // MILESTONE 9: the bindings are `data/core/keys.cfg`'s own.
                // `WGameMain::WGameMain @00470990` resolves them by name out of
                // that file and dispatches them
                // through `WGameMain::DispatchInputActionByName @004734f0`, so
                // the mapping below is key -> action name -> Ui::Action, which
                // is the same indirection.  Milestone 6-8's own `s`/`c`/`p`
                // stance keys are GONE: keys.cfg binds S, C and P to skilldex,
                // character and pipboy, and Q / A / Z to the three stances.
                if (down) {
                    const char* action = nullptr;
                    switch (ev.key.keysym.sym) {
                        case SDLK_i: action = "inventory"; break;
                        case SDLK_s: action = "skilldex"; break;
                        case SDLK_c: action = "character"; break;
                        case SDLK_p: action = "pipboy"; break;
                        case SDLK_m: action = "pipboyMap"; break;
                        case SDLK_ESCAPE: action = "menu"; break;
                        case SDLK_q: action = "stand"; break;
                        case SDLK_a: action = "crouch"; break;
                        case SDLK_z: action = "prone"; break;
                        case SDLK_w: action = "stop"; break;
                        case SDLK_r: action = "reload"; break;
                        case SDLK_y: action = "cycleWeaponModes"; break;
                        case SDLK_u: action = "swapHands"; break;
                        case SDLK_SPACE: action = "cycleMembers"; break;
                        case SDLK_BACKSPACE: action = "characterEndTurn/stop"; break;
                        case SDLK_F1: action = "select1"; break;
                        case SDLK_F2: action = "select2"; break;
                        case SDLK_F3: action = "select3"; break;
                        case SDLK_F4: action = "select4"; break;
                        case SDLK_F5: action = "select5"; break;
                        case SDLK_F6: action = "select6"; break;
                        // MILESTONE 16: the rest of keys.cfg's selection block.
                        case SDLK_F7: action = "recallGroup1"; break;
                        case SDLK_F8: action = "recallGroup2"; break;
                        case SDLK_F9: action = "recallGroup3"; break;
                        case SDLK_F10: action = "recallGroup4"; break;
                        case SDLK_F11: action = "recallGroup5"; break;
                        case SDLK_F12: action = "selectAll"; break;
                        case SDLK_COMMA: action = "sentryOff"; break;
                        case SDLK_PERIOD: action = "sentryScout"; break;
                        case SDLK_SLASH: action = "sentryOn"; break;
                        // MILESTONE 18: keys.cfg's `1`..`8`, which are the
                        // skilldex in the order of the `.data` pointer table at
                        // 0x008a907c -- sneak, lockpick, steal, traps,
                        // firstAid, doctor, science, repair.  All eight go
                        // through `WGameWorld::EvaluateSneakSkillCondition
                        // @0049de10`; see NOTES.md 100.
                        case SDLK_1: action = "sneak"; break;
                        case SDLK_2: action = "lockpick"; break;
                        case SDLK_3: action = "steal"; break;
                        case SDLK_4: action = "traps"; break;
                        case SDLK_5: action = "firstAid"; break;
                        case SDLK_6: action = "doctor"; break;
                        case SDLK_7: action = "science"; break;
                        case SDLK_8: action = "repair"; break;
                        default: break;
                    }
                    // `<shift>F1..F6 = {modSelect1..6}`, `<ctrl>F7..F11 =
                    // {setGroup1..5}`.
                    std::string mod;
                    if (action != nullptr) {
                        if ((ev.key.keysym.mod & KMOD_SHIFT) != 0 &&
                            std::strncmp(action, "select", 6) == 0 && std::strlen(action) == 7) {
                            mod = std::string("modSelect") + action[6];
                            action = mod.c_str();
                        } else if ((ev.key.keysym.mod & KMOD_CTRL) != 0 &&
                                   std::strncmp(action, "recallGroup", 11) == 0) {
                            mod = std::string("setGroup") + action[11];
                            action = mod.c_str();
                        }
                    }
                    // `<alt>X = {quit}`.
                    if (ev.key.keysym.sym == SDLK_x && (ev.key.keysym.mod & KMOD_ALT) != 0) {
                        g.running = false;
                    } else if (action != nullptr && g.uiOn && g.ui.Ready()) {
                        g.ui.Action(g.scenep, action);
                    }
                }
                break;
            }
            case SDL_MOUSEMOTION:
                g.mouseX = ev.motion.x;
                g.mouseY = ev.motion.y;
                g.mouseIn = true;
                if (g.bandActive) {
                    g.bandX1 = ev.motion.x;
                    g.bandY1 = ev.motion.y;
                }
                // MILESTONE 15: a campaign screen owns the pointer while it is
                // up, so the hover highlight follows it there rather than in
                // the mission interface underneath.
                if (g.campaignMode && g.camScreen != CampaignScreen::None && g.camUi) {
                    g.camUi->SetMouse(ev.motion.x, ev.motion.y);
                    break;
                }
                if (g.uiOn && g.ui.Ready()) g.ui.MouseMove(ev.motion.x, ev.motion.y);
                break;
            case SDL_MOUSEBUTTONUP: {
                // A press or release happens where the pointer is, even with no
                // motion before it (a touch, or a pen tap).
                g.mouseX = ev.button.x;
                g.mouseY = ev.button.y;
                g.mouseIn = true;
                const bool band = g.bandActive && ev.button.button == SDL_BUTTON_LEFT;
                g.bandActive = false;
                bool taken = false;
                if (g.uiOn && g.ui.Ready()) {
                    taken = g.ui.MouseUp(g.scenep, ev.button.x, ev.button.y,
                                         ev.button.button == SDL_BUTTON_RIGHT);
                }
                if (!band || (taken && ev.button.y >= Ui::ViewportHeight(g.H))) break;
                const int32_t dx = ev.button.x - g.bandX0, dy = ev.button.y - g.bandY0;
                const SDL_Keymod km = SDL_GetModState();
                if (dx * dx + dy * dy > kBandThreshold * kBandThreshold) {
                    BandSelect(g.bandX0, g.bandY0, ev.button.x, ev.button.y,
                               (km & KMOD_SHIFT) != 0);
                } else {
                    WorldClick(ev.button.x, ev.button.y, false, (km & KMOD_SHIFT) != 0,
                               (km & KMOD_CTRL) != 0);
                }
                break;
            }
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                    g.mouseIn = false;
                } else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
                           ev.window.data1 >= 64 && ev.window.data2 >= 64) {
                    g.W = ev.window.data1;
                    g.H = ev.window.data2;
                    g.fb.assign(static_cast<size_t>(g.W) * g.H * 4, 0);
                    if (g.texture != nullptr) {
                        SDL_DestroyTexture(g.texture);
                    }
                    g.texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888,
                                                  SDL_TEXTUREACCESS_STREAMING, g.W, g.H);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                // MILESTONE 15: the campaign screens are mouse-driven.
                //
                // Until now `CampaignClick` had exactly one caller -- the
                // `--cam click:x,y` script verb -- so the main menu, the world
                // map, the recruit screen and save/load could only be operated
                // from the command line. That was survivable for a milestone
                // driven by scripted checks and impossible for the browser lane,
                // where the command line is one fixed argv and the player has
                // nothing but a pointer. A campaign screen takes the click
                // before the mission interface, because while one is up there
                // is no mission being simulated (see the CampaignFrame guard
                // below).
                g.mouseX = ev.button.x;  // as on release: the press is where the pointer is
                g.mouseY = ev.button.y;
                g.mouseIn = true;
                if (g.campaignMode && g.camScreen != CampaignScreen::None) {
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        CampaignClick(ev.button.x, ev.button.y);
                    }
                    break;
                }
                // The interface gets the click first, and the world only sees
                // it when the interface did not take it -- which is what the
                // window manager's own hit test does.
                if (g.uiOn && g.ui.Ready() &&
                    g.ui.MouseDown(g.scenep, ev.button.x, ev.button.y,
                                   ev.button.button == SDL_BUTTON_RIGHT)) {
                    break;
                }
                // MILESTONE 16: a left press starts a band box; the click
                // itself is decided on release, when we know whether the
                // pointer travelled more than the 15 px
                // `WGameWorld::GetSelectionCursorIcon @0065b300` allows.
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    g.bandActive = true;
                    g.bandX0 = ev.button.x;
                    g.bandY0 = ev.button.y;
                    g.bandX1 = ev.button.x;
                    g.bandY1 = ev.button.y;
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    const SDL_Keymod km = SDL_GetModState();
                    WorldClick(ev.button.x, ev.button.y, true, (km & KMOD_SHIFT) != 0,
                               (km & KMOD_CTRL) != 0);
                }
                break;
            default:
                break;
        }
    }

    // MILESTONE 12: while a campaign screen is up there is no mission scene to
    // simulate; the campaign frame replaces everything below.
    if (g.campaignMode && g.camScreen != CampaignScreen::None) {
        RunCamScript();
        CampaignFrame(deltaMs);
        for (const std::string& sh : g.shotAt) {
            const size_t c = sh.find(':');
            if (c != std::string::npos && std::atoll(sh.c_str()) == g.frameNo) {
                SaveShot(sh.substr(c + 1));
            }
        }
        PresentFrame(deltaMs);
        return;
    }
    if (g.campaignMode) RunCamScript();

    // MILESTONE 15: the in-mission menu's Save / Load / Exit.
    //
    // `Ui` draws the panel but knows nothing about the campaign, so it records
    // the click and this consumes it. Save and Load raise the campaign's own
    // save screen over the paused mission -- `camReturn` is `None`, which is
    // "back to the mission", and the CampaignFrame guard above already stops
    // simulating while a campaign screen is up. Exit is `CampaignLeaveMission`,
    // the same call `--cam exit` makes.
    // MILESTONE 16: this used to be gated on `g.campaignMode`, so in a plain
    // `fotgame <mission>.mis` run NOTHING on the menu worked except Done.
    // Quick Keys and Options are the interface's own pages and are handled
    // inside `Ui`; Save, Load and Exit need the campaign layer, and outside it
    // they say so instead of silently doing nothing.
    if (g.uiOn && g.ui.Ready()) {
        const int32_t choice = g.ui.TakeMenuChoice();
        const bool haveCampaign = g.campaignMode && g.camUi != nullptr;
        if (choice == 1 || choice == 2) {  // gameMenu_saveGame / gameMenu_loadGame
            if (haveCampaign) {
                RefreshSaveSlots();
                g.camReturn = CampaignScreen::None;
                g.camScreen = choice == 1 ? CampaignScreen::SaveGame : CampaignScreen::LoadGame;
            } else {
                g.ui.PushMessage(choice == 1
                                     ? "Save needs the campaign shell: run with --campaign."
                                     : "Load needs the campaign shell: run with --campaign.");
            }
        } else if (choice == 4) {  // gameMenu_exit
            if (haveCampaign) {
                // "Exit to Main" -- the label is explicit, so the mission is
                // left (`ExitMissionToWorldMap @006a2410`, which is what
                // `CampaignLeaveMission` ports) and the shell goes on to the
                // main menu rather than stopping on the world map.
                CampaignLeaveMission();
                g.camScreen = CampaignScreen::MainMenu;
                g.camUi->SetStatus("Left the mission.");
            } else {
                // "Exit to Main" with no campaign shell is the end of the run.
                std::printf("menu: exit to main -- no campaign shell, quitting\n");
                g.running = false;
            }
        }
    }

    // --- scroll ---
    double dx = 0, dy = 0;
    if (g.keyLeft) dx -= 1;
    if (g.keyRight) dx += 1;
    if (g.keyUp) dy -= 1;
    if (g.keyDown) dy += 1;
    if (g.mouseIn && !g.headless) {
        if (g.mouseX >= 0 && g.mouseX < kEdgeBand) dx -= 1;
        if (g.mouseX >= g.W - kEdgeBand && g.mouseX < g.W) dx += 1;
        if (g.mouseY >= 0 && g.mouseY < kEdgeBand) dy -= 1;
        if (g.mouseY >= g.H - kEdgeBand && g.mouseY < g.H) dy += 1;
    }
    if (dx != 0 || dy != 0) {
        const double step = kScrollPxPerSec * deltaMs / 1000.0;
        g.camFx += dx * step;
        g.camFy += dy * step;
        ClampCamera();
    }

    RunScript();
    RunOrders();
    if (g.bench) {
        // Scroll one screen-space unit per frame in both axes so nothing is
        // measured out of a warm cache.
        g.camFx += 4;
        g.camFy += 2;
        ClampCamera();
    }

    // --- simulate and draw ---
    // MILESTONE 13: the listener is the camera.  `Set3DListenerOrientation
    // @0071e580` takes the view point every frame; here that is the world
    // point under the middle of the world VIEWPORT -- the game bar owns the
    // bottom 128 px, so it is not the middle of the window.
    if (g.audio.Enabled()) {
        int32_t wx = 0, wz = 0;
        const int32_t vh = g.uiOn ? Ui::ViewportHeight(g.H) : g.H;
        // ScreenToGround writes the point whether or not it found a floor
        // there (it returns false only to say "no walkable surface"), and the
        // listener wants the point either way.
        g.scenep->ScreenToGround(g.camX, g.camY, g.W / 2, vh / 2, &wx, &wz);
        g.scenep->SetListener(wx, wz);
        g.audio.SetTick(g.scenep->Ticks());
    }
    g.scenep->Update(deltaMs);
    if (g.audio.Enabled()) g.audio.Update(deltaMs);
    if (g.follow && g.scenep->Selected() >= 0) {
        const LiveEntity& e = g.scenep->Entities()[static_cast<size_t>(g.scenep->Selected())];
        int32_t cx = 0, cy = 0;
        ProjectWorldToScreen(e.world[0], e.world[1], e.world[2], &cx, &cy);
        g.camFx = cx - g.W / 2.0;
        g.camFy = cy - g.H / 2.0;
        ClampCamera();
    }
    if (g.uiOn && g.ui.Ready()) {
        g.ui.SetCamera(g.camX, g.camY);
        g.ui.SetClock(g.scenep->NowMs());
        const SDL_Keymod km = SDL_GetModState();
        g.ui.SetModifiers((km & KMOD_SHIFT) != 0, (km & KMOD_CTRL) != 0);
        const ft::Volumes& av = g.audio.GetVolumes();
        g.ui.SetVolumes(av.master, av.music, av.sfx, av.speech);
        // MILESTONE 16: a double-clicked squad tab (or `space`) recentres.
        const int32_t cr = g.ui.TakeCentreRequest();
        if (cr >= 0 && cr < static_cast<int32_t>(g.scenep->Entities().size())) {
            const LiveEntity& ce = g.scenep->Entities()[static_cast<size_t>(cr)];
            int32_t cx = 0, cy = 0;
            ProjectWorldToScreen(ce.world[0], ce.world[1], ce.world[2], &cx, &cy);
            g.camFx = cx - g.W / 2.0;
            g.camFy = cy - Ui::ViewportHeight(g.H) / 2.0;
            ClampCamera();
        }
    }
    RenderStats st;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    g.scenep->Render(g.camX, g.camY, g.W, g.H, g.fb.data(), &st);
    g.renderTicks += SDL_GetPerformanceCounter() - t0;
    g.renderFrames += 1;
    g.lastStats = st;
    // MILESTONE 16: every selected member gets a ring, not just the primary.
    // MILESTONE 22 (M5): the squad's ring is the capture's near-white, and a
    // HOSTILE gets one too -- bright red, two pixels thick -- which is what
    // f0348 and f0350 show under the raider the squad is fighting.  Which
    // hostiles carry one is a RECONSTRUCTION: the capture only ever shows a
    // ring on an enemy that is on screen and alive, and the port has no
    // per-entity "the player can see this" bit, so every live hostile
    // combatant inside the viewport gets one.
    for (int32_t s : g.scenep->Selection()) {
        DrawSelectionRing(g.scenep->Entities()[static_cast<size_t>(s)], 240, 228, 212, 3);
    }
    {
        const std::vector<LiveEntity>& es = g.scenep->Entities();
        const std::vector<int32_t>& sel = g.scenep->Selection();
        if (!sel.empty()) {
            const int32_t me = sel.front();
            for (size_t i = 0; i < es.size(); ++i) {
                const LiveEntity& h = es[i];
                if (!h.isCombatant || h.dead) continue;
                if (!g.scenep->IsHostile(me, static_cast<int32_t>(i))) continue;
                DrawSelectionRing(h, 200, 40, 40, 2);
            }
        }
    }
    if (g.scenep->Selected() >= 0 && g.drawPath) DrawPath(g.scenep->Selected());
    // The band box, while the button is down.
    if (g.bandActive) {
        int32_t x0 = std::min(g.bandX0, g.bandX1), x1 = std::max(g.bandX0, g.bandX1);
        int32_t y0 = std::min(g.bandY0, g.bandY1), y1 = std::max(g.bandY0, g.bandY1);
        auto px = [&](int32_t x, int32_t y) {
            if (x < 0 || y < 0 || x >= g.W || y >= g.H) return;
            uint8_t* p = g.fb.data() + (static_cast<size_t>(y) * g.W + x) * 4;
            p[0] = 60; p[1] = 255; p[2] = 90; p[3] = 255;
        };
        for (int32_t x = x0; x <= x1; ++x) { px(x, y0); px(x, y1); }
        for (int32_t y = y0; y <= y1; ++y) { px(x0, y); px(x1, y); }
    }

    // Drain the combat console line.  Milestone 9 routes it into the game
    // bar's own message list (the `WTextList` that shares the minimap's rect,
    // toggled by the `mmtext` checkbox at (233, 79)); it still goes to stdout
    // as well, because the verification harness reads it there.
    if (g.showLog && !g.scenep->Log().empty()) {
        for (const std::string& l : g.scenep->Log()) {
            std::printf("[tick %lld] %s\n", (long long)g.scenep->Ticks(), l.c_str());
            g.hud.push_back(l);
            if (g.uiOn && g.ui.Ready()) g.ui.PushMessage(l);
        }
        while (g.hud.size() > 6) g.hud.erase(g.hud.begin());
        g.scenep->ClearLog();
    }
    // MILESTONE 17: a queued activate that reached its target and opened the
    // loot window.
    if (g.scenep->TakeLootJustOpened() >= 0 && g.uiOn && g.ui.Ready()) {
        g.ui.SetScreen(game::UiScreen::Loot);
    }
    // MILESTONE 10: the trigger engine's own log, drained the same way.
    if (g.scriptLog) {
        const std::vector<std::string>& sl = g.scenep->Script().Log();
        for (size_t i = g.scriptLogSeen; i < sl.size(); ++i) {
            std::printf("[tick %lld] SCRIPT %s\n", (long long)g.scenep->Ticks(), sl[i].c_str());
        }
        g.scriptLogSeen = sl.size();
    }
    // MILESTONE 12: the campaign layer's two hooks into a running mission --
    // `ActionSetMissionState`'s queued requests, and `ActionEndMission`'s
    // `Exit to = "World Map"`, which `ExitMissionToWorldMap @006a2410` acts on.
    if (g.campaignMode && !g.camMission.empty()) {
        DrainMissionStateRequests();
        if (g.scenep->Script().Ended() != MissionEnd::None) {
            CampaignLeaveMission();
        }
    }
    // ActionCenterView asks the window to recentre on a unit.
    {
        const int32_t c = g.scenep->TakeCentreRequest();
        if (c >= 0 && c < static_cast<int32_t>(g.scenep->Entities().size())) {
            const LiveEntity& ce = g.scenep->Entities()[static_cast<size_t>(c)];
            int32_t cx = 0, cy = 0;
            ProjectWorldToScreen(ce.world[0], ce.world[1], ce.world[2], &cx, &cy);
            g.camFx = cx - g.W / 2.0;
            g.camFy = cy - g.H / 2.0;
            ClampCamera();
        }
    }
    if (g.traceCombat && g.traceEvery > 0 && g.frameNo % g.traceEvery == 0) {
        const std::string t = g.scenep->TraceCombat();
        if (!t.empty()) std::printf("%s", t.c_str());
    }
    // The combat HUD: the mode line, the selection's own state, and the last
    // few console messages.  There is no game UI yet (milestone 8 prints its
    // CombatMessages); this exists so a screenshot can carry the same text.
    if (g.showHud) {
        char buf[256];
        int y = 6;
        const int32_t act = g.scenep->ActiveTurnEntity();
        if (g.scenep->TurnBased()) {
            std::snprintf(buf, sizeof(buf), "TURN-BASED  turn: %s  queue %zu",
                          act >= 0 ? g.scenep->Entities()[act].label.c_str() : "-",
                          g.scenep->TurnOrder().size());
        } else {
            std::snprintf(buf, sizeof(buf), "REAL-TIME (continuous turn-based)");
        }
        game::HudText(g.fb.data(), g.W, g.H, 6, y, buf, 255, 220, 120);
        y += 10;
        const int32_t sel = g.scenep->Selected();
        if (sel >= 0 && sel < (int32_t)g.scenep->Entities().size()) {
            const LiveEntity& e = g.scenep->Entities()[sel];
            std::snprintf(buf, sizeof(buf), "%s  HP %d/%d  AP %.1f/%d  %s %s  ammo %d",
                          e.label.c_str(), e.hp, e.maxHp, e.ap, e.maxAp,
                          e.weapon.valid ? e.weapon.displayName.c_str() : "-",
                          (e.weapon.Mode() && !e.weapon.Mode()->modeName.empty())
                              ? e.weapon.Mode()->modeName.c_str() : "",
                          e.ammo);
            game::HudText(g.fb.data(), g.W, g.H, 6, y, buf, 160, 255, 160);
            y += 10;
        }
        for (const std::string& l : g.hud) {
            game::HudText(g.fb.data(), g.W, g.H, 6, y, l.substr(0, 118));
            y += 9;
        }
    }

    // MILESTONE 9: the interface, over the finished world frame.
    if (g.uiOn && g.ui.Ready()) {
        RunUiScript();
        g.ui.Draw(g.scenep, g.fb.data(), g.W, g.H);
    }

    // MILESTONE 14: one line per RENDERED frame, so a script can assert that
    // the per-frame screen delta is uniform instead of zero-then-jump.
    if (!gTraceScreenIds.empty()) {
        for (int32_t id : gTraceScreenIds) {
            int32_t sx = 0, sy = 0;
            if (g.scenep->EntityScreen(id, &sx, &sy)) {
                std::printf("screen frame %lld tick %lld ent %d at %d,%d cam %lld,%lld\n",
                            (long long)g.frameNo, (long long)g.scenep->Ticks(), id, sx, sy,
                            (long long)g.camX, (long long)g.camY);
            }
        }
    }

    if (g.traceEvery > 0 && g.frameNo % g.traceEvery == 0) {
        const std::string t = g.scenep->TraceMovement();
        if (!t.empty()) {
            std::printf("--- move, frame %lld (tick %lld) ---\n%s", (long long)g.frameNo,
                        (long long)g.scenep->Ticks(), t.c_str());
        }
    }

    if (g.shotEvery > 0) {
        const int64_t t = g.scenep->Ticks();
        if (t / g.shotEvery != g.lastShotTick) {
            g.lastShotTick = t / g.shotEvery;
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s%03lld.png", g.shotPrefix.c_str(),
                          (long long)(t / g.shotEvery));
            SaveShot(buf);
        }
    }

    for (const std::string& s : g.shotAt) {
        const size_t c = s.find(':');
        if (c != std::string::npos && std::atoll(s.c_str()) == g.frameNo) {
            SaveShot(s.substr(c + 1));
        }
    }

    PresentFrame(deltaMs);
}

void PresentFrame(int64_t deltaMs) {
    if (!g.headless) {
        SDL_UpdateTexture(g.texture, nullptr, g.fb.data(), g.W * 4);
        SDL_RenderClear(g.renderer);
        SDL_RenderCopy(g.renderer, g.texture, nullptr, nullptr);
        SDL_RenderPresent(g.renderer);
    }

    g.fpsAccum += deltaMs / 1000.0;
    ++g.fpsFrames;
    if (g.fpsAccum >= 1.0) {
        g.fps = g.fpsFrames / g.fpsAccum;
        if (g.showFps) {
            std::printf("fps %.1f  (%zu tiles, %zu entities, %zu sorted)\n", g.fps,
                        g.lastStats.tilesDrawn, g.lastStats.entitiesDrawn,
                        g.lastStats.sortElems);
        }
        g.fpsAccum = 0;
        g.fpsFrames = 0;
    }

    ++g.frameNo;
    if (g.stopAfter >= 0 && g.frameNo >= g.stopAfter) {
        if (!g.shot.empty()) {
            SaveShot(g.shot);
        }
        g.running = false;
    }
}

#ifdef __EMSCRIPTEN__
void EmFrame() {
    Tick();
    if (!g.running) {
        emscripten_cancel_main_loop();
    }
}
#endif

void Usage() {
    std::fprintf(stderr,
                 "usage: fotgame <file.mis> [options]\n"
                 "       fotgame --campaign [--assets <core>] [--save-dir <d>] [options]\n"
                 "       fotgame            with nothing else: find the game (or ask for\n"
                 "                          it) and start the campaign, as a double-click does\n"
                 "  --help                                 this text\n"
                 "  --tiles <d> --sprites <d> --ents <d>   asset roots (default %s)\n"
                 "  --assets <d>                           all three at once\n"
                 "  --image-dump <rel>[:<out.png>]         load one image through the\n"
                 "                                         format sniff and report it\n"
                 "  --archive <f.bos>                      mount one more archive\n"
                 "  --resources-verbose                    list every mount\n"
                 "  --inside x,z | x,y,z                   roof-fade reference point\n"
                 "  --time HH:MM                           world clock\n"
                 "  --inside-k K                           roof-fade radius scale\n"
                 "  --centre x,z | x,y,z                   start the camera here\n"
                 "  --size w,h                             window size (default 1024x768)\n"
                 "  --frames N                             run N ticks then exit\n"
                 "  --screenshot <p.png>                   dump the last frame\n"
                 "  --shot-at N:<p.png>                    dump frame N (repeatable)\n"
                 "  --click N:x,y                          synthetic click at frame N\n"
                 "  --scroll N:dx,dy                       synthetic scroll at frame N\n"
                 "  --bench N                              N frames, scrolling, then fps\n"
                 "  --headless                             no window, fixed 33 ms steps\n"
                 "  --no-interp                            draw at the tick-exact position\n"
                 "  --trace-screen i[,j...]                per-frame screen anchor of those\n"
                 "\n  -- milestone 9 --\n"
                 "  --gui <d>                              the gui/ + locale/ root\n"
                 "  --no-ui                                do not draw the interface\n"
                 "  --ui-screen N:<name>                   open inventory|character|pipboy|\n"
                 "                                         skilldex|menu|none\n"
                 "  --ui-action N:<keys.cfg action>        as if the key were pressed\n"
                 "  --ui-hover N:x,y                       move the pointer\n"
                 "  --ui-click N:x,y                       press and release\n"
                 "  --ui-press N:x,y / --ui-release N:x,y  the two halves, for a\n"
                 "                                         screenshot mid-drag\n"
                 "  --ui-drag N:x0,y0,x1,y1                a synthetic item drag\n"
                 "  --fps                                  print fps once a second\n"
                 "  --trace                                dump animation state per screenshot\n"
                 "  --manifest <f>                         list every asset file opened, exit\n"
                 "  --squad <e1,e2,..>                     campaign recruits at the SpawnPoint\n"
                 "  --campaign-start                       drop the map's squad placeholders\n"
                 "  --issue / --no-issue                   quartermaster's first issue to the\n"
                 "                                         squad (default: on with --campaign-start)\n"
                 "  --use N:i                              the selection uses entity i\n"
                 "\n  -- milestone 18 --\n"
                 "  --talk N:i                             talk to entity i (order 0x17)\n"
                 "  --skill N:<name>[,i]                   use a skilldex skill (order 0x16)\n"
                 "  --run-mode 0|1|2                       bos.cfg game.runMode: walk/smart/run\n"
                 "  --give N:i,<ent path>[,count]          put an item in i's pack   [test]\n"
                 "  --hurt N:i,<hp>                        set i's hit points        [test]\n"
                 "  --cursor N:i | N:x,y                   name the cursor over an entity\n"
                 "  --loot-take N:i / --loot-put N:i       move a row between the loot halves\n"
                 "  --loot-all N:1  --loot-close N:1       TAKE ALL / close the loot window\n"
                 "  --loot-dump                            containers, corpses, ground items\n"
                 "  --entity-dump [substr]                 the raw <entity_file> slot table\n"
                 "  --select N:i                           select entity i at frame N\n"
                 "  --move N:x,z                           order the selection to world (x,z)\n"
                 "  --run N:x,z                            the same, running\n"
                 "  --posture N:Stand|Crouch|Prone         stance change at frame N\n"
                 "  --path                                 overlay the computed path\n"
                 "  --nav-png <p.png>                      dump the navigation grid\n"
                 "  --follow                               keep the selection centred\n"
                 "\n  -- milestone 10 --\n"
                 "  --triggers                             dump the mission script and exit\n"
                 "\n  -- milestone 11 --\n"
                 "  --ai-dump                              dump the parsed AI layer and exit\n"
                 "  --difficulty 0..4|easy|normal|hard     `{game.difficulty}` from bos.cfg\n"
                 "                                         (0 easy .. 4, 1 is the default)\n"
                 "  --weapon-dump [substr]                 every weapon .ent: Display Name,\n"
                 "                                         damage band, ammo and modes\n"
                 "  --ent-props <path>                     every property of one .ent\n"                 "  --entities [--stats]                   dump every combatant as an ENT record\n"
                 "                                         (difftest/compare.py diffs these against\n"
                 "                                         the original's own ENT records)\n"
                 "  --trace-ai                             AI state per screenshot / at exit\n"
                 "  --trace-detect                         log every detection with its terms\n"
                 "  --sentry N:i,off|scout|on              set a squad member's sentry mode\n"
                 "  --posture-at N:i,Stand|Crouch|Prone    stance for entity i at frame N\n"
                 "  --script-log                           echo the trigger engine's log\n"
                 "  --no-script                            do not run the trigger engine\n"
                 "  --speech N:<tag>=<node>                force a speech node\n"
                 "  --kill N:<tag>                         ActionKillEntity on a tagged unit\n"
                 "  --setvar N:<name>=<value>              set a mission variable\n"
                 "  --teleport N:<tag>,x,z                 put a tagged unit down    [test]\n"
                 "  --trace-move                           dump movement state per screenshot\n"
                 "\n  -- milestone 8 --\n"
                 "  --attack N:i                           the selection attacks entity i\n"
                 "  --mode N:single|burst|swing|...        pick the active weapon mode\n"
                 "  --turnbased N:on|off                   toggle turn-based mode\n"
                 "  --endturn N:1                          end the active turn\n"
                 "  --seed S / --seed-at N:S               the combat RNG seed\n"
                 "  --trace-combat                         dump every combatant per frame\n"
                 "  --no-ai                                stop the enemies acting\n"
                 "  --hud                                  draw the combat console on the frame\n"
                 "  --place N:i,x,z                        put entity i down at world (x,z)\n"
                 "  --hittest N:target,count               roll `count` attacks, report the rate\n"
                 "  --trace-every N                        dump movement state every N frames\n"
                 "  --shot-every T:<prefix>                a PNG every T ticks\n"
                 "  --lights on|off  --light-base clock|map|white  --markers\n"
                 "\n  -- milestone 13 --\n"
                 "  --no-sound                             do not open the mixer\n"
                 "  --audio-log                            one line per sound event\n"
                 "  --sprite-manifest <f>                  sequences drawn, at exit\n"
                 "  --audio-dump <p.wav>                   mix offline, write the result\n"
                 "  --audio-volumes m,mu,sfx,sp            the four bos.cfg buses, 0..100\n"
                 "\n  -- milestone 16 --\n"
                 "  --click N:x,y[,shift][,ctrl][,right]   a real click, through the mouse path\n"
                 "  --band N:x0,y0,x1,y1                   a band-box selection\n"
                 "\nMOUSE AND KEYS -- the shipped game's own, from locale/gui/help.txt\n"
                 "and data/core/keys.cfg:\n"
                 "  left click on ground        move the selection (walk)\n"
                 "  shift + left click          run there\n"
                 "  ctrl + left click           force move to the same level\n"
                 "  left click on a squad member  select just him\n"
                 "  shift + left click on him     add / remove him (keys.cfg modSelect1..6)\n"
                 "  left click on an enemy      fire the active weapon at him\n"
                 "  right click                 fire (ctrl-right = force fire)\n"
                 "  left drag (over 15 px)      band-select every member inside the box\n"
                 "  click a game-bar squad tab  select that member\n"
                 "  double-click a squad tab    centre the view on him   [reconstruction]\n"
                 "  F1-F6 / shift+F1-F6         select / add member 1-6\n"
                 "  F12                         select the whole squad\n"
                 "  F7-F11 / ctrl+F7-F11        recall / store a selection grouping\n"
                 "  space                       next member, and centre on him\n"
                 "  arrows, screen edge         scroll        escape  menu\n"
                 "  I S C P M                   inventory skilldex character pipboy map\n"
                 "  Q A Z  W  R  Y  U  back     stand crouch prone, stop, reload, weapon\n"
                 "                              mode, swap hands, end turn\n"
                 "  , . /                       sentry off / scout / on   <alt>X  quit\n"
                 "  1-8                         skilldex skills: sneak lockpick steal traps\n"
                 "                              firstAid doctor science repair  (right click\n"
                 "                              cancels an armed skill cursor)\n"
                 "  left click on a villager    talk to him (he needs a Click Speech node)\n"
                 "  the run rule                bos.cfg {game.runMode}: 0 always walk,\n"
                 "                              1 smart run, 2 always run -- shift inverts\n",
                 kDefaultAssets);
}

}  // namespace

// The engine as the command line drives it. main(), at the end of the file,
// calls it directly when there are options, and with the campaign options
// launch.h works out when there are none.
static int RunGame(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mis;
    SceneOptions opt;
    opt.tileDir = opt.spriteDir = opt.entDir = kDefaultAssets;
    int64_t centre[3] = {0, 0, 0};
    bool haveCentre = false;
    int64_t size[2] = {1024, 768};
    std::string manifest;
    std::string navPng;
    std::vector<std::string> randMove;
    bool farthest = false;
    uint32_t combatSeed = 1;
    bool noAi = false;
    bool noScript = false;
    bool dumpTriggers = false;
    bool dumpEntities = false;
    bool dumpLoot = false;
    // MILESTONE 17.  -1 "not said": on with --campaign-start, off otherwise.
    int issueEquipment = -1;
    std::string dumpEntityFilter;
    bool dumpAi = false;
    // MILESTONE 19: `--entities` dumps one stat block per live combatant in
    // the same `ENT` record format the FTSE logger writes for the original, so
    // difftest/compare.py can diff the two directly.  `--stats` adds the
    // SPECIAL and skill columns.  (`--entity-dump` above is a different thing:
    // the raw `<entity_file>` slot table.)
    bool dumpEntStats = false;
    bool dumpStats = false;
    // MILESTONE 19: `--hitcalc <attacker>,<target>,<weaponEnt>,<gap_m>[,<mode>]`
    // answers one to-hit question with THIS port's own formula, for an
    // arbitrary pair out of the loaded mission at an arbitrary range.  It is
    // what difftest/compare.py replays a HIT record from the original with:
    // the record names the attacker, the target, the weapon and the gap, and
    // the reply carries every term so the offending one is visible.
    // Repeatable; each one prints a HITCALC line and the run then exits.
    std::vector<std::string> hitCalcs;
    // MILESTONE 23 (difftest): `--weapon-dump [substr]` and `--ent-props <path>`.
    // The session logger writes a weapon's DISPLAY name ("Pump-action
    // Shotgun"), and joining that back to a `.ent` by guessing at the file
    // stem is what broke `compare.py`: `switchBlade.ent` is keyed
    // `name_switchblade` and `berettaShotgun.ent` is keyed
    // `name_berrettaShotgun` -- one differs in case, the other is a
    // misspelling in the shipped locale.  The `.ent`'s own `Display Name` IS
    // that locale key, so dumping it here makes the map exact instead of
    // heuristic, and carries the damage band and every mode alongside.
    // `GetDifficultyModifierForAttackerTarget`'s multiplier: easy 1.05 (+2 to
    // every shot the player's squad takes), normal 1.0, hard 0.95 (-2).  The
    // session logs do not record which one was set, so it is a flag and the
    // residual in `difftest/compare.py` is what estimates it.
    std::string difficulty = "normal";
    std::string weaponDump;
    bool weaponDumpAll = false;
    std::vector<std::string> entProps;
    bool traceDetect = false;
    bool vehicleCensus = false;
    // MILESTONE 20: whether the two `bos.cfg` game options came from the
    // command line, in which case the file must not overwrite them.
    bool runModeFromFlag = false;
    const bool formationFromFlag = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs an argument\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        } else if (a == "--tiles") {
            opt.tileDir = next("--tiles");
        } else if (a == "--sprites") {
            opt.spriteDir = next("--sprites");
        } else if (a == "--ents") {
            opt.entDir = next("--ents");
        } else if (a == "--assets") {
            opt.tileDir = opt.spriteDir = opt.entDir = next("--assets");
            g.assetsDir = opt.entDir;
        } else if (a == "--inside") {
            const std::string v = next("--inside");
            int64_t m[3] = {0, 0, 0};
            if (ParseInts(v.c_str(), 3, m)) {
                opt.inside[0] = m[0];
                opt.inside[1] = m[1];
                opt.inside[2] = m[2];
            } else if (ParseInts(v.c_str(), 2, m)) {
                opt.inside[0] = m[0];
                opt.inside[2] = m[1];
                opt.inside[1] = INT64_MIN;
            } else {
                std::fprintf(stderr, "--inside wants x,z or x,y,z\n");
                return 2;
            }
            opt.haveInside = true;
        } else if (a == "--inside-k") {
            opt.insideK = std::atof(next("--inside-k").c_str());
        } else if (a == "--time") {
            int hh = 0, mm = 0;
            if (std::sscanf(next("--time").c_str(), "%d:%d", &hh, &mm) != 2) {
                std::fprintf(stderr, "--time wants HH:MM\n");
                return 2;
            }
            opt.haveTime = true;
            opt.timeHour = hh;
            opt.timeMinute = mm;
        } else if (a == "--lights") {
            opt.lights = next("--lights") != "off";
        } else if (a == "--light-base") {
            const std::string v = next("--light-base");
            if (v == "map") {
                opt.lightBase = LightBase::kMap;
            } else if (v == "white" || v == "game") {
                opt.lightBase = LightBase::kWhite;
            } else if (v == "clock") {
                opt.lightBase = LightBase::kClock;
            } else {
                std::fprintf(stderr, "--light-base wants clock, map or white\n");
                return 2;
            }
        } else if (a == "--markers") {
            opt.drawMarkers = true;
        } else if (a == "--centre" || a == "--center") {
            const std::string v = next("--centre");
            if (!ParseInts(v.c_str(), 3, centre)) {
                int64_t m[2];
                if (!ParseInts(v.c_str(), 2, m)) {
                    std::fprintf(stderr, "--centre wants x,z or x,y,z\n");
                    return 2;
                }
                centre[0] = m[0];
                centre[1] = 0;
                centre[2] = m[1];
            }
            haveCentre = true;
        } else if (a == "--size") {
            if (!ParseInts(next("--size").c_str(), 2, size)) {
                return 2;
            }
        } else if (a == "--frames") {
            g.stopAfter = std::atoll(next("--frames").c_str());
            g.headless = true;
        } else if (a == "--screenshot") {
            g.shot = next("--screenshot");
        } else if (a == "--shot-at") {
            g.shotAt.push_back(next("--shot-at"));
        } else if (a == "--click") {
            g.script.push_back(std::string("") + next("--click"));
            g.script.back().insert(g.script.back().find(':') + 1, "click:");
        } else if (a == "--band") {
            g.script.push_back(std::string("") + next("--band"));
            g.script.back().insert(g.script.back().find(':') + 1, "band:");
        } else if (a == "--scroll") {
            g.script.push_back(std::string("") + next("--scroll"));
            g.script.back().insert(g.script.back().find(':') + 1, "scroll:");
        } else if (a == "--headless") {
            g.headless = true;
        } else if (a == "--manifest") {
            manifest = next("--manifest");
        } else if (a == "--bench") {
            g.bench = true;
            g.headless = true;
            g.stopAfter = std::atoll(next("--bench").c_str());
        } else if (a == "--squad") {
            std::string v = next("--squad");
            size_t p0 = 0;
            while (p0 <= v.size()) {
                const size_t c = v.find(',', p0);
                const std::string one = v.substr(p0, c - p0);
                if (!one.empty()) opt.squad.push_back(one);
                if (c == std::string::npos) break;
                p0 = c + 1;
            }
        } else if (a == "--campaign-start") {
            opt.campaignStart = true;
        } else if (a == "--issue") {
            issueEquipment = 1;
        } else if (a == "--no-issue") {
            issueEquipment = 0;
        } else if (a == "--attack" || a == "--mode" || a == "--turnbased" ||
                   a == "--endturn" || a == "--seed-at" || a == "--place" ||
                   a == "--hittest" || a == "--use" || a == "--loot-take" ||
                   a == "--loot-put" || a == "--loot-all" || a == "--loot-close") {
            const std::string v = next(a.c_str());
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "%s wants N:<arg>\n", a.c_str());
                return 2;
            }
            const std::string verb = (a == "--seed-at") ? "seed" : a.substr(2);
            g.orders.push_back(v.substr(0, c + 1) + verb + ":" + v.substr(c + 1));
        } else if (a == "--talk" || a == "--skill" || a == "--give" || a == "--hurt" ||
                   a == "--cursor") {
            const std::string v = next(a.c_str());
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "%s wants N:<arg>\n", a.c_str());
                return 2;
            }
            g.orders.push_back(v.substr(0, c + 1) + a.substr(2) + ":" + v.substr(c + 1));
        } else if (a == "--run-mode") {
            g.runMode = std::atoi(next("--run-mode").c_str());
            runModeFromFlag = true;
        } else if (a == "--kill" || a == "--speech" || a == "--setvar" || a == "--teleport") {
            const std::string v = next(a.c_str());
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "%s wants N:<arg>\n", a.c_str());
                return 2;
            }
            g.orders.push_back(v.substr(0, c + 1) + a.substr(2) + ":" + v.substr(c + 1));
        } else if (a == "--difficulty") {
            difficulty = next("--difficulty");
        } else if (a == "--weapon-dump") {
            weaponDumpAll = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') weaponDump = argv[++i];
        } else if (a == "--ent-props") {
            entProps.push_back(next("--ent-props"));
        } else if (a == "--hitcalc") {
            hitCalcs.push_back(next("--hitcalc"));
        } else if (a == "--entities") {
            dumpEntStats = true;
        } else if (a == "--stats") {
            dumpStats = true;
        } else if (a == "--ai-dump") {
            dumpAi = true;
        } else if (a == "--trace-ai") {
            g.traceAi = true;
        } else if (a == "--trace-detect") {
            traceDetect = true;
        } else if (a == "--vehicle") {
            const std::string v2 = next("--vehicle");
            const size_t c = v2.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "--vehicle wants N:<verb>:<args>\n");
                return 2;
            }
            g.orders.push_back(v2);
        } else if (a == "--vehicle-census") {
            vehicleCensus = true;
        } else if (a == "--sentry" || a == "--posture-at") {
            const std::string v = next(a.c_str());
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "%s wants N:<arg>\n", a.c_str());
                return 2;
            }
            g.orders.push_back(v.substr(0, c + 1) + a.substr(2) + ":" + v.substr(c + 1));
        } else if (a == "--triggers") {
            dumpTriggers = true;
        } else if (a == "--loot-dump") {
            dumpLoot = true;
        } else if (a == "--entity-dump") {
            dumpEntities = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') dumpEntityFilter = argv[++i];
        } else if (a == "--campaign") {
            g.campaignMode = true;
        } else if (a == "--image-dump") {
            g.imageDump.push_back(next("--image-dump"));
        } else if (a == "--campaign-dump") {
            g.campaignMode = true;
            g.camDump = true;
        } else if (a == "--resource-manifest") {
            // MILESTONE 15: every resource the run resolved, written at exit.
            // `scripts/make-fot-fixture.py` turns that list into small archives.
            g.resourceManifest = next("--resource-manifest");
            ft::VfsSetAccessLog(true);
        } else if (a == "--sprite-manifest") {
            // MILESTONE 20: every sprite SEQUENCE the run actually drew,
            // written at exit.  `sprtrim --keep <that file>` rewrites the
            // character `.spr` files down to just those animations; see
            // sprtrim/trim.h for why the keep-set is sequence names and not
            // collection names.
            g.spriteManifest = next("--sprite-manifest");
            ft::SprSetUsageLog(true);
        } else if (a == "--archive") {
            // MILESTONE 15: mount one more `.bos` from outside --assets.
            g.extraArchives.push_back(next("--archive"));
        } else if (a == "--resources-verbose") {
            g.verboseResources = true;
        } else if (a == "--save-dir") {
            g.saveDir = next("--save-dir");
        } else if (a == "--cam") {
            g.camScript.push_back(next("--cam"));
        } else if (a == "--script-log") {
            g.scriptLog = true;
        } else if (a == "--no-script") {
            noScript = true;
        } else if (a == "--seed") {
            combatSeed = static_cast<uint32_t>(std::atoll(next("--seed").c_str()));
        } else if (a == "--hud") {
            g.showHud = true;
        } else if (a == "--no-sound") {
            g.audioOn = false;
        } else if (a == "--audio-log") {
            g.audioLog = true;
        } else if (a == "--audio-dump") {
            g.audioDump = next("--audio-dump");
        } else if (a == "--audio-volumes") {
            // "master,music,sfx,speech", each 0..100 -- bos.cfg's four buses,
            // so a verification run can isolate one of them.
            const std::string v = next("--audio-volumes");
            int b[4] = {100, 60, 100, 100};
            std::sscanf(v.c_str(), "%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3]);
            g.audioVolumes.master = b[0];
            g.audioVolumes.music = b[1];
            g.audioVolumes.sfx = b[2];
            g.audioVolumes.speech = b[3];
            g.haveAudioVolumes = true;
        } else if (a == "--trace-combat") {
            g.traceCombat = true;
            if (g.traceEvery == 0) g.traceEvery = 1;
        } else if (a == "--no-ai") {
            noAi = true;
        } else if (a == "--select" || a == "--move" || a == "--run" || a == "--posture") {
            const std::string v = next(a.c_str());
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "%s wants N:<arg>\n", a.c_str());
                return 2;
            }
            g.orders.push_back(v.substr(0, c + 1) + a.substr(2) + ":" + v.substr(c + 1));
        } else if (a == "--shot-every") {
            const std::string v = next("--shot-every");
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "--shot-every wants T:<prefix>\n");
                return 2;
            }
            g.shotEvery = std::atoll(v.c_str());
            g.shotPrefix = v.substr(c + 1);
        } else if (a == "--farthest") {
            farthest = true;
        } else if (a == "--rand-move") {
            // MILESTONE 13 (verification only): switch a player controller's
            // `Rand Move` on.  "all" or a player index.
            randMove.push_back(next("--rand-move"));
        } else if (a == "--nav-png") {
            navPng = next("--nav-png");
        } else if (a == "--trace-every") {
            g.traceEvery = std::atoll(next("--trace-every").c_str());
            gTraceMove = true;
        } else if (a == "--follow") {
            g.follow = true;
        } else if (a == "--path") {
            g.drawPath = true;
        } else if (a == "--trace-move") {
            gTraceMove = true;
        } else if (a == "--no-interp") {
            // MILESTONE 14: draw at the tick-exact position, i.e. the
            // behaviour the binary has.  Verification only.
            gInterp = false;
        } else if (a == "--trace-screen") {
            // MILESTONE 14: `i` or `i,j,...` -- print the screen anchor of
            // those entities on EVERY rendered frame, which is what the
            // smoothness check samples.
            gTraceScreen = next("--trace-screen");
        } else if (a == "--trace") {
            gTrace = true;
        } else if (a == "--fps") {
            g.showFps = true;
        } else if (a == "--gui") {
            g.guiDir = next("--gui");
        } else if (a == "--no-ui") {
            g.uiOn = false;
        } else if (a == "--ui-screen" || a == "--ui-click" || a == "--ui-hover" ||
                   a == "--ui-drag" || a == "--ui-action" || a == "--ui-press" ||
                   a == "--ui-release") {
            const std::string v = next(a.c_str());
            const size_t c = v.find(':');
            if (c == std::string::npos) {
                std::fprintf(stderr, "%s wants N:<arg>\n", a.c_str());
                return 2;
            }
            g.uiScript.push_back(v.substr(0, c + 1) + a.substr(5) + ":" + v.substr(c + 1));
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else {
            mis = a;
        }
    }
    if (mis.empty() && !g.campaignMode && !vehicleCensus && g.imageDump.empty() &&
        !weaponDumpAll && entProps.empty()) {
        Usage();
        return 2;
    }
    if (g.assetsDir.empty()) g.assetsDir = opt.entDir;

    // MILESTONE 15: the resource layer.  `--assets <dir>` now means "mount
    // everything under here": every `*.bos` in ascending name order (so a
    // per-CD archive patches the base) and then the directory itself as a loose
    // tree, which wins over both.  A fully extracted tree therefore behaves
    // exactly as it did before -- nothing is mounted as an archive, the loose
    // walk indexes it, and every index below is built from that one walk
    // instead of three separate ones.
    //
    // This is what lets the browser lane work: it mounts the player's own
    // retail `core/*.bos` out of the vault and never extracts a byte.
    {
        std::string mountErr;
        const size_t archives = ft::VfsMountAssetRoot(g.assetsDir, &mountErr);
        for (const std::string& extra : g.extraArchives) {
            std::string e;
            if (!ft::VfsMountArchive(extra, &e)) {
                if (!mountErr.empty()) mountErr += "; ";
                mountErr += e;
            }
        }
        if (!mountErr.empty()) std::fprintf(stderr, "resources: %s\n", mountErr.c_str());
        if (ft::VfsMounted()) {
            std::printf("resources: %zu files from %zu archive(s) + loose tree under %s\n",
                        ft::VfsSize(), archives, g.assetsDir.c_str());
            if (g.verboseResources) {
                for (const std::string& line : ft::VfsMountReport()) {
                    std::printf("resources:   %s\n", line.c_str());
                }
            }
        }
    }

    // MILESTONE 23 (difftest): the two data dumps.  Both need the resource
    // layer and nothing else -- no mission, no campaign -- so they run here and
    // exit.  `--weapon-dump` is what `difftest/compare.py` builds its display
    // name -> `.ent` map from, and what settled the shotgun band (see
    // NOTES.md).
    if (weaponDumpAll || !entProps.empty()) {
        ft::PathIndex index(opt.entDir, "entities/", ".ent");
        ft::EntProtoCache cache(&index);
        auto dumpEsh = [&](const ft::Esh& esh, const std::string& prefix,
                           auto&& self) -> void {
            for (const ft::EshProp& pr : esh.props) {
                std::string sv;
                int32_t iv = 0;
                float fv = 0;
                bool bv = false;
                ft::Esh nested;
                std::string nerr;
                if (esh.GetNested(pr.name, &nested, &nerr)) {
                    std::printf("PROP %s%s = <nested %zu>\n", prefix.c_str(),
                                pr.name.c_str(), nested.props.size());
                    self(nested, prefix + pr.name + "/", self);
                } else if (esh.GetString(pr.name, &sv)) {
                    std::printf("PROP %s%s = \"%s\"\n", prefix.c_str(), pr.name.c_str(),
                                sv.c_str());
                } else if (esh.GetBool(pr.name, &bv)) {
                    std::printf("PROP %s%s = %s\n", prefix.c_str(), pr.name.c_str(),
                                bv ? "true" : "false");
                } else if (esh.GetInt(pr.name, &iv)) {
                    std::printf("PROP %s%s = %d\n", prefix.c_str(), pr.name.c_str(), iv);
                } else if (esh.GetFloat(pr.name, &fv)) {
                    std::printf("PROP %s%s = %g\n", prefix.c_str(), pr.name.c_str(), fv);
                } else {
                    std::printf("PROP %s%s = <type %u, %zu bytes>\n", prefix.c_str(),
                                pr.name.c_str(), pr.type, pr.payload.size());
                }
            }
        };
        for (const std::string& path : entProps) {
            std::string perr;
            const ft::EntProto* proto = cache.Get(path, &perr);
            if (proto == nullptr) {
                std::printf("ENTPROPS %s error=%s\n", path.c_str(), perr.c_str());
                continue;
            }
            std::printf("ENTPROPS %s class=%s props=%zu\n", path.c_str(),
                        proto->block.className.c_str(), proto->block.esh.props.size());
            dumpEsh(proto->block.esh, "", dumpEsh);
        }
        if (weaponDumpAll) {
            std::vector<std::string> paths;
            ft::VfsEnumerate("entities/", ".ent",
                             [&](const std::string& key) { paths.push_back(key); });
            std::sort(paths.begin(), paths.end());
            for (const std::string& path : paths) {
                if (!weaponDump.empty() &&
                    path.find(weaponDump) == std::string::npos) {
                    continue;
                }
                std::string perr;
                const ft::EntProto* proto = cache.Get(path, &perr);
                if (proto == nullptr) continue;
                game::WeaponDef w;
                if (!game::ReadWeapon(proto->block.esh, &w) || !w.valid) continue;
                std::printf("WEAPON ent=%s display=%s type=%s min=%d max=%d "
                            "ammo=%s variant=%s crit=%d mag=%d modes=%zu",
                            path.c_str(),
                            w.displayName.empty() ? "-" : w.displayName.c_str(),
                            w.itemType.empty() ? "-" : w.itemType.c_str(), w.minDamage,
                            w.maxDamage, w.ammoType.empty() ? "-" : w.ammoType.c_str(),
                            w.ammoVariant.empty() ? "-" : w.ammoVariant.c_str(),
                            w.criticalChance, w.magazine, w.modes.size());
                for (size_t mi = 0; mi < w.modes.size(); ++mi) {
                    const game::WeaponMode& m = w.modes[mi];
                    if (!m.enabled) continue;
                    std::printf(" | mode%zu=%s skill=%s range=%s/%g dmgmult=%g "
                                "acc=%d shots=%d crit=%d",
                                mi, m.modeName.empty() ? "-" : m.modeName.c_str(),
                                m.weaponSkill.c_str(),
                                m.rangeType.empty() ? "-" : m.rangeType.c_str(),
                                m.maxRange, m.damageMult, m.accuracyBonus, m.ammoUsage,
                                m.critBonus);
                }
                std::printf("\n");
            }
        }
        return 0;
    }

    if (g.saveDir.empty()) {
        // Where the retail game keeps campaign saves: <core>/user/save.
        const std::string core = g.assetsDir.empty() ? std::string(kDefaultAssets) : g.assetsDir;
        g.saveDir = core + "/user/save";
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(g.saveDir, ec);
    }

    std::string err;
    g.camOpt = opt;
    // `--image-dump` needs the resource root and nothing else -- no map, no
    // campaign -- so it takes the same "there is no mission" path the campaign
    // shell does.
    const bool campaignOnly =
        (g.campaignMode || vehicleCensus || !g.imageDump.empty()) && mis.empty();
    // MILESTONE 18: `bos.cfg`'s two GAME options this port now honours --
    // `{game.runMode}` (0 walk / 1 smart / 2 run; see `G::runMode`) and
    // `{game.formationMode}` (`DAT_008bda1c`, which picks `cursor_moveFormation`
    // over `cursor_move`).  The file is `{key} = {value}` per line, which is
    // exactly what `Audio::LoadConfig` already reads.
    {
        std::vector<uint8_t> raw;
        std::string cerr;
        if (ft::ReadWholeFile(g.assetsDir + "/bos.cfg", &raw, &cerr)) {
            const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
            size_t p = 0;
            while (p < text.size()) {
                size_t eol = text.find('\n', p);
                if (eol == std::string::npos) eol = text.size();
                const std::string line = text.substr(p, eol - p);
                p = eol + 1;
                const size_t k0 = line.find('{'), k1 = line.find('}');
                if (k0 == std::string::npos || k1 == std::string::npos) continue;
                const size_t v0 = line.find('{', k1), v1 = line.find('}', v0 + 1);
                if (v0 == std::string::npos || v1 == std::string::npos) continue;
                const std::string key = line.substr(k0 + 1, k1 - k0 - 1);
                const int n = std::atoi(line.substr(v0 + 1, v1 - v0 - 1).c_str());
                // MILESTONE 20: the file is the DEFAULT, not an override --
                // `--run-mode` is parsed before this block runs, so without the
                // guard the shipped `{game.runMode} = {0}` silently undid the
                // flag and `--run-mode 1|2` did nothing against a retail tree.
                if (key == "game.runMode" && !runModeFromFlag) g.runMode = n;
                else if (key == "game.formationMode" && !formationFromFlag) g.formationMode = n;
            }
        }
        if (g.uiOn) g.ui.SetFormationMode(g.formationMode);
    }

    // MILESTONE 13: the mixer comes up before the world does, because the
    // entity build probes it for how many voice variants a sound set has.
    if (g.audioOn) {
        std::string aerr;
        g.audio.LoadConfig(g.assetsDir + "/bos.cfg");
        if (g.haveAudioVolumes) g.audio.SetVolumes(g.audioVolumes);
        g.audio.SetLog(g.audioLog);
        const bool dumping = !g.audioDump.empty();
        if (!g.audio.Init(g.assetsDir, !dumping && !g.headless, dumping, &aerr)) {
            std::fprintf(stderr, "audio: %s\n", aerr.c_str());
        } else {
            std::printf("audio: %s  master %d music %d sfx %d speech %d%s\n",
                        dumping ? "offline (--audio-dump)"
                                : (g.headless ? "offline (headless)" : "SDL2 device"),
                        g.audio.GetVolumes().master, g.audio.GetVolumes().music,
                        g.audio.GetVolumes().sfx, g.audio.GetVolumes().speech,
                        g.audioLog ? "  [--audio-log]" : "");
        }
    }
    g.sceneOwn.reset(new Scene());
    g.scenep = g.sceneOwn.get();
    g.scenep->SetAudio(g.audio.Enabled() ? &g.audio : nullptr);
    if (!campaignOnly && !g.scenep->Load(mis, opt, &err)) {
        std::fprintf(stderr, "%s: %s\n", mis.c_str(), err.c_str());
        return 1;
    }
    g.scenep->SeedRng(combatSeed);
    if (noAi) g.scenep->DisableAI();
    if (noScript) g.scenep->EnableScript(false);
    if (traceDetect) g.scenep->TraceDetection(true);
    if (dumpTriggers) {
        std::printf("%s\n", g.scenep->Script().Describe().c_str());
        return 0;
    }
    if (dumpEntities) {
        // MILESTONE 17: the raw `<entity_file>` slot table, class, path and
        // every property name/type.  The loot work needed it because the items
        // a container holds are SEPARATE SLOTS in that table, referenced by id
        // (`kEshEntityId`), and nothing before this milestone resolved them.
        const ft::EntityFile& ef = g.scenep->GetWorld().entityFile;
        std::printf("entity_file v%d: %zu slots, %zu live\n", ef.version,
                    static_cast<size_t>(ef.slotCount), ef.entities.size());
        for (const ft::EntityInstance& e : ef.entities) {
            if (!dumpEntityFilter.empty() &&
                e.className.find(dumpEntityFilter) == std::string::npos &&
                e.entPath.find(dumpEntityFilter) == std::string::npos) {
                continue;
            }
            std::printf("slot %4d id %5u/%5u  %-14s %-46s frame=%d world=(%d,%d,%d)\n", e.slot,
                        e.idA, e.idB, e.className.c_str(), e.entPath.c_str(), e.hasFrame ? 1 : 0,
                        e.world[0], e.world[1], e.world[2]);
            for (const ft::EshProp& p : e.esh.props) {
                std::string v;
                if (p.type == ft::kEshEntityId && p.payload.size() >= 2) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "id %u",
                                  static_cast<unsigned>(p.payload[0] | (p.payload[1] << 8)));
                    v = b;
                } else if (ft::EshTypeIsString(p.type) && p.payload.size() >= 4) {
                    const int32_t n = static_cast<int32_t>(
                        p.payload[0] | (p.payload[1] << 8) | (p.payload[2] << 16) |
                        (static_cast<uint32_t>(p.payload[3]) << 24));
                    if (n > 0 && static_cast<size_t>(n) + 4 <= p.payload.size()) {
                        v.assign(reinterpret_cast<const char*>(p.payload.data() + 4),
                                 static_cast<size_t>(n));
                        if (v.size() > 40) v = v.substr(0, 40) + "...";
                        for (char& c : v) if (c < 32) c = '.';
                    }
                } else if (p.type == ft::kEshInt && p.payload.size() >= 4) {
                    char b[24];
                    std::snprintf(b, sizeof(b), "%d",
                                  static_cast<int>(p.payload[0] | (p.payload[1] << 8) |
                                                   (p.payload[2] << 16) |
                                                   (static_cast<uint32_t>(p.payload[3]) << 24)));
                    v = b;
                } else if (p.type == ft::kEshBool && !p.payload.empty()) {
                    v = p.payload[0] ? "true" : "false";
                } else {
                    for (size_t bi = 0; bi < p.payload.size() && bi < 16; ++bi) {
                        char b[4];
                        std::snprintf(b, sizeof(b), "%02x", p.payload[bi]);
                        v += b;
                    }
                }
                std::printf("      %-28s t=%-3u %zu bytes  %s\n", p.name.c_str(), p.type,
                            p.payload.size(), v.c_str());
            }
        }
        return 0;
    }
    // MILESTONE 22.  `--hitcalc` and `--entities` both `return 0` before the
    // mission-start path below ever runs, so until now they replayed a squad
    // the quartermaster had not reached: Mick came out at AC 8 and with a
    // punch in his hand, which is neither what the original's session had nor
    // what `difftest/compare.py` is comparing against.  The issue runs here
    // too, under the same flag, so the two dumps see the squad the mission
    // would actually start with.
    if ((!hitCalcs.empty() || dumpEntStats) && !campaignOnly &&
        (issueEquipment == 1 || (issueEquipment != 0 && opt.campaignStart))) {
        IssueStartingEquipment();
        issueEquipment = 0;
    }
    {
        // MILESTONE 25.  `GetDifficultyModifierForAttackerTarget @0061b990`,
        // out of the binary at last.  It switches on
        // `WorldGlobal->unknown4 + 0x24` -- `{game.difficulty}` in `bos.cfg`,
        // 1 in the shipped file -- through a five-entry table, and then:
        // if the ATTACKER is a player character it returns `1 / m`, if the
        // TARGET is one it returns `m`, and otherwise 1.0.  (Its very first
        // line returns 1.0 outright when `WorldGlobal->flag_for_time` is set.)
        // `--difficulty` therefore takes the setting's own index; `easy`,
        // `normal` and `hard` are aliases for 0, 1 and 2, and a bare number is
        // still taken as the multiplier itself for experiments.
        static const float kDifficultyTable[5] = {0.7f, 1.0f, 1.3f, 2.0f, 5.0f};
        float dm = 1.0f;
        int idx = -1;
        if (difficulty == "easy") idx = 0;
        else if (difficulty == "normal") idx = 1;
        else if (difficulty == "hard") idx = 2;
        else if (difficulty.size() == 1 && difficulty[0] >= '0' && difficulty[0] <= '4')
            idx = difficulty[0] - '0';
        if (idx >= 0) {
            dm = 1.0f / kDifficultyTable[idx];
        } else {
            dm = static_cast<float>(std::atof(difficulty.c_str()));
        }
        if (dm <= 0.0f) dm = 1.0f;
        g.scenep->SetDifficultyMultiplier(dm);
    }
    if (!hitCalcs.empty()) {
        const std::vector<game::LiveEntity>& es = g.scenep->Entities();
        // an entity is named either by index ("#12") or by the name the
        // original's own log carries (its ShownName, e.g. "CORE_raider")
        auto findEntity = [&](const std::string& sel, int32_t skip) -> int32_t {
            if (!sel.empty() && sel[0] == '#') return std::atoi(sel.c_str() + 1);
            for (size_t i = 0; i < es.size(); ++i) {
                if (!es[i].isCombatant || static_cast<int32_t>(i) == skip) continue;
                // The original's log names an entity by its INTERNAL name --
                // the prototype's `Display Name`, "CORE_tribal" -- while
                // `ShownName()` is the localised one and falls back to the
                // mission tag, so a tagged actor ("Captive-1") could not be
                // found by the name the session actually carries.
                if (es[i].ShownName() == sel || es[i].label == sel ||
                    es[i].displayName == sel || es[i].tag == sel ||
                    es[i].entPath.find(sel) != std::string::npos)
                    return static_cast<int32_t>(i);
            }
            return -1;
        };
        for (const std::string& spec : hitCalcs) {
            std::vector<std::string> f;
            size_t p = 0;
            while (p <= spec.size()) {
                const size_t c = spec.find(',', p);
                f.push_back(spec.substr(p, c == std::string::npos ? c : c - p));
                if (c == std::string::npos) break;
                p = c + 1;
            }
            if (f.size() < 4) {
                std::printf("HITCALC spec=%s error=wants attacker,target,weapon,gap\n",
                            spec.c_str());
                continue;
            }
            const int32_t ai = findEntity(f[0], -1);
            const int32_t ti = findEntity(f[1], ai);
            const double gap = std::atof(f[3].c_str());
            if (ai < 0 || ti < 0) {
                std::printf("HITCALC spec=%s error=no such entity (attacker=%d target=%d)\n",
                            spec.c_str(), ai, ti);
                continue;
            }
            std::string why;
            if (!f[2].empty() && f[2] != "-") {
                if (!g.scenep->ForceEquipByPath(ai, f[2], &why))
                    std::printf("HITCALC spec=%s note=could not equip %s (%s)\n",
                                spec.c_str(), f[2].c_str(), why.c_str());
            }
            if (f.size() >= 5 && !f[4].empty() && f[4] != "-")
                g.scenep->SetWeaponMode(ai, f[4]);
            // put the target that many metres away: a world unit is a quarter
            // of a metre (mapview/NOTES.md 14)
            const game::LiveEntity& at = es[ai];
            // wpos is in metres; PlaceEntity takes world units, four to the
            // metre.  PlaceEntity snaps to a walkable cell, so try eight
            // directions and keep whichever lands closest to the gap asked for
            // -- otherwise a wall behind the target silently changes the range
            // the comparison is about.
            {
                // 32 bearings and a small ladder of radii, not 8 bearings at
                // one radius.  `PlaceEntity` snaps to a walkable cell, and with
                // eight tries a target asked for at 1.4 m routinely landed on
                // top of the attacker at 0.00 m, which changes the close-range
                // bonus by ten points and made the whole row a comparison of
                // two different geometries rather than of two formulas.
                double best = 1e9;
                int32_t bestX = 0, bestZ = 0;
                static const double kRadii[7] = {1.0, 1.15, 0.85, 1.3, 0.7, 1.5, 0.5};
                for (int ri = 0; ri < 7 && best > 0.15; ++ri) {
                    const double r = gap * kRadii[ri];
                    for (int k = 0; k < 32; ++k) {
                        const double th = 2.0 * 3.14159265358979 * k / 32.0;
                        const int32_t wx =
                            static_cast<int32_t>((at.wpos[0] + r * std::cos(th)) * 4.0);
                        const int32_t wz =
                            static_cast<int32_t>((at.wpos[2] + r * std::sin(th)) * 4.0);
                        if (!g.scenep->PlaceEntity(ti, wx, wz)) continue;
                        game::Scene::HitTerms probe;
                        int64_t h2 = 0, s2 = 0;
                        g.scenep->HitRateTest(ai, ti, 1, &probe, &h2, &s2);
                        const double err = std::fabs(probe.distance - gap);
                        if (err < best) { best = err; bestX = wx; bestZ = wz; }
                    }
                }
                if (best < 1e8) g.scenep->PlaceEntity(ti, bestX, bestZ);
            }
            game::Scene::HitTerms t;
            int64_t hits = 0, shots = 0;
            const bool ok = g.scenep->HitRateTest(ai, ti, 1, &t, &hits, &shots);
            // `t.distance` is the BOUNDING-BOX gap, which is what the to-hit
            // sum uses; the FTSE logger's `gap_m` is the CENTRE-to-centre
            // distance between the two `GetPosition`s.  Both are printed, so
            // `compare.py` can check that the replay stood where the record
            // says without confusing the two measurements -- two actors toe to
            // toe are 1.4 m apart by their centres and 0.08 m apart by their
            // boxes.
            const double cdx = es[ai].wpos[0] - es[ti].wpos[0];
            const double cdz = es[ai].wpos[2] - es[ti].wpos[2];
            const double centreGap = std::sqrt(cdx * cdx + cdz * cdz);
            // The band the original's `DMG` rolls are drawn from: the weapon's
            // field through the mode multiplier and the melee/ranged bonus,
            // THEN the ammunition's damage multiplier, which is where a
            // shotgun firing flechettes gets past its printed maximum.
            int32_t bandLo = 0, bandHi = 0;
            float ammoMult = 1.0f, ammoDrMod = 1.0f;
            // The cone multiplier reads the CENTRE-to-centre distance, and the
            // range the SPEC asked for is that distance: the FTSE logger's
            // `gap_m` is centre to centre, so a `DMG` record replayed here is
            // asking for the band at exactly `gap`.  Using the replay's own
            // `centreGap` instead would fold in `PlaceEntity`'s walkable-cell
            // snap, which is placement noise and not part of the formula, so
            // the band is reported at the requested range and `centre=` is
            // printed beside it for the geometry check.
            g.scenep->DamageBandOf(es[ai], &bandLo, &bandHi, &ammoMult, &ammoDrMod,
                                   static_cast<float>(gap));
            const int32_t rollLo = static_cast<int32_t>(static_cast<float>(bandLo) * ammoMult);
            const int32_t rollHi = static_cast<int32_t>(static_cast<float>(bandHi) * ammoMult);
            const std::string& av = es[ai].weapon.ammoVariant;
            const game::WeaponMode* mp = es[ai].weapon.Mode();
            const std::string modeName =
                (mp != nullptr && !mp->modeName.empty()) ? mp->modeName : std::string("-");
            std::printf("HITCALC spec=%s attacker=%s target=%s weapon=%s dist=%.2f "
                        "chance=%d skill=%d acc=%d rc=%d rangePen=%d close=%d ac=%d "
                        "dodge=%d cover=%d down=%d big=%d dmg_min=%d dmg_max=%d "
                        "wname=%s base_min=%d base_max=%d ammomult=%g ammo=%s "
                        "mode=%s reserve=%d diff=%d centre=%.2f%s%s\n",
                        spec.c_str(), es[ai].ShownName().c_str(),
                        es[ti].ShownName().c_str(),
                        es[ai].weapon.path.empty() ? "-" : es[ai].weapon.path.c_str(),
                        t.distance, t.total, t.skill, t.accuracyBonus, t.rangeClass,
                        t.rangePenalty, t.closeBonus, t.armourClass, t.meleeDodge,
                        t.coverPenalty, t.targetDown, t.bigTarget,
                        rollLo, rollHi,
                        es[ai].weapon.displayName.empty() ? "-"
                                                          : es[ai].weapon.displayName.c_str(),
                        es[ai].weapon.minDamage, es[ai].weapon.maxDamage, ammoMult,
                        av.empty() ? "-" : av.c_str(), modeName.c_str(),
                        es[ai].ammoReserve, t.difficulty, centreGap,
                        ok ? "" : " refused=", ok ? "" : t.reason.c_str());
        }
        return 0;
    }
    if (dumpEntStats) {
        auto Underscored = [](const std::string& v) {
            if (v.empty()) return std::string("-");
            std::string out = v;
            for (char& c : out) {
                if (c == ' ') c = '_';
            }
            return out;
        };
        // The same line shape as an ENT record from the original, so that a
        // diff is a field-by-field comparison and not a parsing exercise.
        const std::vector<game::LiveEntity>& es = g.scenep->Entities();
        for (size_t i = 0; i < es.size(); ++i) {
            const game::LiveEntity& e = es[i];
            if (!e.isCombatant) continue;
            // `internal` is the name the original's Entity:GetInternalName
            // returns (CORE_raider), which is what joins the two sides;
            // `ent` is the prototype path, which is what separates
            // raider_06_infbas from the leather-armoured raiders.
            // `internal` must be the PROTOTYPE's own Display Name and nothing
            // else: `ShownName()` falls back to the mission tag for a tagged
            // actor, so the elder, the captives and the guards used to be
            // published as "Keeper (K)" where the original publishes
            // "CORE_raider" with the tag in its own field, and every one of
            // them fell out of the join.  `race` is the `.ent`'s `Race Type`,
            // the field that separates a RaiderLarge from a Raider on the
            // original's side.
            std::printf("ENT index=%zu internal=%s tag=%s race=%s ent=%s class=%s "
                        "race_index=%d "
                        "hp=%d maxhp=%d hp_bonus=%d ac=%d ac_base=%d armour_ac=%d "
                        "ap=%d maxap=%d armour=%s hand_r=%s",
                        i, e.displayName.empty() ? "-" : e.displayName.c_str(),
                        // spaces collapse to '_', as the FTSE logger does, so
                        // the record stays one field per token
                        Underscored(e.label).c_str(),
                        e.raceType.empty() ? "-" : e.raceType.c_str(),
                        e.entPath.empty() ? "-" : e.entPath.c_str(),
                        e.className.empty() ? "-" : e.className.c_str(),
                        e.attr.raceIndex, e.hp, e.maxHp, e.attr.maxHitPointsBonus,
                        e.attr.armorClass, e.attr.armorClass - e.armour.armorClass,
                        e.armour.armorClass, static_cast<int>(e.ap), e.maxAp,
                        e.armour.path.empty() ? "-" : e.armour.path.c_str(),
                        e.weapon.path.empty() ? "-" : e.weapon.path.c_str());
            if (dumpStats) {
                // MILESTONE 25: the stored career XP and the level it buys.
                std::printf(" xp=%d level=%d", e.attr.experience,
                            game::LevelForExperience(e.attr.experience));
                std::printf(" special=%d,%d,%d,%d,%d,%d,%d",
                            e.attr.strength, e.attr.perception, e.attr.endurance,
                            e.attr.charisma, e.attr.intelligence, e.attr.agility,
                            e.attr.luck);
                std::printf(" skills=");
                for (int k = 0; k < game::kSkillCount; ++k)
                    std::printf("%s%d", k ? "," : "", e.attr.skills[k]);
            }
            std::printf("\n");
        }
        return 0;
    }
    if (dumpAi) {
        std::printf("%s\n", g.scenep->DescribeAI().c_str());
        return 0;
    }
    // MILESTONE 9: the interface.  Its resource root is the same extracted
    // tree the world uses; it needs `gui/` and `locale/`, which come out of
    // `data/core/gui_0.bos` and `data/core/locale_0.bos`.
    if (g.uiOn) {
        const std::string guiRoot = g.guiDir.empty() ? opt.entDir : g.guiDir;
        std::string uiErr;
        if (!g.ui.Init(guiRoot.empty() ? std::string(kDefaultAssets) : guiRoot, &uiErr)) {
            std::fprintf(stderr, "ui: %s\n", uiErr.c_str());
            g.uiOn = false;
        } else {
            std::printf("%s\n", g.ui.Info().c_str());
            // MILESTONE 10: the mission's own string table, out of
            // `loc-mis_<letter>.bos` (locale/missions/<mission>/*.txt).
            if (!campaignOnly) g.ui.LoadMissionStrings(mis);
            // The campaign's own location names live in
            // `locale/game/generic_mission.txt`, which `Locale::LoadDir`
            // already reads as part of `locale/game`.
        }
    }
    // MILESTONE 17: the quartermaster's first issue, on the mission-start path.
    if (!campaignOnly &&
        (issueEquipment == 1 || (issueEquipment != 0 && opt.campaignStart))) {
        IssueStartingEquipment();
    }
    if (!campaignOnly) MissionStarted();
    // MILESTONE 17.  `--loot-dump` writes the whole loot state -- every
    // container, corpse, ground item and actor with its inventory, its carried
    // weight and its `ST * 25` capacity.  It prints at EXIT, so a scripted run
    // can loot something first and dump the result; with no `--frames` of its
    // own it dumps the freshly loaded mission and stops.
    if (dumpLoot && g.stopAfter < 0) {
        g.stopAfter = 1;
        g.headless = true;
    }
    if (!campaignOnly) std::printf("%s\n%s\n", mis.c_str(), g.scenep->Info().c_str());
    if (!campaignOnly) {
        int32_t sp[3] = {0, 0, 0};
        if (g.scenep->SpawnPoint(sp)) {
            std::printf("spawn point         world (%d, %d, %d); squad %zu placed", sp[0], sp[1],
                        sp[2], g.scenep->Squad().size());
            for (int32_t i : g.scenep->Squad()) {
                std::printf(" [%d] %s @%d,%d,%d", i, g.scenep->Entities()[i].label.c_str(),
                            g.scenep->Entities()[i].world[0], g.scenep->Entities()[i].world[1],
                            g.scenep->Entities()[i].world[2]);
            }
            std::printf("\n");
        }
    }
    if (!manifest.empty()) {
        FILE* f = std::fopen(manifest.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", manifest.c_str());
            return 1;
        }
        for (const std::string& p : g.scenep->Manifest()) {
            std::fprintf(f, "%s\n", p.c_str());
        }
        std::fclose(f);
        std::printf("manifest: %zu files -> %s\n", g.scenep->Manifest().size(), manifest.c_str());
        return 0;
    }

    if (farthest) {
        // A debug aid for scripting a long walk: flood-fill the navigation grid
        // from the squad's spawn and report the reachable cell with the largest
        // step count.  Not part of the game.
        const NavGrid& n = g.scenep->Nav();
        int32_t sp[3] = {0, 0, 0};
        if (g.scenep->SpawnPoint(sp)) {
            std::vector<int32_t> dist(static_cast<size_t>(n.nx) * n.nz, -1);
            std::vector<int32_t> q;
            const int32_t s0 = n.CellX(sp[0]), s1 = n.CellZ(sp[2]);
            if (n.In(s0, s1)) {
                dist[n.Idx(s0, s1)] = 0;
                q.push_back(s1 * n.nx + s0);
            }
            size_t head = 0;
            int32_t bi = -1, bd = -1;
            while (head < q.size()) {
                const int32_t cur = q[head++];
                const int32_t cx = cur % n.nx, cz = cur / n.nx;
                const int32_t d0 = dist[static_cast<size_t>(cur)];
                if (d0 > bd) {
                    bd = d0;
                    bi = cur;
                }
                for (int32_t dz = -1; dz <= 1; ++dz) {
                    for (int32_t dx = -1; dx <= 1; ++dx) {
                        const int32_t tx = cx + dx, tz = cz + dz;
                        if ((dx == 0 && dz == 0) || !n.Open(tx, tz)) continue;
                        if (std::abs(n.Surface(tx, tz) - n.Surface(cx, cz)) > kStepUp) continue;
                        if (dist[n.Idx(tx, tz)] >= 0) continue;
                        dist[n.Idx(tx, tz)] = d0 + 1;
                        q.push_back(tz * n.nx + tx);
                    }
                }
            }
            std::printf("reachable from spawn: %zu cells; farthest is %d steps at world (%d, %d)\n",
                        q.size(), bd, n.CentreX(bi % n.nx), n.CentreZ(bi / n.nx));
        }
    }

    // MILESTONE 14.
    g.scenep->SetInterpolation(gInterp);
    if (!gTraceScreen.empty()) {
        size_t at = 0;
        while (at <= gTraceScreen.size()) {
            const size_t c = gTraceScreen.find(',', at);
            const std::string tok = gTraceScreen.substr(
                at, c == std::string::npos ? std::string::npos : c - at);
            if (!tok.empty()) {
                gTraceScreenIds.push_back(static_cast<int32_t>(std::atoll(tok.c_str())));
            }
            if (c == std::string::npos) break;
            at = c + 1;
        }
    }

    for (const std::string& rm : randMove) {
        const int32_t pi = (rm == "all") ? -1 : static_cast<int32_t>(std::atoll(rm.c_str()));
        const size_t n = g.scenep->SetControllerRandMove(pi, true);
        std::printf("rand move: %zu controller(s) for player %s\n", n, rm.c_str());
    }
    if (!navPng.empty()) {
        // A debug view of the navigation grid: one pixel per cell.  Green is
        // walkable, red a static blocker, blue a cell with a floor whose
        // elevation differs from its neighbours, black no floor at all.
        const NavGrid& n = g.scenep->Nav();
        std::vector<uint8_t> img(static_cast<size_t>(n.nx) * n.nz * 4, 0);
        for (int32_t z = 0; z < n.nz; ++z) {
            for (int32_t x = 0; x < n.nx; ++x) {
                uint8_t* d = img.data() + (static_cast<size_t>(z) * n.nx + x) * 4;
                const int16_t s = n.surface[n.Idx(x, z)];
                d[3] = 255;
                if (s == kNoFloor) {
                    continue;
                }
                const int v = 60 + ((s - 120) * 6 % 160);
                if (n.stat[n.Idx(x, z)]) {
                    d[0] = 200;
                    d[1] = 40;
                    d[2] = 40;
                } else {
                    d[0] = 30;
                    d[1] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
                    d[2] = 30;
                }
            }
        }
        std::string perr;
        if (!ft::WritePng(navPng, n.nx, n.nz, img, &perr)) {
            std::fprintf(stderr, "%s: %s\n", navPng.c_str(), perr.c_str());
        } else {
            std::printf("nav grid %dx%d -> %s\n", n.nx, n.nz, navPng.c_str());
        }
    }

    g.W = static_cast<int>(size[0]);
    g.H = static_cast<int>(size[1]);
    g.fb.assign(static_cast<size_t>(g.W) * g.H * 4, 0);
    g.scenep->ScreenBounds(g.bounds);
    if (haveCentre) {
        int32_t cx = 0, cy = 0;
        ProjectWorldToScreen(static_cast<int32_t>(centre[0]), static_cast<int32_t>(centre[1]),
                             static_cast<int32_t>(centre[2]), &cx, &cy);
        g.camFx = cx - g.W / 2.0;
        g.camFy = cy - g.H / 2.0;
    } else {
        g.camFx = (g.bounds[0] + g.bounds[2]) / 2.0 - g.W / 2.0;
        g.camFy = (g.bounds[1] + g.bounds[3]) / 2.0 - g.H / 2.0;
    }
    if (!campaignOnly) {
        ClampCamera();
    }

    // MILESTONE 12: the vehicle census -- which shipped campaign maps carry a
    // `Vehicle`, and what each one is.
    if (vehicleCensus) {
        const std::string root =
            (g.assetsDir.empty() ? std::string(kDefaultAssets) : g.assetsDir) +
            "/campaigns/missions/core";
        std::vector<std::string> names;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.size() > 4 && n.substr(n.size() - 4) == ".mis") names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            Scene sc;
            SceneOptions o = opt;
            std::string e2;
            if (!sc.Load(root + "/" + n, o, &e2)) continue;
            for (int32_t vi : sc.Vehicles()) {
                const LiveEntity& v = sc.Entities()[static_cast<size_t>(vi)];
                std::printf("VEHICLE %-14s entity %3d  %-14s %-10s seats %d  accel %.1f  "
                            "max %.1f/%.1f  turn %.0f  gunner %d  weapon %s  at %d,%d,%d\n",
                            n.c_str(), vi, v.label.c_str(), v.vehicleType.c_str(), v.seats,
                            v.accel, v.maxSpeed, v.maxRunSpeed, v.turnRate,
                            v.gunnerEnabled ? 1 : 0,
                            v.vehicleWeapon.valid ? v.vehicleWeapon.itemType.c_str() : "-",
                            v.world[0], v.world[1], v.world[2]);
            }
        }
        return 0;
    }

    // MILESTONE 21: the image layer's own probe.  `--image-dump <rel>[:<png>]`
    // pulls one resource through `ResourceRoot::Image` -- the SAME entry point
    // every screen uses, sniff and all -- and says which handler claimed it and
    // what came out.  With a second field it also writes the decoded RGBA as a
    // PNG, which a check can compare against the shipped file.
    if (!g.imageDump.empty()) {
        int bad = 0;
        for (const std::string& spec : g.imageDump) {
            std::string rel = spec, out;
            const size_t colon = spec.rfind(':');
            if (colon != std::string::npos && colon + 1 < spec.size()) {
                rel = spec.substr(0, colon);
                out = spec.substr(colon + 1);
            }
            const char* fmt = "unknown";
            std::vector<uint8_t> raw;
            std::string ferr;
            const std::string path = g.ui.Ready() ? g.ui.Resources()->Resolve(rel) : std::string();
            if (!path.empty() && ft::ReadWholeFile(path, &raw, &ferr)) {
                fmt = ft::ImageFormatName(ft::DetectImageFormat(raw));
            }
            const Art* a = g.ui.Ready() ? g.ui.Resources()->Image(rel) : nullptr;
            if (a == nullptr || !a->ok) {
                std::printf("IMAGE %-34s %-6s FAILED\n", rel.c_str(), fmt);
                ++bad;
                continue;
            }
            std::printf("IMAGE %-34s %-6s %dx%d %zu bytes\n", rel.c_str(), fmt, a->w, a->h,
                        raw.size());
            if (!out.empty()) {
                std::string perr;
                if (!ft::WritePng(out, a->w, a->h, a->rgba, &perr)) {
                    std::printf("IMAGE %-34s write %s: %s\n", rel.c_str(), out.c_str(),
                                perr.c_str());
                    ++bad;
                } else {
                    std::printf("IMAGE %-34s wrote %s\n", rel.c_str(), out.c_str());
                }
            }
        }
        return bad == 0 ? 0 : 1;
    }

    // MILESTONE 12: boot the campaign layer.
    if (g.campaignMode) {
        g.camUi.reset(new CampaignUi(&g.ui));
        CampaignBoot(g.assetsDir);
        if (g.camDump) {
            std::printf("%s\n", g.camp.file.info.c_str());
            std::printf("start: %s   squad marker (%.0f, %.0f)   terrain %dx%d\n",
                        g.camp.file.startMission.c_str(), g.camp.posX, g.camp.posY,
                        g.camp.file.terrain.w, g.camp.file.terrain.h);
            std::printf("\nmission graph (%zu locations, campaign order):\n",
                        g.camp.cores.size());
            for (size_t i = 0; i < g.camp.cores.size(); ++i) {
                const CoreMission& m = g.camp.cores[i];
                std::printf("  %2zu  %-40s %-18s (%4d,%4d) r=%2d  %-9s %s\n", i,
                            m.misPath.c_str(),
                            g.ui.Ready() ? g.ui.Strings().Get(m.nameKey).c_str()
                                         : m.nameKey.c_str(),
                            m.locX, m.locY, m.radius,
                            m.state == MissionState::Visited
                                ? "Visited"
                                : (m.state == MissionState::UnCovered ? "UnCovered" : "Covered"),
                            m.isBunker ? "BUNKER" : "");
            }
            std::printf("\nspecials: %zu   landscapes: %zu   prefabs: %zu   forces: %zu\n",
                        g.camp.file.specials.size(), g.camp.file.landscapes.size(),
                        g.camp.file.prefabs.size(), g.camp.file.forces.size());
            for (size_t i = 0; i < g.camp.file.prefabs.size(); ++i) {
                std::printf("  prefab %zu  %s\n", i, g.camp.file.prefabs[i].c_str());
            }
            std::printf("\nstarting squad (tag mission00) and pool (tag mission01):\n");
            for (const RosterMember& m : g.camp.roster) {
                std::printf("  squad %s\n", m.entPath.c_str());
            }
            for (const std::string& p2 : g.camp.pool) std::printf("  pool  %s\n", p2.c_str());
            std::printf("\nquartermaster at mission01: ");
            int n = 0;
            for (const auto& kv : g.camp.stock) {
                if (n++ < 6) std::printf("%s x%d  ", kv.first.c_str(), kv.second);
            }
            std::printf("(%zu lines)\n", g.camp.stock.size());
            return 0;
        }
    }

    if (g.headless) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!g.headless) {
        g.window = SDL_CreateWindow("Fallout Tactics CE", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, g.W, g.H, SDL_WINDOW_RESIZABLE);
        if (g.window == nullptr) {
            std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return 1;
        }
        g.renderer = SDL_CreateRenderer(g.window, -1, SDL_RENDERER_ACCELERATED);
        if (g.renderer == nullptr) {
            g.renderer = SDL_CreateRenderer(g.window, -1, SDL_RENDERER_SOFTWARE);
        }
        SDL_SetWindowSize(g.window, g.W, g.H);
        // MILESTONE 18: the game draws its own cursor out of `gui/cursor/*.zar`
        // (`gui/cursors.cfg`, `LoadCursorDefinitions @006605f0`), so the OS
        // one goes away -- which is what `display.cursorMode = software` does
        // in the original, and it is the mode whose help string says the
        // cursor "will appear in screenshots".
        SDL_ShowCursor(SDL_DISABLE);
#ifdef __EMSCRIPTEN__
        // The emscripten SDL2 backend leaves the canvas at whatever size the
        // page gave it; pin it to the framebuffer so the blit is 1:1.
        emscripten_set_canvas_element_size("#canvas", g.W, g.H);
        SDL_RenderSetLogicalSize(g.renderer, g.W, g.H);
#endif
        g.texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888,
                                      SDL_TEXTUREACCESS_STREAMING, g.W, g.H);
    }
    g.lastTicks = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(EmFrame, 0, 1);
#else
    while (g.running) {
        Tick();
    }
#endif
    // MILESTONE 11: one last AI dump, so a headless run that takes no
    // screenshot still reports what every AI ended up doing.
    if (dumpLoot) {
        std::printf("%s", g.scenep->TraceLoot().c_str());
    }
    if (g.traceAi) {
        std::printf("--- ai, end of run (tick %lld) ---\n%s",
                    (long long)g.scenep->Ticks(), g.scenep->TraceAI().c_str());
    }
    if (g.renderFrames > 0) {
        const double secs = static_cast<double>(g.renderTicks) /
                            static_cast<double>(SDL_GetPerformanceFrequency());
        std::printf("render: %lld frames in %.3f s -> %.1f fps (%.2f ms/frame); last frame "
                    "%zu tiles, %zu entities, %zu sorted\n",
                    (long long)g.renderFrames, secs, g.renderFrames / secs,
                    secs * 1000.0 / g.renderFrames, g.lastStats.tilesDrawn,
                    g.lastStats.entitiesDrawn, g.lastStats.sortElems);
    }
    // MILESTONE 13: --audio-dump writes the mix the offline run produced.
    if (!g.audioDump.empty() && g.audio.Enabled()) {
        std::string aerr;
        if (g.audio.WriteDumpWav(g.audioDump, &aerr)) {
            const int64_t frames = g.audio.DumpFrames();
            std::printf("audio-dump: %s  %lld frames (%.2f s)  onsets %d  events %zu\n",
                        g.audioDump.c_str(), (long long)frames,
                        static_cast<double>(frames) / Audio::kRate,
                        g.audio.CountDumpOnsets(), g.audio.Log().size());
        } else {
            std::fprintf(stderr, "audio-dump: %s\n", aerr.c_str());
        }
    }
    // MILESTONE 15: the resource manifest, one name per line.
    if (!g.resourceManifest.empty()) {
        const std::vector<std::string> used = ft::VfsTakeAccessLog();
        if (std::FILE* f = std::fopen(g.resourceManifest.c_str(), "w")) {
            for (const std::string& k : used) std::fprintf(f, "%s\n", k.c_str());
            std::fclose(f);
            std::printf("resource-manifest: %zu names -> %s\n", used.size(),
                        g.resourceManifest.c_str());
        } else {
            std::fprintf(stderr, "resource-manifest: cannot write %s\n",
                         g.resourceManifest.c_str());
        }
    }
    // MILESTONE 20: the sprite manifest, one sequence name per line.
    if (!g.spriteManifest.empty()) {
        const std::vector<std::string> used = ft::SprTakeUsageLog();
        if (std::FILE* f = std::fopen(g.spriteManifest.c_str(), "w")) {
            for (const std::string& k : used) std::fprintf(f, "%s\n", k.c_str());
            std::fclose(f);
            std::printf("sprite-manifest: %zu sequences -> %s\n", used.size(),
                        g.spriteManifest.c_str());
        } else {
            std::fprintf(stderr, "sprite-manifest: cannot write %s\n",
                         g.spriteManifest.c_str());
        }
    }
    g.audio.Shutdown();
    if (g.renderer != nullptr) {
        SDL_DestroyRenderer(g.renderer);
    }
    if (g.window != nullptr) {
        SDL_DestroyWindow(g.window);
    }
    SDL_Quit();
    return 0;
}

int main(int argc, char** argv) {
    game::AttachParentConsole();
    if (!game::IsPlayerLaunch(argc, argv)) return RunGame(argc, argv);

    // A double-click, or the Mac app: find the game, keep the saves apart from
    // the original's, and start at the title menu.
    game::PlayerSetup setup;
    if (!game::ResolvePlayerSetup(&setup)) return 0;
    game::RedirectOutputToLog(setup.logPath);
    std::vector<std::string> args = {argc > 0 ? argv[0] : "fotgame", "--campaign",
                                     "--assets", setup.core, "--save-dir", setup.saveDir};
    std::vector<char*> av;
    for (std::string& a : args) av.push_back(&a[0]);
    av.push_back(nullptr);
    const int rc = RunGame(static_cast<int>(args.size()), av.data());
    if (rc != 0) {
        game::ShowLaunchError("Fallout Tactics CE stopped with an error.\n\n" +
                              game::LogTail(setup.logPath, 12) +
                              "\nThe whole log is in fotgame.log, next to the saves folder.");
    }
    return rc;
}
