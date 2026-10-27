// ui_screens.cpp -- the game bar and the five full-screen windows.
//
// Every literal in this file is a coordinate out of the shipped hi-res build.
// The provenance of each group is in ui.h; the function and address is repeated
// beside the group here.  Anything with no coordinate in the dumps is marked
// RECONSTRUCTION at the point of use.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "scene.h"
#include "ui.h"

namespace game {
namespace {

// `DrawSquadMemberPortraitFrame @004d1740`'s `local_20 = 0.5` arm, applied to a
// packed 0xRRGGBB: the label of a squad member who is down is drawn at half the
// style colour (VIDEO_GAPS M10).
inline uint32_t HalfBright(uint32_t rgb) {
    return (((rgb >> 17) & 0x7F) << 16) | (((rgb >> 9) & 0x7F) << 8) | ((rgb >> 1) & 0x7F);
}

// --- WGameBar, the 800 x 128 design space -----------------------------------
// `WGameMain::WGameMain@00470990` sizes the bar window; every literal below is out of
// `WGameBar::LayoutGameBarControls @0047f420`, whose `iVar3` centring term is
// identically zero because the bar window is itself exactly 800 wide.
constexpr int32_t kBarW = 800, kBarH = 128;
constexpr int32_t kMiniX = 10, kMiniY = 15, kMiniW = 214, kMiniH = 100;
constexpr int32_t kMMX = 233, kMMMapY = 49, kMMTextY = 79;
constexpr int32_t kTabX = 236, kTabY = 0, kTabW = 80, kTabH = 30, kTabPitch = 81;
constexpr int32_t kPortraitX = 270, kPortraitY = 47, kPortraitW = 68, kPortraitH = 72;
constexpr int32_t kHpX = 273, kHpY = 102, kStatW = 73, kStatH = 10;
constexpr int32_t kAcY = 112;
constexpr int32_t kHand0X = 346, kHand1X = 500, kHandY = 50;
constexpr int32_t kHandW = 140, kHandH = 69;
constexpr int32_t kApX = 367, kApY = 33;
// The sentry group at x 651 and the stance group at x 689, both three rows at
// y 48 / 72 / 97 (NOTES.md 31 -- the pitch is 24 then 25, not a uniform 24, and
// the moulded recesses in `game_bar_long.zar` land on exactly those).
constexpr int32_t kSentryX = 651, kStanceX = 689;
constexpr int32_t kColY[3] = {48, 72, 97};
constexpr int32_t kSentryW = 29, kSentryH = 22;   // Sentry1..3_Up|Dn.zar
constexpr int32_t kStanceW = 30, kStanceH = 24;   // Stand/Crouch/Prone_Up|Dn.zar

// MILESTONE 19.  The sentry column is a THREE-BUTTON RADIO GROUP, not one
// cycling button.  `gui/btn/Sentry1_Up|Dn.zar` .. `Sentry3_*` all ship -- 29 x
// 22 Vault Boy faces, calm / wary / grinning, `_Up` grey and `_Dn` backlit
// amber -- and the reference capture shows all three plates stacked with
// exactly one lit (f0576 top, f0639 middle, f0604 bottom).
//
// keys.cfg's `sentryOff` / `sentryScout` / `sentryOn` are the same three, and
// the key dispatcher (`WGameMain::DispatchInputActionByName@004734f0`) calls
// `WGameWorld::LogLegalTargetFieldChange0e @0049eb60` once per verb with the
// mode as its argument: 0e is a SETTER over every selected legal target, not a
// cycler.  So each plate sets its own mode, through the same verb the key uses.
//
// `gui/btn/Overwatch_Up|Dn.zar` (30 x 72, one large medallion; `sentry_Up|Dn`
// is the same art under the old name) is the OTHER arrangement of the same
// 30 x 72 footprint, driven by `...0f @0049ed00`, order 0x0f --
// `WGameBar::UpdateGameBar @004803a0` enables one group or the other the way it
// does for the squad panel versus the portrait.  The retail capture shows the
// three-plate form, so that is what is drawn.
struct SentryButton { const char* art; const char* verb; const char* help; SentryMode mode; };
const SentryButton kSentryButtons[3] = {{"Sentry1", "sentryOff", "sentry1", kSentryOff},
                                        {"Sentry2", "sentryScout", "sentry2", kSentryScout},
                                        {"Sentry3", "sentryOn", "sentry3", kSentryOn}};
struct StanceButton { const char* art; const char* verb; const char* help; };
const StanceButton kStanceButtons[3] = {{"Stand", "stand", "stand"},
                                        {"Crouch", "crouch", "crouch"},
                                        {"Prone", "prone", "prone"}};
constexpr int32_t kBtnX = 744, kBtnY = 7, kBtnPitch = 23, kBtnW = 49, kBtnH = 22;

// The five captions, read out of the executable at 0089a794 (SKL), 0089a790
// (INV), 0089a78c (CHR), 0089a788 (PIP) and 0089a784 (MNU) -- the constructor
// walks them downwards from 0089a794 as y steps by 0x17.
struct BarButton { const char* caption; const char* command; };
const BarButton kBarButtons[5] = {{"SKL", "skilldex"},
                                  {"INV", "inventory"},
                                  {"CHR", "character"},
                                  {"PIP", "pipboy"},
                                  {"MNU", "menu"}};

// `WIIHand::Draw @00492d20`, all relative to the 140 x 69 slot.
constexpr int32_t kModeX0 = 6, kModeY0 = 6, kModeX1 = 126, kModeY1 = 22;
constexpr int32_t kSpareX0 = 7, kSpareX1 = 77;
constexpr int32_t kNoAmmoX0 = 4, kNoAmmoY0 = 27, kNoAmmoX1 = 134, kNoAmmoY1 = 43;
constexpr int32_t kApTextX0 = 7, kApTextY0 = 52, kApTextX1 = 137, kApTextY1 = 68;
constexpr int32_t kAmmoBarX = 134, kAmmoBarY = 44, kAmmoBarTicks = 39;
constexpr int32_t kCheckX = 117, kCheckY = 47;

// `WCharacter`, `WCharacter::WCharacter@00458b40`.  The two panel origins the constructor
// stores at this+0x1596 (AX) and this+0x1592 (SX).
constexpr int32_t kCharAX = 0, kCharSX = 300;
constexpr int32_t kSpecialY0 = 59, kSpecialPitch = 32;      // BuildAttributeSpinnerControls
constexpr int32_t kSkillY0 = 68, kSkillPitch = 10;          // CreateSkillSelectionButtons
constexpr int32_t kDerivedY0 = 174, kDerivedPitch = 11;     // BuildCharacterStatComparisonRows
constexpr int32_t kStatusY0 = 47;   // and pitch 11, for the eight rows below it

// The three `.data` tables, read out of the executable.
// 008a6b70: the seven SPECIAL keys.
const char* const kSpecialKeys[7] = {"strength", "perception",   "endurance", "charisma",
                                     "intelligence", "agility", "luck"};
// 008a6b18: the eighteen skill keys, which are exactly combat.h's SkillId order.
const char* const kSkillKeys[18] = {
    "smallGuns", "bigGuns", "energyWeapons", "unarmed", "meleeWeapons", "throwing",
    "firstAid",  "doctor",  "sneak",         "lockpick", "steal",       "traps",
    "science",   "repair",  "pilot",         "barter",  "gambling",    "outdoorsman"};
// 00898f34: the twelve derived-stat keys the sheet lists, and 00898f68: the
// per-row flag that picks the "%d%%" format over "%d".
const char* const kDerivedKeys[12] = {
    "armorClass",  "meleeDamage",  "bonusDamage",     "normalResist",
    "poisonResist", "radiationResist", "maxActionPoints", "maxCarryWeight",
    "healRate",    "criticalChance", "skillPerLevel",  "levelsPerPerk"};
const uint8_t kDerivedPercent[12] = {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};

// The skilldex list.  `WSkillDex` is in NO dump (one construction call, mode
// 10, size 0x13E, `WGameMain::SetActiveGameWindowMode@00471ad0`) so neither its buttons nor their
// positions are recoverable.  What IS shipped is `keys.cfg`, whose eight
// number keys bind exactly these eight skills in this order -- that is the
// skilldex, and the layout below is a RECONSTRUCTION on the shipped art.
const char* const kSkilldex[8] = {"sneak",   "lockpick", "steal",  "traps",
                                  "firstAid", "doctor",  "science", "repair"};

// The in-game menu.  The window class is in no dump and no `gameMenu_*` symbol
// is referenced anywhere; `locale/gui/gui_text.txt` defines exactly these six
// keys, in this order, and `gui/back/Menu.zar` is the art.  Order and geometry
// are a RECONSTRUCTION.
const char* const kMenuKeys[6] = {"gameMenu_keysHelp", "gameMenu_saveGame", "gameMenu_loadGame",
                                  "gameMenu_options",  "gameMenu_exit",     "gameMenu_done"};

// MILESTONE 16: where a BOS button's label actually goes.
//
// `WBOSButton::WBOSButton @0065bcb0` does NOT draw its own text: it news a
// child `WBOSLargeText` / `WBOSMediumText` and gives it a rect that is a
// hardcoded inset of the button, then resizes the BUTTON to the art
// (`WWindow::ResizeWindow(this, {img.w, img.h})` at the tail) while leaving
// that inset alone.  The three literals, straight out of the constructor:
//
//   size 0 "big"    (0x39, 2, 0xe3, 0x27)  = (57, 2)-(227, 39)   art 240 x 40
//   size 1 "med"    (0x20, 1, 0x92, 0x15)  = (32, 1)-(146, 21)   art 152 x 24
//   size 2 "small"  (0x20, 1, 0x7a, 0x15)  = (32, 1)-(122, 21)   art 125 x 24
//
// so a label is NOT centred on the button: the art's left cap is excluded and
// the text sits right of the button's own centre by 22 px (big), 13 px (med)
// and 14 px (small).  Centring on the whole rect -- which is what this port
// did -- puts every label too far left, which is what the player saw.
//
// Within that rect `DefaultStyle::DrawStyledText @00731bc0` does the placing:
// x = left, or right - textWidth (align 1), or left + (width - textWidth)/2
// (align 2); and in ALL THREE cases y = top + ((bottom - top) - fontHeight)/2.
// There is no baseline offset.  (Modes 3/4/5 are the word-wrapping forms, and
// their line pitch is fontHeight + fontHeight/4 -- which closes NOTES.md 35's
// "line spacing between two drawn lines is not recoverable".)
constexpr int32_t kMenuPlate[4] = {82, 12, 234, 48};

std::string Fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// `WPortrait::RenderPortraitAndPointsLabel @004bc7d0` centres the image in the
// widget; the scale is the ctor's own argument (0.85f for the selected
// character, 0.66f for the six squad-panel slots).  The resampling filter it
// uses is not in the dump -- this is nearest neighbour.
void BlitScaled(Canvas* c, const Art& a, int32_t x, int32_t y, int32_t w, int32_t h,
                float tr = 1.0f, float tg = 1.0f, float tb = 1.0f) {
    if (!a.ok || w <= 0 || h <= 0) return;
    for (int32_t dy = 0; dy < h; ++dy) {
        const int32_t sy = dy * a.h / h;
        for (int32_t dx = 0; dx < w; ++dx) {
            const int32_t sx = dx * a.w / w;
            const uint8_t* s = &a.rgba[(static_cast<size_t>(sy) * a.w + sx) * 4];
            if (s[3] == 0) continue;
            const uint32_t r = static_cast<uint32_t>(std::min(255.0f, s[0] * tr));
            const uint32_t g = static_cast<uint32_t>(std::min(255.0f, s[1] * tg));
            const uint32_t b = static_cast<uint32_t>(std::min(255.0f, s[2] * tb));
            c->Pixel(x + dx, y + dy, (r << 16) | (g << 8) | b, s[3]);
        }
    }
}

// MILESTONE 20 (VIDEO_GAPS M8): the portrait wells are a green CRT.  Every
// `gui/char/**.zar` in the shipped tree is PURE GREYSCALE (R == G == B on every
// pixel), and over f0036's portrait well the capture reads a mean of
// (39.7, 58.3, 29.0) with the odd rows 6 % darker than the even ones -- a tint
// of (0.68, 1.00, 0.50) plus a one-pixel scanline.  `WPortrait::
// RenderPortraitAndPointsLabel @004bc7d0` is the draw and its blit is the
// tinting one, so the tint goes where the original puts it; the scanline pitch
// and depth are MEASURED, not recovered.
constexpr float kCrtR = 0.68f, kCrtG = 1.00f, kCrtB = 0.50f;

void BlitPortraitCrt(Canvas* c, const Art& a, int32_t x, int32_t y, int32_t w, int32_t h) {
    BlitScaled(c, a, x, y, w, h, kCrtR, kCrtG, kCrtB);
    for (int32_t sy = y + 1; sy < y + h; sy += 2) c->Fill(x, sy, x + w, sy + 1, 0x000000, 16);
}

}  // namespace

Ui::Ui() = default;
Ui::~Ui() = default;

bool Ui::Init(const std::string& assetsDir, std::string* errorOut) {
    m_res.reset(new ResourceRoot(assetsDir));
    if (m_res->Resolve("gui/def_style.cfg").empty() &&
        m_res->Resolve("gui/back/game_bar_long.zar").empty()) {
        *errorOut = assetsDir + ": no gui/ tree (extract data/core/gui_0.bos into it)";
        return false;
    }
    if (!m_style.Load(m_res.get(), errorOut)) return false;
    m_loc.LoadDir(*m_res, "locale");
    LoadCursors();                       // MILESTONE 18
    m_ready = true;
    m_info = "ui: fonts " + std::to_string(m_style.normal.Height()) + "/" +
             std::to_string(m_style.lite.Height()) + "/" +
             std::to_string(m_style.medium.Height()) + "/" +
             std::to_string(m_style.large.Height()) + " px";
    return true;
}


// ===========================================================================
// MILESTONE 18 -- the cursors.
// ===========================================================================
// `gui/cursors.cfg` is shipped data and is the whole of the art and the
// hotspots; `LoadCursorDefinitions @006605f0`, called once from
// `GameApplication::MainInit @006c4180` with the array at 0x008bdaf0, is the
// reader.  The two lines it parses are
//
//     cursor_move = {Cursor("gui/cursor/move", Point(15,9) )};
//     cursor_wait = {AnimatedCursor("gui/cursor/normal", Point(1,1), 2, 500)};
//
// and `cursor_normal` / `cursor_caret` come from `gui/def_style.cfg` instead.
// ---------------------------------------------------------------------------
void Ui::LoadCursors() {
    static const char* const kFiles[2] = {"gui/cursors.cfg", "gui/def_style.cfg"};
    for (const char* rel : kFiles) {
        const std::string path = m_res->Resolve(rel);
        if (path.empty()) continue;
        std::vector<uint8_t> file;
        std::string err;
        if (!ft::ReadWholeFile(path, &file, &err)) continue;
        const std::string text(reinterpret_cast<const char*>(file.data()), file.size());
        size_t p = 0;
        while (p < text.size()) {
            size_t eol = text.find('\n', p);
            if (eol == std::string::npos) eol = text.size();
            std::string line = text.substr(p, eol - p);
            p = eol + 1;
            const size_t comment = line.find("//");
            if (comment != std::string::npos) line = line.substr(0, comment);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            size_t k0 = 0;
            while (k0 < key.size() && (key[k0] == ' ' || key[k0] == '\t')) ++k0;
            key = key.substr(k0);
            if (key.compare(0, 7, "cursor_") != 0) continue;
            const size_t q0 = line.find('"', eq);
            const size_t q1 = q0 == std::string::npos ? std::string::npos : line.find('"', q0 + 1);
            if (q1 == std::string::npos) continue;
            const std::string art = line.substr(q0 + 1, q1 - q0 - 1);
            int32_t hx = 0, hy = 0;
            const size_t pt = line.find("Point(", q1);
            if (pt != std::string::npos) {
                std::sscanf(line.c_str() + pt + 6, "%d , %d", &hx, &hy);
            }
            CursorArt ca;
            ca.art = m_res->Image(art);
            ca.hotX = hx;
            ca.hotY = hy;
            if (ca.art != nullptr && ca.art->ok) m_cursors[key.substr(7)] = ca;
        }
    }
}

// `WGameWorld::GetSelectionStatusMessage @004a2c40` -- the world cursor -- and
// `WGameWorld::GetSelectionCursorIcon @0065b300` -- the eight edge arrows --
// transcribed in the order the binary tests them.  The array indices those two
// return are decoded in ui.h; the names below are the `cursors.cfg` keys they
// land on.
//
// The binary's own order:
//   1. band box wider or taller than DAT_00826664  -> drag
//   2. skill mode (0x1d3) or use-item mode (0x1d5) -> skill, or cancelUse when
//      nothing is under the pointer
//   3. attack-friendly mode (0x1d8)                -> attackFriendly
//   4. attack mode (0x1d7)                         -> attack
//   5. vehicle arms                                -> vehicleIn / vehicleOut
//   6. the pointer is over the world (0x1d6):
//        a live legal target of MY player, not a vehicle
//            -> select, or cancelUse when it cannot act this turn
//        IsEntityValidTalkTarget                   -> speech
//        otherwise                                 -> use
//   7. not over anything usable: climbUp / climbDown by tile height, else
//      moveBad; and on walkable ground, move -- or moveFormation when
//      `field_0x1ce == (DAT_008bda1c != 0)` is false, i.e. when
//      `game.formationMode` is on and the F key is not held.
//
// **`cursor_run` is never returned.**  0x008bdb0c is loaded by
// `LoadCursorDefinitions` and referenced by nothing in the executable; a run
// order shows the plain move cursor.  `cursor_loot` is in cursors.cfg and is
// not even loaded.  Both were checked with a whole-binary data-reference scan.
std::string Ui::PickCursor(Scene* scene) {
    if (m_mouseX < 0 || m_mouseY < 0) return "normal";
    // A drag in progress (the equip / loot ghost) is `cursor_drag`, which is
    // also what `WIIBase::CheckDragThresholdExceeded @0049064a` reads.
    if (m_drag.active) return "drag";
    // Over the game bar or any open panel: the plain pointer.
    if (m_screen != UiScreen::None || m_mouseY >= BarY()) return "hand";

    // The eight edge arrows.  `GetSelectionCursorIcon`'s four booleans at
    // `WGameWorld+0x106..0x109` are set by a function that is not in any dump,
    // so which flag is which edge, and the margin, are RECONSTRUCTED; the
    // eight-way selection tree itself is the binary's.
    {
        const int32_t m = EdgeScrollMargin();
        const bool up = m_mouseY < m;
        const bool down = m_mouseY >= ViewportHeight(m_H) - m;
        const bool left = m_mouseX < m;
        const bool rightE = m_mouseX >= m_W - m;
        if (up) {
            if (left) return "scroll7";
            if (rightE) return "scroll1";
            return "scroll0";
        }
        if (down) {
            if (left) return "scroll5";
            if (rightE) return "scroll3";
            return "scroll4";
        }
        if (left) return "scroll6";
        if (rightE) return "scroll2";
    }

    if (scene == nullptr) return "normal";
    const int32_t hit = scene->Pick(m_camX, m_camY, m_mouseX, m_mouseY);
    const std::vector<LiveEntity>& ents = scene->Entities();
    const bool haveHit = hit >= 0 && static_cast<size_t>(hit) < ents.size();

    // 2. the skill cursor, one-shot until the next click.
    if (!m_skillCursor.empty()) return haveHit ? "skill" : "cancelUse";

    const int32_t mine = scene->SpawnPlayer();
    if (haveHit) {
        const LiveEntity& t = ents[static_cast<size_t>(hit)];
        // 6a. one of mine -> select.
        if (t.isCombatant && !t.dead && t.playerIndex == mine) return "select";
        // 4. an attack target -> attack.  `field_0x1d7` is the targeting
        //    flag and it is tested BEFORE the entity arms; it is set when the
        //    thing under the pointer is something this squad would fire at,
        //    i.e. a live hostile.  `field_0x1d8` is the friendly-fire twin
        //    (`attackFriendly`), which `locale/gui/help.txt` reaches with
        //    "Ctrl-Right Click: Force Fire" -- so it is the ctrl modifier over
        //    a live actor that is NOT hostile.
        if (t.isCombatant && !t.dead) {
            const int32_t me = scene->Selected();
            if (me >= 0 && scene->IsHostile(me, hit)) return "attack";
            if (m_ctrl) return "attackFriendly";
        }
        // 6b. talk.
        if (scene->TalkTarget(hit)) return "speech";
        // 6c. anything else the pointer is over -> use.  This is
        //     `GetSelectionStatusMessage`'s own fall-through (DAT_008bdaf8),
        //     which is reached for EVERY hovered entity that is not one of
        //     mine, not talkable and not a vehicle -- a container, a corpse, a
        //     ground item, a door, and equally a live neutral NPC, because
        //     "Left Click on Object: Use" is what the click would do to it.
        return "use";
    }

    // 7. the ground.
    int32_t wx = 0, wz = 0;
    if (!scene->ScreenToGround(m_camX, m_camY, m_mouseX, m_mouseY, &wx, &wz)) return "moveBad";
    const bool formation = (m_formationMode != 0) != m_formationInvert;
    return formation ? "moveFormation" : "move";
}

void Ui::DrawCursor(Canvas* c) {
    if (!m_ready || m_mouseX < 0 || m_mouseY < 0) return;
    auto it = m_cursors.find(m_cursorName);
    if (it == m_cursors.end()) it = m_cursors.find("normal");
    if (it == m_cursors.end() || it->second.art == nullptr) return;
    c->SetClip(0, 0, m_W, m_H);
    c->Blit(*it->second.art, m_mouseX - it->second.hotX, m_mouseY - it->second.hotY);
}

void Ui::DrawPointer(Canvas* c, int32_t x, int32_t y) const {
    if (!m_ready || x < 0 || y < 0) return;
    const auto it = m_cursors.find("normal");
    if (it == m_cursors.end() || it->second.art == nullptr) return;
    c->SetClip(0, 0, c->w, c->h);
    c->Blit(*it->second.art, x - it->second.hotX, y - it->second.hotY);
}

void Ui::PushMessage(const std::string& s) {
    m_messages.push_back(s);
    if (m_messages.size() > 64) m_messages.erase(m_messages.begin());
}

Ui::Rect Ui::Modal(int32_t w, int32_t h) const {
    Rect r;
    r.x0 = (m_W - w) / 2;
    r.y0 = (ViewportHeight(m_H) - h) / 2;
    if (r.y0 < 0) r.y0 = 0;
    r.x1 = r.x0 + w;
    r.y1 = r.y0 + h;
    return r;
}

// ---------------------------------------------------------------------------
// The frame.
// ---------------------------------------------------------------------------
void Ui::Draw(Scene* scene, uint8_t* fb, int32_t W, int32_t H) {
    if (!m_ready) return;
    m_W = W;
    m_H = H;
    Canvas c;
    c.fb = fb;
    c.w = W;
    c.h = H;
    c.SetClip(0, 0, W, H);
    m_tooltip.clear();
    m_trace.clear();

    switch (m_screen) {
        case UiScreen::Inventory: DrawInventory(scene, &c); break;
        case UiScreen::Character: DrawCharacter(scene, &c); break;
        case UiScreen::Pipboy: DrawPipboy(scene, &c); break;
        case UiScreen::Skilldex: DrawSkilldex(scene, &c); break;
        case UiScreen::Menu: DrawMenu(scene, &c); break;
        case UiScreen::Loot: DrawLoot(scene, &c); break;
        case UiScreen::MenuKeys:
        case UiScreen::MenuOptions: DrawMenuPage(scene, &c); break;
        case UiScreen::Debrief: DrawDebrief(scene, &c); break;
        case UiScreen::None: break;
    }
    c.SetClip(0, 0, W, H);
    // MILESTONE 10.  The script's own overlays sit over the world and under the
    // game bar; the debrief is a screen of its own and suppresses them.
    if (m_screen != UiScreen::Debrief) {
        DrawSpeech(scene, &c);
        DrawWorldText(scene, &c);
        DrawObjectivePopup(scene, &c);
    }
    if (scene->Script().DebriefShown() && m_screen == UiScreen::None && !m_campaignShell) {
        m_screen = UiScreen::Debrief;
        DrawDebrief(scene, &c);
    }
    c.SetClip(0, 0, W, H);
    DrawGameBar(scene, &c);
    DrawDragGhost(scene, &c);
    DrawTooltip(&c);
    // MILESTONE 18: the cursor is the LAST thing drawn, over everything.  The
    // original hides the OS cursor and draws its own -- `display.cursorMode`
    // in bos.cfg picks software / hardware / threaded, and the software one
    // "will appear in screenshots" (`optionsDisplayHelp_cursorMode_software`),
    // which is what this port does so the headless captures carry it.
    m_cursorName = PickCursor(scene);
    DrawCursor(&c);
}

// ---------------------------------------------------------------------------
// The game bar.
// ---------------------------------------------------------------------------
// The six squad tabs' roster.  `--squad` gives the campaign roster in roster
// order; without it the roster is every live actor in the SpawnPoint's own
// player slot, which is what `World::ValidateSpawnPointsAndAssignTeamColors
// @004f53d0` leaves standing there.
std::vector<int32_t> Ui::SquadList(const Scene* scene) {
    std::vector<int32_t> squad = scene->Squad();
    if (!squad.empty()) return squad;
    const std::vector<LiveEntity>& ents = scene->Entities();
    int32_t player = scene->SpawnPlayer();
    if (player < 0) {
        const int32_t sel = scene->Selected();
        player = sel >= 0 && static_cast<size_t>(sel) < ents.size() ? ents[sel].playerIndex : -1;
    }
    for (size_t i = 0; i < ents.size() && squad.size() < 6; ++i) {
        if (ents[i].isCombatant && !ents[i].dead && ents[i].playerIndex == player) {
            squad.push_back(static_cast<int32_t>(i));
        }
    }
    return squad;
}

void Ui::DrawGameBar(Scene* scene, Canvas* c) {
    const int32_t bx = BarX(), by = BarY();
    c->SetClip(0, by, m_W, m_H);

    // `WGameBar::CenterChildHorizontally @0047fa20`: the backdrop is centred on
    // the 800-wide bar window, and `game_bar_long.zar` is 1024 x 128, so it
    // sits at x = (800 - 1024) / 2 = -112 and covers a 1024-wide screen.  Drop
    // in the 800 x 128 `char_filler.zar` and the same code gives the original
    // 800 x 600 look with nothing else moved.
    const Art* back = Res()->Image("gui/back/game_bar_long.zar");
    if (back->ok) {
        c->Blit(*back, bx + (kBarW - back->w) / 2, by);
    } else {
        c->Fill(bx, by, bx + kBarW, by + kBarH, m_style.panelFace);
    }

    const std::vector<LiveEntity>& ents = scene->Entities();
    // The squad.  `--squad` gives the campaign roster in roster order; without
    // it the roster is every actor in the selected entity's own player slot,
    // which is what `World::ValidateSpawnPointsAndAssignTeamColors` leaves.
    const std::vector<int32_t> squad = SquadList(scene);

    // --- the minimap / message toggles, WMMGroup at (233, 45) ---------------
    c->Blit(*Res()->Image("gui/btn/Map_Up.zar"), bx + kMMX, by + kMMMapY);
    c->Blit(*Res()->Image("gui/btn/Text_Up.zar"), bx + kMMX, by + kMMTextY);

    c->Blit(*Res()->Image(m_showText ? "gui/btn/Text_Dn.zar" : "gui/btn/Map_Dn.zar"),
            bx + kMMX, m_showText ? by + kMMTextY : by + kMMMapY);
    if (m_showText) DrawMessages(c, bx, by);
    else DrawMinimap(scene, c, bx, by);

    // --- the six squad tabs, WSquadControlPanel (236 + 81k, 0) 80 x 30 ------
    // The tab's INTERNALS are not recoverable (`WSquadControlPanel` is a call
    // site only); what is, is `WSquadMember::DrawSquadMemberPortraitFrame
    // @004d1740`, which draws the name right-aligned at `width - textWidth - 8`
    // with baseline y + 15 and a two-pixel hit-point bar from x = 8 at
    // y = 21..23.  That is what is drawn here, in the 80 x 30 tab.
    //
    // MILESTONE 22 (VIDEO_GAPS M10), and a CORRECTION to that row.
    //
    // * The PLATE.  `gui/btn/squad_Up.zar` and `squad_Dn.zar` are 80 x 30 --
    //   exactly the tab -- and each is a raised riveted plate with a recessed
    //   dark well across its lower third for the health bar, which is the
    //   capture's tab down to the rivets.  Nothing in this port drew them.
    //   `_Dn` is the brighter of the two: on f0639 the selected member's plate
    //   face means (83, 75, 66) against (41, 36, 33) for the other two, which
    //   is the pair, so the SELECTED tab is `_Dn` and the rest `_Up`.
    // * The LABEL.  VIDEO_GAPS M10 says the selected tab is told apart "by
    //   label colour alone"; f0639 says otherwise -- Mick, Farsight and Stitch
    //   all read the same cream (250, 250, 195) there while only Stitch's PLATE
    //   is the bright one.  What the label colour actually marks is whether the
    //   member is still in the fight: on f0348, taken during the squad wipe,
    //   the live Wilma reads (253, 253, 217) and the two downed members read
    //   (85, 77, 75) -- half.  That is the binary's own rule.
    //   `DrawSquadMemberPortraitFrame` opens by halving its colour scale when
    //   the int at +0x114 is negative and the byte at +0x113 is 0
    //   -- +0x114 is the threshold VALUE the health bar draws, so "< 0" is "no
    //   live value" -- and ends by drawing the label in the style colour
    //   scaled by that factor, i.e. 1x normally and 0.5x for a
    //   member who is down.  The port drew a heavy green `selectionFill` wash
    //   over the whole 80 x 30 cell and coloured the label by selection; both
    //   are gone.
    for (int32_t k = 0; k < 6; ++k) {
        const int32_t tx = bx + kTabX + k * kTabPitch, ty = by + kTabY;
        if (k >= static_cast<int32_t>(squad.size())) continue;
        const LiveEntity& e = ents[static_cast<size_t>(squad[static_cast<size_t>(k)])];
        const bool selected = scene->IsSelected(squad[static_cast<size_t>(k)]);
        const Art* plate = Res()->Image(selected ? "gui/btn/squad_Dn.zar"
                                                 : "gui/btn/squad_Up.zar");
        if (plate->ok) c->Blit(*plate, tx, ty);
        if (m_hoverTab == k && !selected) {
            c->Fill(tx + 2, ty + 2, tx + kTabW - 2, ty + 3, m_style.bosText, 90);
        }
        // MILESTONE 22, a second correction to this port's reading of
        // `DrawSquadMemberPortraitFrame`: the name is CENTRED, not
        // right-aligned.  The function's last text call is
        //     (**(code **)(**(int **)(this + 0xd7) + 0x1c))
        //         (surface, font, &colour, &rect, &string, 2, 0)
        // over the rect (0, 4)-(width, 15), and the `2` is the alignment --
        // the same 2 every centred BOS caption passes (NOTES.md 84).  What
        // `width - textWidth - 8` really is, is the TRUNCATION limit a few
        // lines above it, the loop that walks the string a character at a time
        // while `iVar10` stays >= 0; the 8 is its padding, not an inset.
        // f0639 agrees: "Mick", "Farsight" and "Stitch" are each centred in
        // their own 80 px plate.
        // MILESTONE 16: the LOCALISED name.  `WSquadMember::
        // UpdateSquadMemberStatusDisplay @004d1f10` builds the label from
        // `GetLocalizedEntityName @0064f5d0`, i.e. `name_<Display Name>` out
        // of `locale/game/characters.txt`, which turns "CORE_rec_hm01" into
        // "Robin".  Before this the raw key was drawn and the tab cut it to
        // "CORE_rec_h".
        std::string name = e.ShownName();
        while (!name.empty() && m_style.lite.Measure(name) > kTabW - 8) name.pop_back();
        // MILESTONE 19: gold, for the same reason as the mode name -- the
        // capture's tab labels ("Mick", "Farsight", "Stitch", f0639) are the
        // same warm colour as the mode names, not green.
        const bool down = e.dead || e.hp <= 0;
        m_style.lite.DrawAligned(c, tx, ty + 4, tx + kTabW, ty + 19, name,
                                 down ? HalfBright(m_style.bosText) : m_style.bosText, 2);
        // The bar: x from 8 to 8 + min(current, max), i.e. one pixel per point,
        // clamped to the tab.
        const int32_t maxw = kTabW - 16;
        const int32_t lit = e.maxHp > 0
                                ? std::max(0, std::min(maxw, e.hp * maxw / e.maxHp))
                                : 0;
        c->Fill(tx + 8, ty + 21, tx + 8 + maxw, ty + 24, m_style.panelDkShadow);
        // `GetThresholdColor(field 0x114)` picks the colour; the thresholds
        // themselves are in no dump.  RECONSTRUCTION: green / yellow / red.
        // MEASURED off f0639: Farsight's and Stitch's full bars are a mid
        // green and Mick's low one is a warm orange-brown, not red or yellow.
        const uint32_t col = e.hp * 2 < e.maxHp ? 0xB0631Eu : 0x2A9A28u;
        c->Fill(tx + 8, ty + 21, tx + 8 + lit, ty + 24, col);
    }

    const int32_t sel = scene->Selected();
    const LiveEntity* who =
        sel >= 0 && static_cast<size_t>(sel) < ents.size() ? &ents[static_cast<size_t>(sel)]
                                                           : nullptr;

    // --- the selected character's portrait, (270, 47) 68 x 72 ---------------
    // `WGameBar::UpdateGameBar @004803a0` enables EITHER this plus the two
    // hands OR the six-portrait squad panel (`gui/back/game_bar_panel.zar` at
    // (261, 29)); the single-character arrangement is the one the in-game
    // screenshot shows and the one drawn here.
    if (who != nullptr) {
        const std::string key = who->displayName.empty() ? std::string("no_char")
                                                         : who->displayName;
        const Art* p = Res()->Image("gui/char/" + key + ".zar");
        if (!p->ok) p = Res()->Image("gui/char/no_char.zar");
        if (p->ok) {
            // scale 0.85f, FUN_004bc000(0x3f59999a) in the ctor
            const int32_t pw = static_cast<int32_t>(p->w * 0.85f);
            const int32_t ph = static_cast<int32_t>(p->h * 0.85f);
            BlitPortraitCrt(c, *p, bx + kPortraitX + (kPortraitW - pw) / 2,
                            by + kPortraitY + (kPortraitH - ph) / 2, pw, ph);
        }
        if (!who->rank.empty()) {
            const Art* r = Res()->Image("gui/rank/" + who->rank + ".zar");
            if (r->ok) c->Blit(*r, bx + kPortraitX + kPortraitW - 30, by + kPortraitY);
        }
        // hitPoints and armorClass, (273, 102) and (273, 112), 73 x 10, under
        // the keys `bar_hitPoints` and `bar_armorClass`, whose shipped values
        // in `locale/game/stat.txt` are the bare units "HP" and "AC".
        // MILESTONE 18: the real game reads "35HP" / "22AC" -- value first,
        // unit appended, one line each, no maximum -- which is the `"%d%s"` at
        // 0089b84c, the same format the AP line uses, NOT `"%d"` with the key
        // in front.  Read off the reference capture (refvideo frame f0350,
        // 1920 x 1080, the bar's portrait well).
        m_style.normal.DrawAligned(c, bx + kHpX, by + kHpY, bx + kHpX + kStatW,
                                   by + kHpY + kStatH,
                                   Fmt("%d%s", who->hp, m_loc.Get("bar_hitPoints").c_str()),
                                   m_style.buttonText, 0);
        m_style.normal.DrawAligned(c, bx + kHpX, by + kAcY, bx + kHpX + kStatW,
                                   by + kAcY + kStatH,
                                   Fmt("%d%s", who->attr.armorClass,
                                       m_loc.Get("bar_armorClass").c_str()),
                                   m_style.buttonText, 0);

        // --- the action points, WActionPoints at (367, 33) ------------------
        // `gui/misc/ap_dot_back.zar` and `ap_dot_front.zar`, both 8 x 8.  The
        // dot pitch is not in any dump; 8 px, i.e. the art's own width, is the
        // only value that does not overlap.  RECONSTRUCTION.
        const Art* dotBack = Res()->Image("gui/misc/ap_dot_back.zar");
        const Art* dotFront = Res()->Image("gui/misc/ap_dot_front.zar");
        const int32_t maxAp = std::min(who->maxAp, 14);
        // MILESTONE 19: the lit dot is TINTED.  `ap_dot_front.zar` is a plain
        // WHITE dot -- ours read (227,227,227) against the capture's
        // (109,229,91) -- and (109,229,91) is `def_style.cfg`'s
        // `color_buttonText` (60,248,0) through the capture's JPEG, so the
        // widget multiplies the art by that colour.  The unlit dot keeps the
        // art as it is.
        const float tr = static_cast<float>((m_style.buttonText >> 16) & 0xff) / 255.0f;
        const float tg = static_cast<float>((m_style.buttonText >> 8) & 0xff) / 255.0f;
        const float tb = static_cast<float>(m_style.buttonText & 0xff) / 255.0f;
        for (int32_t i = 0; i < maxAp; ++i) {
            const int32_t dx = bx + kApX + i * 8, dy = by + kApY;
            if (i < static_cast<int32_t>(who->ap)) {
                c->BlitTinted(*dotFront, dx, dy, tr, tg, tb);
            } else {
                c->Blit(*dotBack, dx, dy);
            }
        }

        DrawHandSlot(scene, c, sel, bx + kHand0X, by + kHandY, 0);
        DrawHandSlot(scene, c, sel, bx + kHand1X, by + kHandY, 1);
    }

    // --- the sentry column and the stance column ---------------------------
    // MILESTONE 19: three sentry plates, exactly one lit.
    for (int32_t i = 0; i < 3; ++i) {
        const bool on = who != nullptr && who->sentry == kSentryButtons[i].mode;
        const std::string art =
            std::string("gui/btn/") + kSentryButtons[i].art + (on ? "_Dn.zar" : "_Up.zar");
        const int32_t sx = bx + kSentryX, sy = by + kColY[i];
        c->Blit(*Res()->Image(art), sx, sy);
        if (m_mouseX >= sx && m_mouseX < sx + kSentryW && m_mouseY >= sy &&
            m_mouseY < sy + kSentryH) {
            m_tooltip = HelpText(kSentryButtons[i].help);
        }
    }
    for (int32_t i = 0; i < 3; ++i) {
        const bool on = who != nullptr && who->posture == kStanceButtons[i].art;
        const std::string art =
            std::string("gui/btn/") + kStanceButtons[i].art + (on ? "_Dn.zar" : "_Up.zar");
        const int32_t sx = bx + kStanceX, sy = by + kColY[i];
        c->Blit(*Res()->Image(art), sx, sy);
        if (m_mouseX >= sx && m_mouseX < sx + kStanceW && m_mouseY >= sy &&
            m_mouseY < sy + kStanceH) {
            m_tooltip = HelpText(kStanceButtons[i].help);
        }
    }

    // --- SKL / INV / CHR / PIP / MNU, (744, 7 + 23k), gbar_std 49 x 22 ------
    for (int32_t i = 0; i < 5; ++i) {
        const int32_t x = bx + kBtnX, y = by + kBtnY + i * kBtnPitch;
        const bool down =
            (i == 0 && m_screen == UiScreen::Skilldex) ||
            (i == 1 && m_screen == UiScreen::Inventory) ||
            (i == 2 && m_screen == UiScreen::Character) ||
            (i == 3 && m_screen == UiScreen::Pipboy) || (i == 4 && m_screen == UiScreen::Menu);
        c->Blit(*Res()->Image(down ? "gui/btn/gbar_std_dn.zar" : "gui/btn/gbar_std_up.zar"), x, y);
        // MILESTONE 19: the caption is WHITE.  `color_buttonText` (60,248,0)
        // is `def_style.cfg`'s generic button colour and this port used it
        // here, but the reference capture's glyph cores are (255,254,251) --
        // pure white -- on all five (f0639, bar-design x 744, y 7 + 23k).
        m_style.normal.DrawAligned(c, x, y, x + kBtnW, y + kBtnH, kBarButtons[i].caption,
                                   0xFFFFFFu, 2);
        if (m_mouseX >= x && m_mouseX < x + kBtnW && m_mouseY >= y && m_mouseY < y + kBtnH) {
            m_tooltip = HelpText(kBarButtons[i].command);
        }
    }

    // --- turn-based: the end-turn panel -------------------------------------
    // `WEndTurnPanel` is created by the SCREEN, not the bar (`WGameMain::WGameMain@00470990`
    // l.18520), and its constructor is in no dump, so its position is
    // unrecoverable.  The art is `gui/back/end_turn.zar` (119 x 52) with
    // `gui/btn/endtrn_up.zar` (33 x 23) on it; RECONSTRUCTION: bottom right of
    // the world viewport, above the bar.
    if (scene->TurnBased()) {
        const Art* et = Res()->Image("gui/back/end_turn.zar");
        const int32_t ex = m_W - (et->ok ? et->w : 119) - 8;
        const int32_t ey = by - (et->ok ? et->h : 52) - 8;
        c->SetClip(0, 0, m_W, m_H);
        c->Blit(*et, ex, ey);
        c->Blit(*Res()->Image("gui/btn/endtrn_up.zar"), ex + 43, ey + 15);
        const int32_t active = scene->ActiveTurnEntity();
        m_style.lite.DrawAligned(c, ex, ey - 12, ex + 119, ey,
                                 active == sel ? m_loc.Get("misc_yourturn")
                                               : m_loc.Get("misc_otherturn"),
                                 m_style.buttonText, 2);
    }
    c->SetClip(0, 0, m_W, m_H);
}

// `WIIHand::Draw @00492d20` plus `WIIBase::Draw @00490b50`.
// MILESTONE 18.  `WIIBase::Draw @00490b50`.  The vertical centring is the
// binary's own formula; the horizontal one is a reconstruction (the stored
// x offset `WIIBase+0x167` is never written in any dump).
bool Ui::DrawItemIcon(Scene* scene, Canvas* c, const std::string& spritePath, bool big,
                      int32_t x, int32_t y, int32_t w, int32_t h) {
    if (scene == nullptr || spritePath.empty()) return false;
    const Bitmap* bm = nullptr;
    if (!scene->ItemIcon(spritePath, big, &bm) || bm == nullptr) return false;
    const int32_t ix = x + (w - bm->w) / 2;
    const int32_t iy = y + (h - bm->h) / 2;
    c->BlitRGBA(bm->rgba.data(), bm->w, bm->h, ix, iy);
    return true;
}

void Ui::DrawHandSlot(Scene* scene, Canvas* c, int32_t index, int32_t x, int32_t y,
                      int32_t hand) {
    const LiveEntity& e = scene->Entities()[static_cast<size_t>(index)];
    const WeaponDef& w = hand == 0 ? e.weapon : e.weapon2;
    const bool active = e.handSelected == hand;

    const Art* base = Res()->Image(hand == 0 ? "gui/btn/lhand_up.zar" : "gui/btn/rhand_up.zar");
    c->Blit(*base, x, y);
    // "not the selected hand" forces the ambient to (0.5, 0.5, 0.5).
    const float dim = active ? 1.0f : 0.5f;

    if (!w.valid) return;      // an empty slot draws only its own button art

    // The mode name, rect (6, 6)-(126, 22).  `GetUseActionName` gives the raw
    // mode name and the label is the localised `"mode_" + name`; the font is
    // style[0x28] == font_medium, dropping to style[0x24] == font_lite when the
    // measured width exceeds slotWidth - 10.
    const WeaponMode* mode = w.Mode();
    std::string modeName = mode != nullptr ? mode->modeName : std::string();
    std::string modeLabel = modeName.empty() ? std::string() : m_loc.Get("mode_" + modeName);
    for (char& ch : modeLabel) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const Font& modeFont = m_style.medium.Measure(modeLabel) > kHandW - 10 ? m_style.lite
                                                                           : m_style.medium;
    // MILESTONE 19: the mode name is `color_buttonYellow` (196,160,40), not
    // `color_buttonText` green -- "SINGLE" and "SWING" are plainly gold in the
    // reference capture (f0639, both hand cells) and buttonYellow is the only
    // warm colour `def_style.cfg` defines.
    modeFont.DrawAligned(c, x + kModeX0, y + kModeY0, x + kModeX1, y + kModeY1, modeLabel,
                         active ? m_style.buttonYellow : m_style.windowText, 1);

    // MILESTONE 18.  The item icon.  `WIIBase::Draw @00490b50` blits the item
    // ENTITY's own sprite, animation `guibig` (the 140 x 69 hand cell) or
    // `guismall` (a list row), frame 0 direction 0, UNSCALED and centred
    // vertically in the cell:
    //
    //     y = slot.iconY + (((cellBottom - cellTop) - frameH) + frameY) / 2
    //
    // The two stored offsets `WIIBase+0x167` (x) and `+0x16b` (y) are never
    // written in any dump, so the HORIZONTAL centring here is a labelled
    // reconstruction; the vertical rule is the binary's own.  The unarmed
    // punch/kick have no `Sprite` and fall back to the name, which is the
    // milestone 17 deviation this slot already carried.
    if (!DrawItemIcon(scene, c, w.spritePath, true, x, y, kHandW, kHandH)) {
        m_style.lite.DrawAligned(c, x + 4, y + 22, x + 134, y + 40, w.displayName,
                                 active ? m_style.windowText : 0x808080u, 2);
    }

    // `desc_noammo` ("RELOAD"), rect (4, 27)-(134, 43), centre-justified.
    if (w.magazine > 0 && e.ammo <= 0) {
        m_style.medium.DrawAligned(c, x + kNoAmmoX0, y + kNoAmmoY0, x + kNoAmmoX1, y + kNoAmmoY1,
                                   m_loc.Get("desc_noammo"), 0xC00000u, 2);
    }

    // The AP line, rect (7, 52)-(137, 68), format "%d%s" (0089b84c) with the
    // localised `mode_AP` ("AP").
    if (mode != nullptr) {
        // `Weapon::GetAPCost @005da3d0` resolves the mode's
        // `actionCostType` through Tables::ActionTypes; that lookup is
        // combat.cpp's `ActionTypeCost`.
        const int32_t ap = ActionTypeCost(mode->actionCostType);
        if (ap > 0) {
            m_style.medium.DrawAligned(c, x + kApTextX0, y + kApTextY0, x + kApTextX1,
                                       y + kApTextY1,
                                       Fmt("%d%s", ap, m_loc.Get("mode_AP").c_str()),
                                       active ? m_style.buttonText : m_style.windowText, 0);
        }
    }

    // The spare-ammo count, rect (7, 6)-(77, 22), format "x%d" (0089b848),
    // drawn only when the weapon has no magazine of its own.
    if (w.magazine <= 0 && e.ammoReserve > 0) {
        m_style.lite.DrawAligned(c, x + kSpareX0, y + kModeY0, x + kSpareX1, y + kModeY1,
                                 Fmt("x%d", e.ammoReserve), m_style.windowText, 0);
    }

    // The ammo bar: 39 iterations, two pixels wide at x 134 and 135, marching
    // from y = 44, one lit tick every second iteration -- twenty visible ticks.
    if (w.magazine > 0) {
        const float frac = static_cast<float>(e.ammo) / static_cast<float>(w.magazine);
        const int32_t ticks = static_cast<int32_t>(std::lround(frac * kAmmoBarTicks));
        for (int32_t i = 0; i < kAmmoBarTicks; ++i) {
            if ((i & 1) != 0) continue;
            const uint32_t col = i < ticks ? m_style.buttonText : m_style.panelShadow;
            c->Pixel(x + kAmmoBarX, y + kAmmoBarY - i, col, 255);
            c->Pixel(x + kAmmoBarX + 1, y + kAmmoBarY - i, col, 255);
        }
    }

    // The called-shot checkbox, rect (117, 47)-(138, 66); enabled only when the
    // mode `cancallshot`s (`WIIHand::Update @004924b0`).
    if (mode != nullptr && mode->canCallShot) {
        c->Blit(*Res()->Image("gui/btn/check_up.zar"), x + kCheckX, y + kCheckY);
    }

    if (m_mouseX >= x && m_mouseX < x + kHandW && m_mouseY >= y && m_mouseY < y + kHandH) {
        // `WIIBase::BuildItemTooltipText @00490360`: "%s<Cw>x%d<C_>" -- the
        // name, then a colour-shifted count.  The colour escapes are dropped.
        m_tooltip = w.displayName;
        if (w.magazine > 0) m_tooltip += Fmt("  %d/%d", e.ammo, w.magazine);
        if (e.ammoReserve > 0) m_tooltip += Fmt(" x%d", e.ammoReserve);
    }
    (void)dim;
}

// The minimap -- VIDEO_GAPS M7.
//
// `WMinimapDlg` is a call site only: its world-to-map projection is NOT in any
// dump.  What ships, and what milestone 22 found, is the whole widget's art:
// the rect is (10, 15) 214 x 100, `gui/misc/Minimap_Noise0000..0004.zar` are
// five 214 x 100 overlays -- exactly the rect -- `gui/misc/mmap/circle1..5.zar`
// are the five 7 x 7 blips (one per squad slot), and `gui/misc/pmap/small`,
// `medium`, `large` (20 / 40 / 60 px rings, each with an `S` twin) are the
// three zoom steps.
//
// The capture (f0348, f0161) settles what it looks like: a BLACK rect carrying
// a bright-green VECTOR WIREFRAME of the walls and buildings -- long thin
// diagonal runs where a fence or wall goes, filled diamonds where a building
// footprint is -- with white blips for the selected squad, grey for the rest,
// red for a hostile, and a small cream `+` in the top-right corner and `-` in
// the bottom-right.  Milestone 21 and earlier drew the map's FOOTPRINT: a solid
// filled green diamond of every walkable cell, which is the opposite picture.
//
// The wireframe source is `NavGrid::stat`, the static-blocker plane milestone 8
// already builds from the walls and tall objects -- so a wall run is a
// one-cell-wide line and a building is a filled block, which is precisely the
// two shapes the capture shows.  The projection is the world's own
// (`mapview/iso.h`: screenX = 6(x - z), screenY = -3(x + z)) so the minimap is
// the same shape as the view, CENTRED ON THE CAMERA and scaled by the zoom.
// The three scales and the default are a RECONSTRUCTION measured off f0348: the
// fenced compound spans about a third of the widget there, which puts the
// middle step at about one minimap pixel per ten world-screen pixels.
namespace {
// One minimap pixel per N world-screen pixels, small / medium / large.
constexpr double kMiniZoom[3] = {1.0 / 16.0, 1.0 / 10.0, 1.0 / 6.0};
}  // namespace

void Ui::DrawMinimap(Scene* scene, Canvas* c, int32_t bx, int32_t by) {
    const NavGrid& n = scene->Nav();
    const int32_t x0 = bx + kMiniX, y0 = by + kMiniY;
    c->Fill(x0, y0, x0 + kMiniW, y0 + kMiniH, m_style.panelDkShadow);
    if (n.nx <= 0 || n.nz <= 0) return;
    const double scale = kMiniZoom[m_miniZoom < 0 ? 0 : (m_miniZoom > 2 ? 2 : m_miniZoom)];
    // The world-screen point under the middle of the viewport.
    const double ccx = static_cast<double>(m_camX) + m_W * 0.5;
    const double ccy = static_cast<double>(m_camY) + ViewportHeight(m_H) * 0.5;
    // The projection carries an elevation term (screenY = -3(x + z) - 7y), so
    // inverting a screen point needs a reference elevation or the whole map
    // lands 7 * y pixels away from the camera.  The reference is the selected
    // actor's own -- one map is one elevation band almost everywhere.
    const std::vector<LiveEntity>& allEnts = scene->Entities();
    const int32_t selIdx = scene->Selected();
    const double refY = selIdx >= 0 && static_cast<size_t>(selIdx) < allEnts.size()
                            ? static_cast<double>(allEnts[static_cast<size_t>(selIdx)].world[1])
                            : 0.0;

    // The wireframe.  Every minimap pixel is inverse-projected to a world cell
    // and lit when that cell carries a static blocker; `statTop` picks the two
    // brightnesses the capture shows (a solid run for a wall, a dimmer one for
    // a fence or a low object).
    for (int32_t py = 0; py < kMiniH; ++py) {
        for (int32_t px = 0; px < kMiniW; ++px) {
            const double sx = ccx + (px - kMiniW * 0.5) / scale;
            const double sy = ccy + (py - kMiniH * 0.5) / scale;
            // screenX = 6(x - z), screenY = -3(x + z)  ->  invert.
            const double u = sx / 6.0, v = -(sy + ft::kIsoHeight * refY) / 3.0;
            const int32_t wx = static_cast<int32_t>(std::lround((u + v) * 0.5));
            const int32_t wz = static_cast<int32_t>(std::lround((v - u) * 0.5));
            const int32_t cx = n.CellX(wx), cz = n.CellZ(wz);
            if (!n.In(cx, cz)) continue;
            const size_t i = n.Idx(cx, cz);
            if (n.stat[i] == 0) continue;
            const int32_t top = n.statTop[i];
            const int32_t floor = n.surface[i] == kNoFloor ? 0 : n.surface[i];
            const bool tall = top != kNoFloor && top - floor >= 6;
            const uint32_t col = tall ? 0x22FF11u : 0x0E9A08u;
            c->Pixel(x0 + px, y0 + py, col, 255);
        }
    }

    // The blips.  `circle1..5.zar` are one per squad slot; the capture draws
    // the selected member's white, the rest grey and a hostile red, so the art
    // is blitted TINTED, the way `ap_dot_front.zar` is (NOTES.md 106).
    const Art* blip = Res()->Image("gui/misc/mmap/circle1.zar");
    const std::vector<int32_t> squad = SquadList(scene);
    const std::vector<LiveEntity>& ents = allEnts;
    const int32_t me = squad.empty() ? -1 : squad.front();
    auto plot = [&](const LiveEntity& e, uint32_t rgb) {
        int32_t sx = 0, sy = 0;
        ft::ProjectWorldToScreen(e.world[0], e.world[1], e.world[2], &sx, &sy);
        const int32_t px = static_cast<int32_t>(std::lround((sx - ccx) * scale + kMiniW * 0.5));
        const int32_t py = static_cast<int32_t>(std::lround((sy - ccy) * scale + kMiniH * 0.5));
        if (px < 3 || py < 3 || px >= kMiniW - 3 || py >= kMiniH - 3) return;
        if (blip->ok) {
            c->BlitTinted(*blip, x0 + px - 3, y0 + py - 3,
                          ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                          (rgb & 0xFF) / 255.0f);
        } else {
            c->Fill(x0 + px - 2, y0 + py - 2, x0 + px + 3, y0 + py + 3, rgb);
        }
    };
    for (size_t i = 0; i < ents.size(); ++i) {
        const LiveEntity& e = ents[i];
        if (!e.isCombatant || e.dead) continue;
        const bool mine = std::find(squad.begin(), squad.end(), static_cast<int32_t>(i)) !=
                          squad.end();
        if (mine) {
            plot(e, scene->IsSelected(static_cast<int32_t>(i)) ? 0xF0F0F0u : 0x969696u);
        } else if (me >= 0 && scene->IsHostile(me, static_cast<int32_t>(i))) {
            plot(e, 0xE01818u);
        }
    }

    // The zoom marks: a 5 x 5 `+` in the top-right corner and a 5 x 1 `-` in
    // the bottom-right, both in the menu's khaki-cream (NOTES.md 111).
    // MEASURED off f0348: the `+`'s centre is 8 px in from the right edge and
    // 8 px down; the `-` is the same x, 5 px up from the bottom.
    const uint32_t mark = m_style.bosText;
    const int32_t mx = x0 + kMiniW - 8, myTop = y0 + 8, myBot = y0 + kMiniH - 5;
    c->Fill(mx - 2, myTop, mx + 3, myTop + 1, mark);
    c->Fill(mx, myTop - 2, mx + 1, myTop + 3, mark);
    c->Fill(mx - 2, myBot, mx + 3, myBot + 1, mark);
    m_miniRect[0] = x0;
    m_miniRect[1] = y0;
    m_miniRect[2] = x0 + kMiniW;
    m_miniRect[3] = y0 + kMiniH;
}

// The `WTextList` that shares the minimap's rect; the map/text checkboxes at
// (233, 49) and (233, 79) are its toggle.  `CombatMessage`'s own routing into
// it is not in the dumps; this shows the newest lines of the combat console.
void Ui::DrawMessages(Canvas* c, int32_t bx, int32_t by) {
    const int32_t x0 = bx + kMiniX, y0 = by + kMiniY;
    c->Fill(x0, y0, x0 + kMiniW, y0 + kMiniH, m_style.windowFill, 200);
    const int32_t sx0 = c->clip[0], sy0 = c->clip[1], sx1 = c->clip[2], sy1 = c->clip[3];
    c->SetClip(x0, y0, x0 + kMiniW, y0 + kMiniH);
    const int32_t lh = m_style.lite.Height() + 1;
    const int32_t rows = kMiniH / lh;
    const int32_t first = std::max<int32_t>(0, static_cast<int32_t>(m_messages.size()) - rows);
    for (int32_t i = first; i < static_cast<int32_t>(m_messages.size()); ++i) {
        m_style.lite.Draw(c, x0 + 3, y0 + (i - first) * lh, m_messages[static_cast<size_t>(i)],
                          m_style.consoleGreen);
    }
    c->SetClip(sx0, sy0, sx1, sy1);
}

// ---------------------------------------------------------------------------
// The inventory.
// ---------------------------------------------------------------------------
void Ui::DrawInventory(Scene* scene, Canvas* c) {
    const Art* back = Res()->Image("gui/back/equip.zar");
    const Rect r = Modal(back->ok ? back->w : 800, back->ok ? back->h : 600);
    c->SetClip(r.x0, r.y0, r.x1, r.y1);
    if (back->ok) c->Blit(*back, r.x0, r.y0);

    const int32_t sel = scene->Selected();
    m_invRows.clear();
    m_slotRects.clear();
    if (sel < 0 || static_cast<size_t>(sel) >= scene->Entities().size()) {
        c->SetClip(0, 0, m_W, m_H);
        return;
    }
    const LiveEntity& e = scene->Entities()[static_cast<size_t>(sel)];

    // The three equip wells.  The equip SCREEN's own class is in no dump, so
    // these rects are MEASURED OFF `equip.zar` (see ui.h): three recessed wells
    // in the left column at (33, 172), (32, 275) and (33, 378), each 139 x 69 --
    // which is exactly the `WIIHand` cell the game bar uses, so slots 0 and 1
    // are the hands and slot 2, the one the paperdoll is driven from
    // (`slot_equip[2]`, `Actor::UpdateSprite@00540e50`), is the armour.
    static const int32_t kWell[3][2] = {{33, 172}, {32, 275}, {33, 378}};
    static const char* const kWellKey[3] = {"equipMenu_hand1", "equipMenu_hand2",
                                            "equipMenu_armor"};
    for (int i = 0; i < 3; ++i) {
        Rect w;
        w.x0 = r.x0 + kWell[i][0];
        w.y0 = r.y0 + kWell[i][1];
        w.x1 = w.x0 + 139;
        w.y1 = w.y0 + 68;
        m_slotRects.push_back(w);
        std::string label, sub;
        if (i == 0 && e.weapon.valid) { label = e.weapon.displayName; sub = e.weapon.itemType; }
        if (i == 1 && e.weapon2.valid) { label = e.weapon2.displayName; sub = e.weapon2.itemType; }
        if (i == 2 && e.armour.valid) { label = e.armour.displayName; sub = e.armour.itemType; }
        const bool over = m_mouseX >= w.x0 && m_mouseX < w.x1 && m_mouseY >= w.y0 &&
                          m_mouseY < w.y1;
        if (over) c->Fill(w.x0, w.y0, w.x1, w.y1, m_style.selectionFill, 30);
        std::string wellSprite;
        if (i == 0 && e.weapon.valid) wellSprite = e.weapon.spritePath;
        if (i == 1 && e.weapon2.valid) wellSprite = e.weapon2.spritePath;
        if (i == 2 && e.armour.valid) wellSprite = e.armour.spritePath;
        if (label.empty()) {
            m_style.lite.DrawAligned(c, w.x0, w.y0, w.x1, w.y1, m_loc.Get(kWellKey[i]),
                                     0x606060u, 2);
        } else {
            // MILESTONE 18: the well IS a `WIIHand` cell (139 x 69 measured off
            // `equip.zar`, the same 140 x 69 the game bar uses), so it carries
            // the same `guibig` icon `WIIBase::Draw @00490b50` draws.
            if (!DrawItemIcon(scene, c, wellSprite, true, w.x0, w.y0, w.x1 - w.x0,
                              w.y1 - w.y0)) {
                m_style.medium.DrawAligned(c, w.x0 + 4, w.y0 + 8, w.x1 - 4, w.y0 + 30, label,
                                           m_style.buttonText, 2);
            }
            m_style.lite.DrawAligned(c, w.x0 + 4, w.y1 - 16, w.x1 - 4, w.y1 - 2, sub,
                                     m_style.windowText, 2);
            if (over) m_tooltip = label;
        }
    }

    // The paperdoll well, measured at (421, 29) 179 x 246.
    {
        const std::string key = e.displayName.empty() ? std::string("no_char") : e.displayName;
        const Art* p = Res()->Image("gui/char/" + key + ".zar");
        if (!p->ok) p = Res()->Image("gui/char/no_char.zar");
        if (p->ok) BlitScaled(c, *p, r.x0 + 421 + (179 - p->w * 2) / 2, r.y0 + 40, p->w * 2,
                              p->h * 2);
        m_style.medium.DrawAligned(c, r.x0 + 421, r.y0 + 290, r.x0 + 600, r.y0 + 312, e.ShownName(),
                                   m_style.buttonText, 2);
        m_style.lite.DrawAligned(c, r.x0 + 421, r.y0 + 314, r.x0 + 600, r.y0 + 328,
                                 m_loc.Get("equipMenu_HP") + Fmt(" %d/%d", e.hp, e.maxHp),
                                 m_style.windowText, 2);
        m_style.lite.DrawAligned(c, r.x0 + 421, r.y0 + 328, r.x0 + 600, r.y0 + 342,
                                 m_loc.Get("equipMenu_AC") + Fmt(" %d", e.attr.armorClass),
                                 m_style.windowText, 2);
        // `equipMenu_weight` ("Total Wt:") is a shipped key, so the equip
        // screen the dumps are missing did carry a weight readout.  The only
        // encumbrance signal that IS in the code is the boolean
        // `Entity::IsEncumbered()` (`WCharacter::UpdateCharacterStatusEffectsDisplay@004612d0`), and the carry
        // capacity is `ST * 25` from `Attributes::SumFinalAttributes @006088d0`.
        float wt = e.weapon.weight + e.weapon2.weight;
        for (const LiveEntity::InvItem& it : e.inventory) {
            wt += it.weight * static_cast<float>(it.count);
        }
        m_style.lite.DrawAligned(c, r.x0 + 421, r.y0 + 342, r.x0 + 600, r.y0 + 356,
                                 m_loc.Get("equipMenu_weight") +
                                     Fmt(" %.0f/%d", static_cast<double>(wt),
                                         e.attr.strength * 25),
                                 m_style.windowText, 2);
    }

    // The carried items.  FT's container view is a scrolling LIST, not a grid
    // (`WIIBase` derives from `WListItem`; the panes are `WInvActor`, whose
    // body is in no dump), and `equip.zar` shows two tall list wells with
    // header strips at (225, 35) 168 x 399 and (625, 35) 169 x 399.  The left
    // one is the actor's own inventory.
    const int32_t lx0 = r.x0 + 225, ly0 = r.y0 + 35, lx1 = r.x0 + 393, ly1 = r.y0 + 434;
    m_style.lite.DrawAligned(c, r.x0 + 226, r.y0 + 12, r.x0 + 387, r.y0 + 33,
                             m_loc.Get("equipMenu_inventory"), m_style.buttonText, 2);
    // MILESTONE 18: the row is `guismall`'s own cell.  `WInvActor` is a
    // `WListItem` walk with no body in any dump, so the height is still a
    // RECONSTRUCTION -- but it is now sized to the animation the rows carry
    // (`Combat Knife.spr`'s `guismall` is 63 x 26) instead of the 24 px
    // milestone 17 guessed.
    const int32_t rowH = 28;
    for (size_t i = 0; i < e.inventory.size(); ++i) {
        const int32_t ry = ly0 + static_cast<int32_t>(i) * rowH - m_invScroll;
        if (ry + rowH < ly0 || ry > ly1) continue;
        Rect row;
        row.x0 = lx0; row.y0 = ry; row.x1 = lx1; row.y1 = ry + rowH;
        m_invRows.push_back(row);
        const bool over = m_mouseX >= row.x0 && m_mouseX < row.x1 && m_mouseY >= row.y0 &&
                          m_mouseY < row.y1;
        if (over) {
            c->Fill(row.x0, row.y0, row.x1, row.y1, m_style.selectionFill, 40);
            // `WIIBase::BuildItemTooltipText`: name, then "x<count>".
            m_tooltip = e.inventory[i].displayName;
            if (e.inventory[i].count >= 2) m_tooltip += Fmt("  x%d", e.inventory[i].count);
        }
        {
            const int32_t cx0 = c->clip[0], cy0 = c->clip[1], cx1 = c->clip[2],
                          cy1 = c->clip[3];
            c->SetClip(std::max(cx0, row.x0), std::max(cy0, row.y0),
                       std::min(cx1, row.x0 + 66), std::min(cy1, row.y1));
            DrawItemIcon(scene, c, e.inventory[i].spritePath, false, row.x0 + 2, row.y0, 64,
                         rowH);
            c->SetClip(cx0, cy0, cx1, cy1);
        }
        m_style.lite.Draw(c, row.x0 + 70, ry + 8, e.inventory[i].displayName,
                          over ? m_style.buttonText : m_style.windowText);
        // The count strip.  `WIIBase::Draw` puts it at y 62..74 of a `guibig`
        // cell, right-aligned, and only when count >= 2.
        if (e.inventory[i].count >= 2) {
            m_style.lite.DrawAligned(c, row.x0, ry + 6, row.x1 - 6, ry + 18,
                                     Fmt("x%d", e.inventory[i].count), m_style.windowText, 1);
        }
    }
    if (e.inventory.empty()) {
        m_style.lite.DrawAligned(c, lx0, ly0 + 10, lx1, ly0 + 26, std::string(),
                                 0x606060u, 2);
    }
    // The right well is the barter/ground pane; the barter table is out of
    // scope for this milestone and it is drawn empty and labelled.
    m_style.lite.DrawAligned(c, r.x0 + 625, r.y0 + 12, r.x0 + 787, r.y0 + 33,
                             m_loc.Get("equipMenu_barter"), 0x606060u, 2);
    c->SetClip(0, 0, m_W, m_H);
}


// ---------------------------------------------------------------------------
// MILESTONE 17: the loot window.
// ---------------------------------------------------------------------------
// Two windows side by side, both of them decompiled in full this milestone:
//
//   `WLoot::WLoot @00495940` -- a `WPicture` on `gui/back/rloot`
//   (`s_gui_back_rloot_00898e60`), carrying
//     * a `WInvActor` list at rect (0xd3, 0xe)-(0x187, 0x1ca) = (211, 14)-(391,
//       458), the container's own contents;
//     * a `WEntityDisplay` at (0x20, 0x24)-(0xaa, 0xa4) = (32, 36)-(170, 164),
//       the picture of the thing being looted;
//     * a `WBOSButton` at (0x2a, 0x1b0) = (42, 432) captioned with the
//       localised `equipMenu_takeAll` -- "TAKE ALL".
//
//   `WMultiEquipLoot::WMultiEquipLoot @004962a0` -- a bare `WWindow` at
//   (400, 0)-(800, 0x1d8) = (400, 0)-(800, 472) carrying the SAME `WInvActor`
//   rect (211, 14)-(391, 458), relative to its own origin, plus a
//   `WItemDisplay` at (0x20, 0x24)-(0xb4, 0x10e) = (32, 36)-(180, 270).  That
//   is the looting actor's own inventory.
//
// So the screen is 800 x 472 -- the same size as `equip.zar` -- and the two
// list wells land at absolute x 211..391 and 611..791.  `rloot.zar` is the
// left half's art; the right half has no picture of its own in the
// constructor, so the same art is mirrored under it here and that is the one
// thing on this screen that is a PORT CHOICE rather than a read literal.
//
// `WInvActor::RebuildInventoryItemList @00494760` is the list itself and is a
// `WListItem` walk, not a grid -- the same finding as section 32.  Row height
// is not in it (the child windows' rects come from `WIIBase::WIIBase`'s own
// `guibig`/`guismall` cell, which this port does not decode); 24 px is the
// equip screen's and is carried over, LABELLED.
void Ui::DrawLoot(Scene* scene, Canvas* c) {
    m_lootRowsL.clear();
    m_lootRowsR.clear();
    m_lootTakeAll = Rect();
    m_lootDone = Rect();
    const int32_t target = scene->LootTarget();
    const int32_t actor = scene->LootActor();
    const std::vector<LiveEntity>& ents = scene->Entities();
    if (target < 0 || actor < 0 || static_cast<size_t>(target) >= ents.size() ||
        static_cast<size_t>(actor) >= ents.size()) {
        m_screen = UiScreen::None;
        return;
    }
    const LiveEntity& box = ents[static_cast<size_t>(target)];
    const LiveEntity& who = ents[static_cast<size_t>(actor)];

    // The backdrop.  `WLoot`'s own `WPicture` names `gui/back/rloot`
    // (`s_gui_back_rloot_00898e60`) and `WMultiEquipLoot` names no picture at
    // all -- it is a bare `WWindow` at (400, 0)-(800, 472).  The shipped art
    // settles what belongs under it: `rloot.zar` is 401 x 473 -- HALF the
    // 800 x 472 the two windows span -- and the only other panel of exactly
    // that size in `gui/back/` is `lequip.zar`, 401 x 473, which is not
    // referenced by any decompiled function either.  The two together are the
    // screen.  Which of the two goes on which side is INFERRED from the window
    // rects, not read: `WLoot`, the container, is the left one because its
    // `WBOSButton` sits at (42, 432) inside its own window, and the Take All
    // recess in `rloot.zar` is at exactly that spot.
    const Art* left = Res()->Image("gui/back/rloot.zar");
    const Art* right = Res()->Image("gui/back/lequip.zar");
    const int32_t w = 800, h = 472;
    const Rect r = Modal(w, h);
    c->SetClip(r.x0, r.y0, r.x1, r.y1);
    if (left->ok) c->Blit(*left, r.x0, r.y0);
    else c->Fill(r.x0, r.y0, r.x0 + 400, r.y1, 0x201c14u, 255);
    if (right->ok) c->Blit(*right, r.x0 + 400, r.y0);
    else c->Fill(r.x0 + 400, r.y0, r.x1, r.y1, 0x201c14u, 255);

    auto drawList = [&](int32_t ox, const std::vector<LiveEntity::InvItem>& items,
                        std::vector<Rect>* rows, int32_t scroll) {
        const int32_t lx0 = r.x0 + ox + 211, ly0 = r.y0 + 14;
        const int32_t lx1 = r.x0 + ox + 391, ly1 = r.y0 + 458;
        const int32_t rowH = 28;   // the equip screen's; see the note above
        for (size_t i = 0; i < items.size(); ++i) {
            const int32_t ry = ly0 + 6 + static_cast<int32_t>(i) * rowH - scroll;
            if (ry + rowH < ly0 || ry > ly1) continue;
            Rect row;
            row.x0 = lx0; row.y0 = ry; row.x1 = lx1; row.y1 = ry + rowH;
            rows->push_back(row);
            const bool over = m_mouseX >= row.x0 && m_mouseX < row.x1 && m_mouseY >= row.y0 &&
                              m_mouseY < row.y1;
            if (over) {
                c->Fill(row.x0, row.y0, row.x1, row.y1, m_style.selectionFill, 40);
                // `WIIBase::BuildItemTooltipText @00490360`: "%s<Cw>x%d<C_>".
                m_tooltip = items[i].displayName;
                if (items[i].count >= 2) m_tooltip += Fmt("  x%d", items[i].count);
            }
            {   // MILESTONE 18: `WIIBase::Draw`'s `guismall` icon.
                const int32_t cx0 = c->clip[0], cy0 = c->clip[1], cx1 = c->clip[2],
                              cy1 = c->clip[3];
                c->SetClip(std::max(cx0, row.x0), std::max(cy0, row.y0),
                           std::min(cx1, row.x0 + 66), std::min(cy1, row.y1));
                DrawItemIcon(scene, c, items[i].spritePath, false, row.x0 + 2, row.y0, 64, rowH);
                c->SetClip(cx0, cy0, cx1, cy1);
            }
            m_style.lite.Draw(c, row.x0 + 70, ry + 8, items[i].displayName,
                              over ? m_style.buttonText : m_style.windowText);
            if (items[i].count >= 2) {
                m_style.lite.DrawAligned(c, row.x0, ry + 8, row.x1 - 6, ry + 20,
                                         Fmt("x%d", items[i].count), m_style.windowText, 1);
            }
        }
        if (items.empty()) {
            m_style.lite.DrawAligned(c, lx0, ly0 + 10, lx1, ly0 + 26, m_loc.Get("log_empty"),
                                     0x606060u, 2);
        }
    };

    // The left half: what is being looted.  `WEntityDisplay` at (32, 36) shows
    // the entity; this port has no item-sprite path into the UI (section 35's
    // standing placeholder) and puts the name and the weight there instead.
    drawList(0, box.inventory, &m_lootRowsL, m_lootScrollL);
    {
        float wt = 0.0f;
        for (const LiveEntity::InvItem& it : box.inventory) {
            wt += it.weight * static_cast<float>(it.count);
        }
        m_style.medium.DrawAligned(c, r.x0 + 32, r.y0 + 36, r.x0 + 170, r.y0 + 60,
                                   box.ShownName(), m_style.buttonText, 2);
        m_style.lite.DrawAligned(c, r.x0 + 32, r.y0 + 62, r.x0 + 170, r.y0 + 78,
                                 box.isContainer ? m_loc.Get("ent_isLocked").empty()
                                                       ? std::string("Container")
                                                       : std::string("Container")
                                                 : (box.dead ? std::string("Corpse")
                                                             : std::string("Item")),
                                 m_style.windowText, 2);
        if (box.isContainer && box.maxWeight > 0) {
            // `Container.maxweight` -- the one weight limit the game enforces
            // (`Container::AddToInventory @005ac050`, `log_tooHeavy`).
            m_style.lite.DrawAligned(c, r.x0 + 32, r.y0 + 80, r.x0 + 170, r.y0 + 96,
                                     m_loc.Get("equipMenu_weight") +
                                         Fmt(" %.0f/%d", static_cast<double>(wt), box.maxWeight),
                                     m_style.windowText, 2);
        }
    }
    // `equipMenu_takeAll` at (42, 432).
    {
        const Art* up = Res()->Image("gui/btn/med_up.zar");
        m_lootTakeAll.x0 = r.x0 + 42;
        m_lootTakeAll.y0 = r.y0 + 432;
        m_lootTakeAll.x1 = m_lootTakeAll.x0 + (up->ok ? up->w : 120);
        m_lootTakeAll.y1 = m_lootTakeAll.y0 + (up->ok ? up->h : 24);
        if (up->ok) c->Blit(*up, m_lootTakeAll.x0, m_lootTakeAll.y0);
        m_style.medium.DrawAligned(c, m_lootTakeAll.x0, m_lootTakeAll.y0, m_lootTakeAll.x1,
                                   m_lootTakeAll.y1, m_loc.Get("equipMenu_takeAll"),
                                   m_style.buttonText, 2);
    }

    // The right half: the looting actor.  `WItemDisplay` at (32, 36).
    drawList(400, who.inventory, &m_lootRowsR, m_lootScrollR);
    {
        m_style.medium.DrawAligned(c, r.x0 + 432, r.y0 + 36, r.x0 + 580, r.y0 + 60,
                                   who.ShownName(), m_style.buttonText, 2);
        // `Actor::GetWeight @005550c0` against `ST * 25`
        // (`Attributes::SumFinalAttributes @006088d0`).  An actor is never
        // REFUSED a transfer -- `Actor::AddToInventory @00555180` only raises
        // the `encumbered` event -- so this is a readout, not a gate.
        m_style.lite.DrawAligned(c, r.x0 + 432, r.y0 + 62, r.x0 + 580, r.y0 + 78,
                                 m_loc.Get("equipMenu_weight") +
                                     Fmt(" %.0f/%d",
                                         static_cast<double>(scene->CarriedWeight(who)),
                                         scene->CarryCapacity(who)),
                                 m_style.windowText, 2);
        if (scene->IsEncumbered(who)) {
            m_style.lite.DrawAligned(c, r.x0 + 432, r.y0 + 80, r.x0 + 580, r.y0 + 96,
                                     m_loc.Get("ent_encumbered"), 0xc04040u, 2);
        }
    }
    // A Done button.  `WLoot` has no close button of its own -- the original
    // closes the window from `WLoot::Update @00495ce0`, which watches the
    // container's `Action` string and calls `SetActiveGameWindowMode(0)` the
    // moment it stops being open -- so this rect is a PORT ADDITION, put in the
    // right half's mirror of the Take All position.
    {
        const Art* up = Res()->Image("gui/btn/med_up.zar");
        m_lootDone.x0 = r.x0 + 442;
        m_lootDone.y0 = r.y0 + 432;
        m_lootDone.x1 = m_lootDone.x0 + (up->ok ? up->w : 120);
        m_lootDone.y1 = m_lootDone.y0 + (up->ok ? up->h : 24);
        if (up->ok) c->Blit(*up, m_lootDone.x0, m_lootDone.y0);
        m_style.medium.DrawAligned(c, m_lootDone.x0, m_lootDone.y0, m_lootDone.x1, m_lootDone.y1,
                                   m_loc.Get("gameMenu_done"), m_style.buttonText, 2);
    }
    m_trace += Fmt("loot target=%d actor=%d left=%zu right=%zu takeAll=(%d,%d,%d,%d)\n", target,
                   actor, box.inventory.size(), who.inventory.size(), m_lootTakeAll.x0,
                   m_lootTakeAll.y0, m_lootTakeAll.x1, m_lootTakeAll.y1);
    c->SetClip(0, 0, m_W, m_H);
}

// ---------------------------------------------------------------------------
// The character sheet.
// ---------------------------------------------------------------------------
void Ui::DrawCharacter(Scene* scene, Canvas* c) {
    const Art* back = Res()->Image("gui/back/CharReview.zar");
    const Rect r = Modal(800, 472);
    c->SetClip(r.x0, r.y0, r.x1, r.y1);
    if (back->ok) c->Blit(*back, r.x0, r.y0);
    const Art* fill = Res()->Image("gui/back/CharRightFill.zar");
    if (fill->ok) c->Blit(*fill, r.x0 + 600, r.y0);

    const int32_t sel = scene->Selected();
    if (sel < 0 || static_cast<size_t>(sel) >= scene->Entities().size()) {
        c->SetClip(0, 0, m_W, m_H);
        return;
    }
    const LiveEntity& e = scene->Entities()[static_cast<size_t>(sel)];
    const Attributes& a = e.attr;
    const int32_t special[7] = {a.strength,     a.perception, a.endurance, a.charisma,
                                a.intelligence, a.agility,    a.luck};

    // The seven SPECIAL rows: `BuildAttributeSpinnerControls @0045f1e0` runs
    // y = 0x3B, y += 0x20 while y < 0x11B -- seven rows, pitch 32.
    for (int32_t i = 0; i < 7; ++i) {
        const int32_t y = r.y0 + kSpecialY0 + i * kSpecialPitch;
        // the abbreviation, (AX+7, y-22)-(AX+0x24, y)
        m_style.large.DrawAligned(c, r.x0 + kCharAX + 7, y - 22, r.x0 + kCharAX + 36, y,
                                  m_loc.Get(std::string("short_") + kSpecialKeys[i]),
                                  m_style.buttonText, 0);
        // the value spinner, (AX+0x27, y-0x13)
        m_style.large.Draw(c, r.x0 + kCharAX + 39, y - 19, Fmt("%d", special[i]),
                           m_style.buttonYellow);
        // the descriptor, (AX+0x4E, y-0x12)-(AX+0x84, y+4)
        m_style.normal.DrawAligned(c, r.x0 + kCharAX + 78, y - 18, r.x0 + kCharAX + 132, y + 4,
                                   m_loc.Get(std::string("name_") + kSpecialKeys[i]),
                                   m_style.windowText, 0);
    }

    // The status column, `BuildCharacterStatComparisonRows @00460e90` loop 1:
    // nine rows at x 170..288, y = 0x2F, pitch 11, the first of which is hit
    // points formatted "%d/%d".
    m_style.normal.DrawAligned(c, r.x0 + 170, r.y0 + kStatusY0, r.x0 + 288,
                               r.y0 + kStatusY0 + 10,
                               m_loc.Get("name_maxHitPoints") + "  " +
                                   Fmt("%d/%d", e.hp, e.maxHp),
                               m_style.buttonText, 0);
    // The derived block, loop 2: twelve rows at x 170..288, y = 0xAE, pitch 11,
    // value column at +0x57, formatted "%d" or "%d%%" by the flag table.
    // MILESTONE 21: each of the twelve is now a field the sheet CARRIES, summed
    // by `Attributes::Derive` out of `Attributes::InitCopyAttributes @006080c0`
    // and `Attributes::SumFinalAttributes @006088d0`, rather than the
    // SPECIAL-derived term recomputed here -- so the race prototype's own
    // contribution (a mutant's +6 bonus damage, a ghoul's +40 radiation
    // resistance) and the armour's now show, which is what the original draws.
    const int32_t derived[12] = {a.armorClass,
                                 a.meleeDamage,
                                 a.bonusDamage,
                                 a.resist[kDmgNormal],
                                 a.poisonResist,
                                 a.radResist,
                                 a.maxActionPoints,
                                 a.maxCarryWeight,
                                 a.healRate,
                                 a.criticalChance,
                                 a.skillPerLevel,
                                 a.levelsPerPerk};
    for (int32_t i = 0; i < 12; ++i) {
        const int32_t y = r.y0 + kDerivedY0 + i * kDerivedPitch;
        m_style.normal.DrawAligned(c, r.x0 + 170, y, r.x0 + 170 + 0x57, y + 10,
                                   m_loc.Get(std::string("short_") + kDerivedKeys[i]),
                                   m_style.windowText, 0);
        m_style.normal.DrawAligned(c, r.x0 + 170 + 0x57, y, r.x0 + 288, y + 10,
                                   kDerivedPercent[i] ? Fmt("%d%%", derived[i])
                                                      : Fmt("%d", derived[i]),
                                   m_style.buttonText, 1);
    }

    // The skills, `CreateSkillSelectionButtons @004624c0`: header at
    // (SX+0x19, 0x24), then eighteen rows from y = 0x44 with pitch 10, the tag
    // checkbox at (SX+8, y) and the name at (SX+0x28, y)-(SX+0x113, y+10) with
    // its value in a column at +0x102.
    m_style.large.DrawAligned(c, r.x0 + kCharSX + 25, r.y0 + 36, r.x0 + kCharSX + 153, r.y0 + 63,
                              m_loc.Get("name_skills"), m_style.buttonText, 0);
    for (int32_t i = 0; i < 18; ++i) {
        const int32_t y = r.y0 + kSkillY0 + i * kSkillPitch;
        if (a.tagged[i]) c->Blit(*Res()->Image("gui/btn/tag_dn.zar"), r.x0 + kCharSX + 8, y);
        else c->Blit(*Res()->Image("gui/btn/tag_up.zar"), r.x0 + kCharSX + 8, y);
        m_style.normal.DrawAligned(c, r.x0 + kCharSX + 40, y, r.x0 + kCharSX + 0x102, y + 10,
                                   m_loc.Get(std::string("name_") + kSkillKeys[i]),
                                   a.tagged[i] ? m_style.buttonText : m_style.windowText, 0);
        // The value column: `+0x102` is a tab stop inside the row rect, not a
        // separate widget, so the number is right-aligned at the row's own
        // right edge (SX + 0x113).
        m_style.normal.DrawAligned(c, r.x0 + kCharSX + 40, y, r.x0 + kCharSX + 0x113, y + 10,
                                   Fmt("%d%%", a.skills[i]), m_style.buttonText, 1);
    }

    // The name / level strip and the portrait, `RefreshPortraitAndDisplayName
    // @0045d690` and the (0x296, ...) column of CreateNameAgeSexRaceButtons.
    m_style.large.DrawAligned(c, r.x0 + 620, r.y0 + 10, r.x0 + 795, r.y0 + 40, e.ShownName(),
                              m_style.buttonText, 2);
    {
        const std::string key = e.displayName.empty() ? std::string("no_char") : e.displayName;
        const Art* p = Res()->Image("gui/char/" + key + ".zar");
        if (!p->ok) p = Res()->Image("gui/char/no_char.zar");
        if (p->ok) BlitPortraitCrt(c, *p, r.x0 + 662 - p->w / 2, r.y0 + 60, p->w, p->h);
    }
    if (!e.rank.empty()) {
        const Art* rank = Res()->Image("gui/rank/" + e.rank + ".zar");
        if (rank->ok) c->Blit(*rank, r.x0 + 700, r.y0 + 170);
        m_style.normal.DrawAligned(c, r.x0 + 610, r.y0 + 175, r.x0 + 695, r.y0 + 190,
                                   m_loc.Get("name_rank") + ": " + e.rank, m_style.windowText, 1);
    }
    // The three level rows, rects (22, 275/286/297)-(142, ...).
    // Nothing in this port awards experience, so the values are zero and are
    // labelled as such rather than invented.
    static const char* const kLevelKeys[3] = {"name_level", "name_experience", "name_nextLevel"};
    for (int32_t i = 0; i < 3; ++i) {
        const int32_t y = r.y0 + 275 + i * 11;
        m_style.normal.DrawAligned(c, r.x0 + 22, y, r.x0 + 100, y + 10, m_loc.Get(kLevelKeys[i]),
                                   m_style.windowText, 0);
        m_style.normal.DrawAligned(c, r.x0 + 100, y, r.x0 + 142, y + 10, i == 0 ? "1" : "0",
                                   m_style.buttonText, 1);
    }
    // `genericGui_done` at (320, 442).
    m_style.medium.DrawAligned(c, r.x0 + 320, r.y0 + 442, r.x0 + 450, r.y0 + 466,
                               m_loc.Get("genericGui_done"), m_style.bosText, 0);
    c->SetClip(0, 0, m_W, m_H);
}

// ---------------------------------------------------------------------------
// The pipboy.  `WPipboy::WPipboy @004b71c0`.
// ---------------------------------------------------------------------------
void Ui::DrawPipboy(Scene* scene, Canvas* c) {
    const Art* back = Res()->Image("gui/back/Pipboy.zar");
    const Rect r = Modal(800, 472);
    c->SetClip(r.x0, r.y0, r.x1, r.y1);
    if (back->ok) c->Blit(*back, r.x0, r.y0);

    // The five left-column buttons, x = 20, y = 322 / 352 / 382 / 412 / 442.
    static const char* const kPipKeys[5] = {"pipboy_briefings", "pipboy_dialogue", "pipboy_map",
                                            "pipboy_archives", "pipboy_off"};
    for (int32_t i = 0; i < 5; ++i) {
        const int32_t y = r.y0 + 322 + i * 30;
        const Art* b = Res()->Image("gui/btn/med/0.zar");
        if (b->ok) c->Blit(*b, r.x0 + 20, y);
        m_style.medium.DrawAligned(c, r.x0 + 20, y, r.x0 + 20 + (b->ok ? b->w : 152), y + 24,
                                   m_loc.Get(kPipKeys[i]), m_style.bosText, 2);
    }

    // The content `WListBox` / `WTextList`, (280, 70)-(700, 380).
    const int32_t lh = m_style.medium.Height() + 4;
    int32_t y = r.y0 + 70;
    m_style.large.Draw(c, r.x0 + 280, y, m_loc.Get("pipboy_briefings"), m_style.buttonText);
    y += m_style.large.Height() + 8;

    // The clock.  `WPipboy::Draw` draws NO clock and neither
    // `gui/misc/hourdial` nor `gui/misc/months` is referenced anywhere in the
    // decompilation, so the pipboy's own clock face is NOT RECOVERABLE.  The
    // only date code in the dumps is the `printDate` console command, which is
    // `ConvertMsecToDaysHMS(World.gametime)` -> `NormalizeTimeForYearsAndMonths`
    // -> `FormatDateTimeString`; that is what is shown here, as text.
    // MILESTONE 19: this ran the raw millisecond count through its own
    // arithmetic and so read "DAY 1  00:20" while the clock that drives the
    // lighting -- the SAME quantity, `ConvertMsecToDaysHMS @006ad6a0` then
    // `NormalizeTimeForYearsAndMonths @006ada90`, which is what the comment
    // above already says `printDate` does -- read 06:49 on day 1 of 2197.  It
    // now goes through `WorldTime::FromMillis`, the one port of that pair, so
    // the page and the sun agree.
    // MILESTONE 20 (VIDEO_GAPS M15): the capture shows NO clock, date, mission
    // timer or turn counter on any screen -- checked at full resolution on the
    // menu, character selection, briefing, HUD, Skilldex and inventory.  §107
    // made this readout self-consistent with the sun; the honest end state is
    // that the page does not carry one, so it is gone.  `WorldTime::FromMillis`
    // stays -- it is what drives the lighting.
    y += lh;

    // The objectives.  MILESTONE 10: the LIST is now the real one --
    // `ActionAddObjective::Vtable15 @0067fe70` pushes each key onto the world's
    // objective vector and `ActionCompleteObjective::Vtable15 @00680310` moves
    // it to the completed one -- and the text is the mission's own locale
    // entry for that key.  The list's RECTANGLE inside the pipboy art is still
    // a reconstruction: the four `WPipboy` list populators are referenced and
    // not defined anywhere in the dumps.
    m_style.medium.Draw(c, r.x0 + 280, y, m_loc.Get("pipboy_briefings"), m_style.buttonText);
    y += lh;
    DrawObjectiveList(scene, c, r.x0 + 292, y, 400);
    y += lh * (1 + 2 * static_cast<int32_t>(scene->Script().Objectives().size()));
    y += lh;
    m_style.medium.Draw(c, r.x0 + 280, y, m_loc.Get("pipboy_archives"), m_style.buttonText);
    y += lh;
    const int32_t first = std::max<int32_t>(0, static_cast<int32_t>(m_messages.size()) - 6);
    for (int32_t i = first; i < static_cast<int32_t>(m_messages.size()); ++i) {
        m_style.normal.Draw(c, r.x0 + 292, y, m_messages[static_cast<size_t>(i)],
                            m_style.consoleGreen);
        y += m_style.normal.Height() + 2;
    }

    // The action button, `pipboy_back` at (480, 415)-(520, 435).
    m_style.medium.DrawAligned(c, r.x0 + 480, r.y0 + 415, r.x0 + 560, r.y0 + 435,
                               m_loc.Get("pipboy_back"), m_style.bosText, 0);
    c->SetClip(0, 0, m_W, m_H);
}

// ---------------------------------------------------------------------------
// The skilldex -- VIDEO_GAPS L6.
// ---------------------------------------------------------------------------
// `WSkillDex` has no body anywhere in the decompilation, so where the panel
// sits is still a RECONSTRUCTION.  Everything INSIDE it, though, is measured:
// `gui/back/Skilldex.zar` is 200 x 472 and carries the whole face -- a title
// well, eight name recesses down the left, eight three-drum odometer windows
// down the right and an embossed `%` beside each -- and f0416 shows what the
// game puts into them.  The odometer's bright bands sit at art y 95, 133, 171,
// 209, 248, 286, 324 and 362, i.e. a pitch of 38.14 px over eight rows; the
// drum window runs x 113..156 with its two separators at 127 and 141, so the
// three drums are [114,126], [128,140] and [142,155]; and the name recess runs
// x 22..103 on the same rows.  All MEASURED off the art.
//
// f0416 settles the contents: the value is drawn ZERO-PADDED TO THREE DIGITS,
// one white digit per drum, tall enough to fill the drum (`024`, `023`, `050`,
// `010`), on black -- the drum's own grey band is painted over, which is why
// the capture's drums read black where the shipped art's read grey.  The name
// recess carries a raised olive plate the art does not contain: it means
// (97, 78, 37) on f0416 against the recess's own (41, 37, 27), so the button is
// drawn, not blitted, and the plate below is that MEASURED face with a bevel.
// The label is the menu's khaki-cream and the title is gold.
namespace {
constexpr double kSkdxRowPitch = 38.142857;   // MEASURED: bands 95..362 over 8
constexpr int32_t kSkdxRow0 = 89;             // MEASURED: the first drum's top
constexpr int32_t kSkdxWellX0 = 22, kSkdxWellX1 = 104, kSkdxRowH = 24;
constexpr int32_t kSkdxDrumX[3] = {114, 128, 142};
constexpr int32_t kSkdxDrumW = 13, kSkdxDrumH = 23;
inline int32_t SkdxRowY(int32_t i) {
    return kSkdxRow0 + static_cast<int32_t>(i * kSkdxRowPitch + 0.5);
}
}  // namespace

bool Ui::SkilldexButtonRect(int32_t i, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    if (i < 0 || i >= 8) return false;
    const Art* back = Res()->Image("gui/back/Skilldex.zar");
    const int32_t w = back->ok ? back->w : 200, h = back->ok ? back->h : 472;
    int32_t rx0 = m_W - w - 8;
    int32_t ry0 = ViewportHeight(m_H) - h - 8;
    if (ry0 < 0) ry0 = 0;
    *x0 = rx0 + kSkdxWellX0;
    *y0 = ry0 + SkdxRowY(i) - 1;
    *x1 = rx0 + kSkdxWellX1;
    *y1 = *y0 + kSkdxRowH;
    return true;
}

void Ui::DrawSkilldex(Scene* scene, Canvas* c) {
    const Art* back = Res()->Image("gui/back/Skilldex.zar");
    const int32_t w = back->ok ? back->w : 200, h = back->ok ? back->h : 472;
    Rect r;
    // The skilldex is a non-modal overlay (`VectorInsertFill21Byte(..., 1, ...)`
    // where the pipboy and character sheet pass 2), so it sits beside the game
    // bar rather than over the world.  Its actual anchor is unrecoverable.
    r.x0 = m_W - w - 8;
    r.y0 = ViewportHeight(m_H) - h - 8;
    if (r.y0 < 0) r.y0 = 0;
    r.x1 = r.x0 + w;
    r.y1 = r.y0 + h;
    c->SetClip(r.x0, r.y0, r.x1, r.y1);
    if (back->ok) c->Blit(*back, r.x0, r.y0);

    // The title well is art y 30..56; f0416 reads its caption gold.
    m_style.medium.DrawAligned(c, r.x0, r.y0 + 30, r.x1, r.y0 + 56, m_loc.Get("skilldex"),
                               m_style.buttonYellow, 2);
    const int32_t sel = scene->Selected();
    const Attributes* a = sel >= 0 && static_cast<size_t>(sel) < scene->Entities().size()
                              ? &scene->Entities()[static_cast<size_t>(sel)].attr
                              : nullptr;
    for (int32_t i = 0; i < 8; ++i) {
        const int32_t y = r.y0 + SkdxRowY(i);
        // --- the name plate, MEASURED off f0416 --------------------------
        const int32_t px0 = r.x0 + kSkdxWellX0, px1 = r.x0 + kSkdxWellX1;
        const int32_t py0 = y - 1, py1 = y + kSkdxRowH;
        const bool over = m_mouseX >= px0 && m_mouseX < px1 && m_mouseY >= py0 && m_mouseY < py1;
        c->Fill(px0, py0, px1, py1, over ? 0x7E6430u : 0x614E25u);
        c->Fill(px0, py0, px1, py0 + 1, 0x9C7F3Cu);
        c->Fill(px0, py1 - 1, px1, py1, 0x2A2216u);
        m_style.lite.DrawAligned(c, px0 + 4, py0, px1 - 4, py1,
                                 m_loc.Get(std::string("name_") + kSkilldex[i]),
                                 m_style.bosText, 2);
        // --- the odometer ------------------------------------------------
        if (a != nullptr) {
            const int idx = SkillIndexFromName(kSkilldex[i]);
            const int32_t v = idx >= 0 ? a->skills[idx] : 0;
            const int32_t clamped = v < 0 ? 0 : (v > 999 ? 999 : v);
            const char digits[3] = {static_cast<char>('0' + (clamped / 100) % 10),
                                    static_cast<char>('0' + (clamped / 10) % 10),
                                    static_cast<char>('0' + clamped % 10)};
            for (int32_t d = 0; d < 3; ++d) {
                const int32_t dx = r.x0 + kSkdxDrumX[d];
                c->Fill(dx, y, dx + kSkdxDrumW, y + kSkdxDrumH, 0x000000u);
                const std::string one(1, digits[d]);
                m_style.medium.DrawAligned(c, dx, y, dx + kSkdxDrumW, y + kSkdxDrumH, one,
                                           0xF0F0F0u, 2);
            }
        }
        if (over) m_tooltip = m_loc.Get(std::string("desc_") + kSkilldex[i]);
    }
    // CLOSE, bottom-left, with a small dark-red domed lens beside it -- the
    // same lamp `gui/btn/big/Up.zar` puts on a menu plate (NOTES.md 110), at
    // art (28, 441) with the caption from x 44.  MEASURED off f0416.
    const int32_t cy = r.y0 + 441;
    c->Fill(r.x0 + 28, cy + 3, r.x0 + 36, cy + 11, 0x8A1410u);
    c->Fill(r.x0 + 30, cy + 5, r.x0 + 34, cy + 9, 0xD02018u);
    m_style.medium.DrawAligned(c, r.x0 + 42, cy, r.x0 + 150, cy + 16,
                               m_loc.Get("genericGui_close"), m_style.bosText, 0);
    c->SetClip(0, 0, m_W, m_H);
}

// ---------------------------------------------------------------------------
// The in-game menu (MNU).  RECONSTRUCTION -- see kMenuKeys above.
// ---------------------------------------------------------------------------
// MILESTONE 15.  The one definition of where the six in-mission menu entries
// are; `DrawMenu` and `MouseDown` compute the same thing inline and must stay
// in step with it.
// `locale/gui/help.txt` is not a `key = {value}` table -- it is the plain-text
// key and mouse reference, and it is what the Quick Keys page shows.
void Ui::LoadHelpLines() {
    if (!m_helpLines.empty()) return;
    const std::string path = m_res->Resolve("locale/gui/help.txt");
    std::vector<uint8_t> file;
    std::string err;
    if (path.empty() || !ft::ReadWholeFile(path, &file, &err)) {
        m_helpLines.push_back("locale/gui/help.txt not found");
        return;
    }
    const std::string t(file.begin(), file.end());
    size_t at = 0;
    while (at < t.size()) {
        size_t eol = t.find('\n', at);
        if (eol == std::string::npos) eol = t.size();
        std::string l = t.substr(at, eol - at);
        at = eol + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        m_helpLines.push_back(l);
    }
}

bool Ui::MenuButtonRect(int32_t i, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    if (m_screen != UiScreen::Menu || i < 0 || i > 5) return false;
    const Art* back = Res()->Image("gui/back/Menu.zar");
    const Rect r = Modal(back->ok ? back->w : 316, back->ok ? back->h : 416);
    const Art* btn = Res()->Image("gui/btn/med/0.zar");
    const int32_t bw = btn->ok ? btn->w : 152, bh = btn->ok ? btn->h : 24;
    *x0 = r.x0 + (r.x1 - r.x0 - bw) / 2;
    *y0 = r.y0 + 60 + i * (bh + 12);
    *x1 = *x0 + bw;
    *y1 = *y0 + bh;
    return true;
}

void Ui::DrawMenu(Scene* scene, Canvas* c) {
    (void)scene;
    const Art* back = Res()->Image("gui/back/Menu.zar");
    const Rect r = Modal(back->ok ? back->w : 316, back->ok ? back->h : 416);
    c->SetClip(r.x0, r.y0, r.x1, r.y1);
    if (back->ok) c->Blit(*back, r.x0, r.y0);
    // MILESTONE 16: the title plate, which was empty.
    m_style.medium.DrawAligned(c, r.x0 + kMenuPlate[0], r.y0 + kMenuPlate[1],
                               r.x0 + kMenuPlate[2], r.y0 + kMenuPlate[3],
                               m_loc.Get("mainMenu_menu"), m_style.bosText, 2);
    for (int32_t i = 0; i < 6; ++i) {
        int32_t x = 0, y = 0, x1 = 0, y1 = 0;
        if (!MenuButtonRect(i, &x, &y, &x1, &y1)) continue;
        const Art* btn = Res()->Image(i == m_menuHover ? "gui/btn/med/dn.zar" : "gui/btn/med/0.zar");
        if (!btn->ok) btn = Res()->Image("gui/btn/med/0.zar");
        if (btn->ok) c->Blit(*btn, x, y);
        const bool over = m_mouseX >= x && m_mouseX < x1 && m_mouseY >= y && m_mouseY < y1;
        // The label goes in the med button's own child rect, not the button.
        m_style.medium.DrawAligned(c, x + kBosLabelMed.x0, y + kBosLabelMed.y0,
                                   x + kBosLabelMed.x1, y + kBosLabelMed.y1,
                                   m_loc.Get(kMenuKeys[i]),
                                   over ? m_style.selectionFill : m_style.bosText, 2);
    }
    c->SetClip(0, 0, m_W, m_H);
}

// ---------------------------------------------------------------------------
// MILESTONE 16: the two in-mission menu pages that had nothing behind them.
//
// The real Options is THREE pages, and both the classes and the string keys
// for all three ship.  `WOptionsSoundMenu` is `optionsSound_*` -- provider
// plus the four volume buses `master`, `music`, `sfx`, `speech`;
// `WOptionsDisplayMenu` is `optionsDisplay_*` -- device, mode, anti-aliased
// tiles and characters, cursor mode; and `WOptionsGameMenu::WOptionsGameMenu
// @004acfe0` (13 kB, decompiled in full) is `optionsGame_*` -- run mode
// (walk / smart / run), formation mode, game difficulty (easy / normal /
// hard / insane), turn based (on / off / team), fast turn-based, message
// flow 1-4, violence level 1-4 and language filter, all drawn over
// `gui/back/options_game`.
//
// This port has exactly one of those behind it: the four volume buses, which
// are `--audio-volumes` and `bos.cfg`'s own.  Those are drawn and are live;
// the rest are listed as what the real page has and are NOT drawn as dead
// controls.  `gameMenu_keysHelp` is a reader for the shipped
// `locale/gui/help.txt`, which is the game's own key/mouse reference.
void Ui::DrawMenuPage(Scene* scene, Canvas* c) {
    (void)scene;
    const Art* back = Res()->Image("gui/back/Menu.zar");
    const Rect r = Modal(back->ok ? back->w : 316, back->ok ? back->h : 416);
    // The two pages are wider than the menu plate; use a plain panel.
    const int32_t W = 560, H = 440;
    const Rect p = Modal(W, H);
    (void)r;
    c->SetClip(p.x0, p.y0, p.x1, p.y1);
    c->Fill(p.x0, p.y0, p.x1, p.y1, m_style.panelFace, 240);
    c->Fill(p.x0, p.y0, p.x1, p.y0 + 1, m_style.panelLight);
    c->Fill(p.x0, p.y0, p.x0 + 1, p.y1, m_style.panelLight);
    c->Fill(p.x1 - 1, p.y0, p.x1, p.y1, m_style.panelDkShadow);
    c->Fill(p.x0, p.y1 - 1, p.x1, p.y1, m_style.panelDkShadow);
    const bool keys = m_screen == UiScreen::MenuKeys;
    m_style.medium.DrawAligned(c, p.x0, p.y0 + 6, p.x1, p.y0 + 28,
                               m_loc.Get(keys ? "gameMenu_keysHelp" : "optionsSound_title"),
                               m_style.bosText, 2);
    int32_t y = p.y0 + 34;
    if (keys) {
        for (const std::string& l : m_helpLines) {
            if (y > p.y1 - 40) break;
            m_style.lite.Draw(c, p.x0 + 12, y, l, m_style.windowText);
            y += 11;
        }
    } else {
        static const char* const kBus[4] = {"optionsSound_master", "optionsSound_music",
                                            "optionsSound_sfx", "optionsSound_speech"};
        for (int i = 0; i < 4; ++i) {
            m_style.medium.Draw(c, p.x0 + 20, y, m_loc.Get(kBus[i]), m_style.windowText);
            const int32_t v = m_volumes[i];
            c->Fill(p.x0 + 240, y + 4, p.x0 + 440, y + 12, m_style.panelDkShadow);
            c->Fill(p.x0 + 240, y + 4, p.x0 + 240 + 2 * v, y + 12, m_style.buttonText);
            m_style.medium.Draw(c, p.x0 + 460, y, Fmt("%d", v), m_style.buttonText);
            y += 26;
        }
        y += 12;
        for (const char* l :
             {"The four bos.cfg volume buses are the only options behind this port.",
              "The shipped game has two more pages, and their strings all ship:",
              "  optionsDisplay_*  device, mode, anti-aliased tiles/characters, cursor",
              "  optionsGame_*     run mode, formation, difficulty, turn based, fast TB,",
              "                    message flow, violence level, language filter",
              "                    (WOptionsGameMenu @004acfe0, over gui/back/options_game)",
              "Nothing here implements those, so they are not drawn as dead controls."}) {
            m_style.lite.Draw(c, p.x0 + 16, y, l, m_style.windowText);
            y += 12;
        }
    }
    const Art* btn = Res()->Image("gui/btn/med/0.zar");
    const int32_t bw = btn->ok ? btn->w : 152, bh = btn->ok ? btn->h : 24;
    const int32_t bx = p.x0 + (W - bw) / 2, byy = p.y1 - bh - 10;
    if (btn->ok) c->Blit(*btn, bx, byy);
    m_style.medium.DrawAligned(c, bx + kBosLabelMed.x0, byy + kBosLabelMed.y0,
                               bx + kBosLabelMed.x1, byy + kBosLabelMed.y1,
                               m_loc.Get("gameMenu_done"), m_style.bosText, 2);
    m_pageDone[0] = bx;
    m_pageDone[1] = byy;
    m_pageDone[2] = bx + bw;
    m_pageDone[3] = byy + bh;
    c->SetClip(0, 0, m_W, m_H);
}

// ---------------------------------------------------------------------------
// The drag ghost and the tooltip.
// ---------------------------------------------------------------------------
void Ui::DrawDragGhost(Scene* scene, Canvas* c) {
    (void)scene;
    if (!m_drag.active) return;
    // `WIIBase::SetChildWindowVisible @00490690` builds a 140 x 69 copy of the
    // slot and offsets it so the grab point stays under the cursor.
    const int32_t w = 140, h = 69;
    const int32_t x = m_drag.x - w / 2, y = m_drag.y - h / 2;
    c->Fill(x, y, x + w, y + h, m_style.panelFace, 160);
    c->Fill(x, y, x + w, y + 1, m_style.panelLight);
    c->Fill(x, y + h - 1, x + w, y + h, m_style.panelDkShadow);
    m_style.medium.DrawAligned(c, x + 4, y + 20, x + w - 4, y + 44, m_drag.label,
                               m_style.buttonText, 2);
}

// MILESTONE 19.  One popup-help string out of `locale/gui/help_text.txt` with
// the table's own markup taken off: `~` marks the accelerator letter inside the
// text, `<b+>`/`<b->` are bold on/off and `<Cr>`/`<Cw>`/`<C->`/`<C_>` are the
// colour escapes `WBOSText` parses.  Neither is rendered here, so both go.
// Falls back to the key when the table has nothing (`Locale::Get`'s rule).
std::string Ui::HelpText(const std::string& key) const {
    const std::string& raw = m_loc.Get(key);
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '<') {
            const size_t close = raw.find('>', i);
            if (close != std::string::npos && close - i <= 4) { i = close; continue; }
        }
        if (raw[i] == '~' || raw[i] == '}') continue;
        out += raw[i];
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return out;
}

void Ui::DrawTooltip(Canvas* c) {
    if (m_tooltip.empty() || m_mouseX < 0) return;
    // `WGameWorld::Update`'s tooltip placement is
    // the only tooltip geometry in the dumps and it only says the anchor is the
    // mouse; the wrap width, the padding and the offset are this port's.
    constexpr int32_t kWrap = 280;
    std::vector<std::string> lines;
    {
        std::string line, word;
        for (size_t i = 0; i <= m_tooltip.size(); ++i) {
            const char ch = i < m_tooltip.size() ? m_tooltip[i] : ' ';
            if (ch != ' ') { word += ch; continue; }
            const std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && m_style.lite.Measure(cand) > kWrap) {
                lines.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            word.clear();
        }
        if (!line.empty()) lines.push_back(line);
    }
    int32_t tw = 0;
    for (const std::string& l : lines) tw = std::max(tw, m_style.lite.Measure(l));
    tw += 8;
    const int32_t lh = m_style.lite.Height() + 1;
    const int32_t th = static_cast<int32_t>(lines.size()) * lh + 5;
    int32_t x = m_mouseX + 14, y = m_mouseY - th - 4;
    if (x + tw > m_W) x = m_W - tw;
    if (x < 0) x = 0;
    if (y < 0) y = m_mouseY + 18;
    c->Fill(x, y, x + tw, y + th, m_style.windowFill, 220);
    c->Fill(x, y, x + tw, y + 1, m_style.panelLight);
    c->Fill(x, y + th - 1, x + tw, y + th, m_style.panelDkShadow);
    c->Fill(x, y, x + 1, y + th, m_style.panelLight);
    c->Fill(x + tw - 1, y, x + tw, y + th, m_style.panelDkShadow);
    for (size_t i = 0; i < lines.size(); ++i) {
        m_style.lite.Draw(c, x + 4, y + 3 + static_cast<int32_t>(i) * lh, lines[i],
                          m_style.windowText);
    }
}

// ---------------------------------------------------------------------------
// Input.
// ---------------------------------------------------------------------------
bool Ui::MouseMove(int32_t x, int32_t y) {
    m_mouseX = x;
    m_mouseY = y;
    m_hoverTab = -1;
    const int32_t bx = BarX(), by = BarY();
    if (y >= by && y < by + kBarH) {
        for (int32_t k = 0; k < 6; ++k) {
            const int32_t tx = bx + kTabX + k * kTabPitch;
            if (x >= tx && x < tx + kTabW && y >= by + kTabY && y < by + kTabY + kTabH) {
                m_hoverTab = k;
            }
        }
    }
    if (m_drag.pressed) {
        m_drag.x = x;
        m_drag.y = y;
        // `WIIBase::CheckDragThresholdExceeded @00490600`: `100 < dx*dx + dy*dy`.
        const int32_t dx = x - m_drag.pressX, dy = y - m_drag.pressY;
        if (dx * dx + dy * dy > 100) m_drag.active = true;
    }
    return y >= by || m_screen != UiScreen::None;
}

// Which slot is under (x, y)?  Returns -2 for "none".
int32_t Ui::HitSlot(Scene* scene, int32_t x, int32_t y, int32_t* entityOut) const {
    const int32_t sel = scene->Selected();
    *entityOut = sel;
    if (sel < 0) return -2;
    const int32_t bx = BarX(), by = BarY();
    if (y >= by) {
        if (x >= bx + kHand0X && x < bx + kHand0X + kHandW && y >= by + kHandY &&
            y < by + kHandY + kHandH) {
            return 0;
        }
        if (x >= bx + kHand1X && x < bx + kHand1X + kHandW && y >= by + kHandY &&
            y < by + kHandY + kHandH) {
            return 1;
        }
        return -2;
    }
    if (m_screen != UiScreen::Inventory) return -2;
    for (size_t i = 0; i < m_slotRects.size(); ++i) {
        const Rect& r = m_slotRects[i];
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return static_cast<int32_t>(i);
    }
    for (size_t i = 0; i < m_invRows.size(); ++i) {
        const Rect& r = m_invRows[i];
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
            return 3 + static_cast<int32_t>(i);
        }
    }
    return -2;
}

int32_t Ui::HitLootRow(int32_t x, int32_t y, int32_t* side) const {
    for (size_t i = 0; i < m_lootRowsL.size(); ++i) {
        const Rect& r = m_lootRowsL[i];
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
            *side = 0;
            return static_cast<int32_t>(i);
        }
    }
    for (size_t i = 0; i < m_lootRowsR.size(); ++i) {
        const Rect& r = m_lootRowsR[i];
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
            *side = 1;
            return static_cast<int32_t>(i);
        }
    }
    *side = -1;
    return -1;
}

bool Ui::MouseDown(Scene* scene, int32_t x, int32_t y, bool right) {
    m_mouseX = x;
    m_mouseY = y;
    const int32_t bx = BarX(), by = BarY();
    const bool inBar = y >= by;

    // VIDEO_GAPS M7: the minimap's two zoom marks.  `gui/misc/pmap/small`,
    // `medium` and `large.zar` say there are three steps and the capture puts a
    // `+` in the widget's top-right corner and a `-` in its bottom-right; a
    // click within 10 px of either steps the zoom.
    if (!right && !m_showText && m_miniRect[2] > m_miniRect[0] && x >= m_miniRect[2] - 18 &&
        x < m_miniRect[2] && y >= m_miniRect[1] && y < m_miniRect[3]) {
        if (y < m_miniRect[1] + 18) {
            if (m_miniZoom < 2) ++m_miniZoom;
            return true;
        }
        if (y >= m_miniRect[3] - 14) {
            if (m_miniZoom > 0) --m_miniZoom;
            return true;
        }
    }

    // VIDEO_GAPS L7: DONE on the tutorial popup, which takes the click before
    // anything under it does -- the panel is modal on the capture.
    if (!scene->Script().WorldText().empty() && !right && m_worldTextDone[2] > m_worldTextDone[0]) {
        if (x >= m_worldTextDone[0] && x < m_worldTextDone[2] && y >= m_worldTextDone[1] &&
            y < m_worldTextDone[3]) {
            scene->Script().DismissWorldText();
            return true;
        }
    }

    // MILESTONE 18: the skilldex's eight entries, which had no click handler
    // at all -- choosing a skill from the panel did nothing.  Each one goes
    // through the same `Action` verb keys.cfg's `1`..`8` do.
    if (m_screen == UiScreen::Skilldex && !right) {
        for (int32_t i = 0; i < 8; ++i) {
            int32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
            if (!SkilldexButtonRect(i, &sx0, &sy0, &sx1, &sy1)) continue;
            if (x >= sx0 && x < sx1 && y >= sy0 && y < sy1) {
                Action(scene, kSkilldex[i]);
                return true;
            }
        }
        // CLOSE (VIDEO_GAPS L6): the panel's own dismiss, which the port had
        // no control for at all.
        const Art* sback = Res()->Image("gui/back/Skilldex.zar");
        const int32_t sw = sback->ok ? sback->w : 200, sh = sback->ok ? sback->h : 472;
        int32_t rx0 = m_W - sw - 8;
        int32_t ry0 = ViewportHeight(m_H) - sh - 8;
        if (ry0 < 0) ry0 = 0;
        if (x >= rx0 + 24 && x < rx0 + 150 && y >= ry0 + 438 && y < ry0 + 460) {
            m_screen = UiScreen::None;
            return true;
        }
    }

    if (inBar && !right) {
        // the five window buttons
        for (int32_t i = 0; i < 5; ++i) {
            const int32_t bxx = bx + kBtnX, byy = by + kBtnY + i * kBtnPitch;
            if (x >= bxx && x < bxx + kBtnW && y >= byy && y < byy + kBtnH) {
                Action(scene, kBarButtons[i].command);
                return true;
            }
        }
        // MILESTONE 19: the sentry column, three plates, each a direct set of
        // its own mode through the same verb keys.cfg's comma / period / slash
        // use -- so it goes through `Action`, which applies it to the WHOLE
        // selection and skips the dead, exactly as
        // `LogLegalTargetFieldChange0e @0049eb60` does.
        //
        // THE PLAYER'S BUG: before this the hit test was ONE 30 x 24 rect at
        // the TOP of a 30 x 72 button, so two thirds of the visible control was
        // dead, and what it did hit cycled the mode of the focused member only.
        for (int32_t i = 0; i < 3; ++i) {
            const int32_t sx = bx + kSentryX, sy = by + kColY[i];
            if (x >= sx && x < sx + kSentryW && y >= sy && y < sy + kSentryH) {
                Action(scene, kSentryButtons[i].verb);
                return true;
            }
        }
        // the stance column
        for (int32_t i = 0; i < 3; ++i) {
            const int32_t sx = bx + kStanceX, sy = by + kColY[i];
            if (x >= sx && x < sx + kStanceW && y >= sy && y < sy + kStanceH) {
                Action(scene, kStanceButtons[i].verb);
                return true;
            }
        }
        // the map / text toggles
        if (x >= bx + kMMX && x < bx + kMMX + 28) {
            if (y >= by + kMMMapY && y < by + kMMMapY + 30) {
                m_showText = false;
                return true;
            }
            if (y >= by + kMMTextY && y < by + kMMTextY + 30) {
                m_showText = true;
                return true;
            }
        }
        // A squad tab selects that member: `WSquadMember::HandleClickAtPoint
        // @004d16b0` plays the panel's own sound and hands the entity to
        // `HandleEntityClickForSkillOrSelect @004d13a0`, whose default arm is
        // `WGameWorld::ToggleEntitySelection @0049fb40` -- an exclusive select
        // for a plain click, an add/remove for the modified one (keys.cfg's
        // `<shift>F1..F6` = `modSelect1..6`).
        for (int32_t k = 0; k < 6; ++k) {
            const int32_t tx = bx + kTabX + k * kTabPitch;
            if (x >= tx && x < tx + kTabW && y >= by + kTabY && y < by + kTabY + kTabH) {
                const std::vector<int32_t> squad = SquadList(scene);
                if (k < static_cast<int32_t>(squad.size())) {
                    if (m_shift) scene->SelectToggle(squad[static_cast<size_t>(k)]);
                    else scene->Select(squad[static_cast<size_t>(k)]);
                    // MILESTONE 16: the second click of a double-click on a
                    // tab centres the view on that member.  RECONSTRUCTION:
                    // `WSquadControlPanel` is a call site only and no dump
                    // carries a double-click path; the player asked for it and
                    // this is where the original would have to put it.
                    if (m_lastTab == k && m_nowMs - m_tabClickMs < 400) {
                        m_centreRequest = squad[static_cast<size_t>(k)];
                    }
                    m_lastTab = k;
                    m_tabClickMs = m_nowMs;
                }
                return true;
            }
        }
    }

    // MILESTONE 17: the loot window.  It is modal over the world, like the
    // other full-screen windows.
    if (m_screen == UiScreen::Loot && !right && !inBar) {
        if (x >= m_lootDone.x0 && x < m_lootDone.x1 && y >= m_lootDone.y0 &&
            y < m_lootDone.y1) {
            scene->CloseLoot();
            m_screen = UiScreen::None;
            return true;
        }
        if (x >= m_lootTakeAll.x0 && x < m_lootTakeAll.x1 && y >= m_lootTakeAll.y0 &&
            y < m_lootTakeAll.y1) {
            std::string why;
            const int32_t n = scene->TakeAll(scene->LootTarget(), scene->LootActor(), &why);
            PushMessage(Fmt("%s: %d items", m_loc.Get("equipMenu_takeAll").c_str(), n));
            if (!why.empty() && n == 0) PushMessage(why);
            return true;
        }
        int32_t side = -1;
        const int32_t row = HitLootRow(x, y, &side);
        if (row >= 0) {
            // `WIIBase::HandleMouseEvent @00490870` stores the press point; the
            // drag only begins once `CheckDragThresholdExceeded @00490600`'s
            // `100 < dx*dx + dy*dy` is met.  A press that never exceeds it is a
            // click, which transfers the whole stack -- the original would put
            // `WLoot::PromptItemQuantityToLoot @004a57d0` up for a stack of more
            // than one, and that prompt window is not drawn here.
            m_drag.pressed = true;
            m_drag.active = false;
            m_drag.pressX = x;
            m_drag.pressY = y;
            m_drag.x = x;
            m_drag.y = y;
            m_drag.fromEntity = side == 0 ? scene->LootTarget() : scene->LootActor();
            m_drag.fromSlot = -1;
            m_drag.lootSide = side;
            m_drag.lootRow = row;
            const std::vector<LiveEntity>& es = scene->Entities();
            const size_t owner = static_cast<size_t>(m_drag.fromEntity);
            if (owner < es.size() && static_cast<size_t>(row) < es[owner].inventory.size()) {
                m_drag.label = es[owner].inventory[static_cast<size_t>(row)].displayName;
            }
            return true;
        }
        return true;   // the window is modal
    }

    if (!right) {
        int32_t ent = -1;
        const int32_t slot = HitSlot(scene, x, y, &ent);
        if (slot != -2 && ent >= 0) {
            m_drag.pressed = true;
            m_drag.active = false;
            m_drag.pressX = x;
            m_drag.pressY = y;
            m_drag.x = x;
            m_drag.y = y;
            m_drag.fromEntity = ent;
            m_drag.fromSlot = slot;
            const LiveEntity& e = scene->Entities()[static_cast<size_t>(ent)];
            if (slot == 0) m_drag.label = e.weapon.displayName;
            else if (slot == 1) m_drag.label = e.weapon2.displayName;
            else if (slot == 2) m_drag.label = e.armour.displayName;
            else if (static_cast<size_t>(slot - 3) < e.inventory.size())
                m_drag.label = e.inventory[static_cast<size_t>(slot - 3)].displayName;
            return true;
        }
    }
    if ((m_screen == UiScreen::MenuKeys || m_screen == UiScreen::MenuOptions) && !right) {
        if (x >= m_pageDone[0] && x < m_pageDone[2] && y >= m_pageDone[1] && y < m_pageDone[3]) {
            m_screen = UiScreen::Menu;
        }
        return true;   // the page is modal
    }
    if (m_screen == UiScreen::Menu && !right) {
        for (int32_t i = 0; i < 6; ++i) {
            int32_t bxx = 0, byy = 0, bx1 = 0, by1 = 0;
            if (!MenuButtonRect(i, &bxx, &byy, &bx1, &by1)) continue;
            if (x >= bxx && x < bx1 && y >= byy && y < by1) {
                m_menuHover = i;
                // MILESTONE 16: all six entries now do something.  Two of them
                // are wholly local -- Quick Keys reads the shipped
                // `locale/gui/help.txt` and Options shows the four bos.cfg
                // volume buses -- and the other four are the campaign layer's,
                // so they still go to main.cpp through `TakeMenuChoice`.
                if (i == 0) {
                    LoadHelpLines();
                    m_screen = UiScreen::MenuKeys;
                } else if (i == 3) {
                    m_screen = UiScreen::MenuOptions;
                } else if (i == 5) {
                    m_screen = UiScreen::None;      // gameMenu_done
                }
                m_menuChoice = i;
                return true;
            }
        }
        return true;   // the menu panel is modal: a click outside a button is eaten
    }
    return inBar || m_screen != UiScreen::None;
}

bool Ui::MouseUp(Scene* scene, int32_t x, int32_t y, bool right) {
    m_mouseX = x;
    m_mouseY = y;
    if (right || !m_drag.pressed) {
        m_drag.pressed = false;
        m_drag.active = false;
        return y >= BarY() || m_screen != UiScreen::None;
    }
    const bool wasDragging = m_drag.active;
    m_drag.pressed = false;
    m_drag.active = false;
    // MILESTONE 17: a loot transfer.  Either half of the window sends to the
    // other; a plain click sends to the other half too, which is what makes
    // "click an item to take it" work without the quantity prompt.
    if (m_drag.lootSide >= 0) {
        const int32_t fromSide = m_drag.lootSide;
        const int32_t row = m_drag.lootRow;
        m_drag.lootSide = -1;
        m_drag.lootRow = -1;
        int32_t dropSide = -1;
        if (wasDragging) {
            int32_t dummy = -1;
            HitLootRow(x, y, &dropSide);
            (void)dummy;
            if (dropSide < 0) {
                // A drop anywhere in the other half's column still counts.
                const Rect ref = m_lootTakeAll;
                dropSide = x >= ref.x0 + 400 ? 1 : 0;
            }
            if (dropSide == fromSide) return true;   // dropped back on itself
        }
        const int32_t from = fromSide == 0 ? scene->LootTarget() : scene->LootActor();
        const int32_t to = fromSide == 0 ? scene->LootActor() : scene->LootTarget();
        std::string why;
        if (!scene->TransferItem(from, to, row, 0, &why)) PushMessage(why);
        return true;
    }
    if (!wasDragging) {
        // a click, not a drag: `WIIHand::HandleEquippedItemActivation @0047ff80`
        // selects the hand when it is not the active one.
        if (m_drag.fromSlot == 0 || m_drag.fromSlot == 1) {
            const LiveEntity& e = scene->Entities()[static_cast<size_t>(m_drag.fromEntity)];
            if (e.handSelected != m_drag.fromSlot) scene->SwapHands(m_drag.fromEntity);
        }
        return true;
    }
    int32_t ent = -1;
    const int32_t slot = HitSlot(scene, x, y, &ent);
    if (slot == -2 || ent != m_drag.fromEntity) return true;
    std::string why;
    if (!scene->MoveItem(m_drag.fromEntity, m_drag.fromSlot, slot, &why)) {
        PushMessage(why);
    } else {
        PushMessage(m_drag.label + " -> " +
                    (slot == 0 ? "left hand"
                               : slot == 1 ? "right hand" : slot == 2 ? "armour" : "pack"));
    }
    return true;
}

void Ui::SyntheticDrag(Scene* scene, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    MouseMove(x0, y0);
    MouseDown(scene, x0, y0, false);
    MouseMove(x1, y1);
    m_drag.active = true;   // the threshold, forced for the scripted run
    MouseUp(scene, x1, y1, false);
    MouseMove(x1, y1);
}

bool Ui::Action(Scene* scene, const std::string& action) {
    // MILESTONE 13: the interface click.  `WWindow`'s button classes each play
    // their own `sound/game/Butn_*.wav` -- `HandleFocusGainedPlaySound
    // @00662ff0` is the text field's, and the string
    // table carries Butn_Arrow, Butn_BigRed, Butn_Character, Butn_CheckBox,
    // Butn_InGame, Butn_InGameMap, Butn_ItemSwitch, Butn_Mission,
    // Butn_Number, Butn_PullDown, Butn_ReadyOff, Butn_ReadyOn, Butn_Skill,
    // Butn_SmallRed, Butn_Tab, Butn_Text, Butn_TextLog and Butn_Traits.
    // WHICH control owns which file is only recoverable for the four the
    // dumps name (Arrow, Mission, Skill, and the text field's Butn_Text), so
    // the rest of this table is a RECONSTRUCTION from the file names.
    if (scene != nullptr && scene->GetAudio() != nullptr) {
        const char* wav = "sound/game/Butn_InGame.wav";
        if (action == "character") wav = "sound/game/Butn_Character.wav";
        else if (action == "skilldex") wav = "sound/game/Butn_Skill.wav";
        else if (action == "pipboy" || action == "pipboyMap")
            wav = "sound/game/Butn_InGameMap.wav";
        else if (action == "inventory") wav = "sound/game/Butn_ItemSwitch.wav";
        else if (action == "menu") wav = "sound/game/Butn_BigRed.wav";
        else if (action.compare(0, 6, "sentry") == 0)
            wav = "sound/game/Butn_ReadyOn.wav";
        else if (action == "endTurn") wav = "sound/game/endTurn.wav";
        scene->GetAudio()->PlayUi(wav, action.c_str());
    }
    auto toggle = [&](UiScreen s) {
        m_screen = m_screen == s ? UiScreen::None : s;
        m_drag.active = false;
        m_drag.pressed = false;
    };
    if (action == "inventory") { toggle(UiScreen::Inventory); return true; }
    if (action == "character") { toggle(UiScreen::Character); return true; }
    if (action == "pipboy" || action == "pipboyMap") { toggle(UiScreen::Pipboy); return true; }
    if (action == "skilldex") { toggle(UiScreen::Skilldex); return true; }
    if (action == "menu") { toggle(UiScreen::Menu); return true; }
    const int32_t sel = scene->Selected();
    // -----------------------------------------------------------------------
    // MILESTONE 18: EVERY ORDER APPLIES TO THE WHOLE SELECTION.
    //
    // The three stance verbs are `WGameMain::DispatchInputActionByName
    // @004734f0`'s thunks at @0049eec0 / @0049eeb0 / @0049eea0, each of
    // which is an eight-byte thunk into
    //
    //   `UpdateFlaggedPlayerEntities @0049ee00` (n)
    //     for each entity in the local player's list
    //         if (entity.unkflags & 0x40)              <- SELECTED
    //             FUN_006311e0(entityId, n)            <- post order n
    //
    // with n = 10 stand, 9 crouch, 8 prone (and 1 for the fourth verb at
    // 0089a100).  The sentry column is the identical loop in
    // `WGameWorld::LogLegalTargetFieldChange0e @0049eb60` (order 0x0e) and
    // overwatch in `...0f @0049ed00` (order 0x0f), both of which additionally
    // require `IsLegalTarget`.  The weapon verbs are
    // `WGameMain::ToggleActiveWeaponHandForPlayers @00477cd0`,
    // `UpdateWeaponFireModeForPlayers @00477dd0` and
    // `UpdateCalledShotAvailabilityForPlayers @00477f40` -- the names say it.
    //
    // So NOTHING on this list is per-focused-member; the port applied all of
    // them to `Selected()` alone, which is what the player reported.
    const std::vector<int32_t>& many = scene->Selection();
    auto forEachSelected = [&](const std::function<void(int32_t)>& fn) {
        if (many.empty()) {
            if (sel >= 0) fn(sel);
            return;
        }
        for (int32_t i : many) fn(i);
    };
    if (action == "stand" || action == "crouch" || action == "prone") {
        std::string p = action;
        p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        forEachSelected([&](int32_t i) { scene->SetPosture(i, p); });
        return true;
    }
    // MILESTONE 11: the sentry column.  keys.cfg binds comma / period / slash
    // to sentryOff / sentryScout / sentryOn, which are `SentryAI+0x1ad`'s
    // three modes.  MILESTONE 18: over the selection, and only over its LEGAL
    // TARGETS -- `LogLegalTargetFieldChange0e`'s own extra test, which is what
    // keeps a dead member out of it.
    if (action == "sentryOff" || action == "sentryScout" || action == "sentryOn") {
        const SentryMode m = action == "sentryOff"    ? kSentryOff
                             : action == "sentryScout" ? kSentryScout
                                                       : kSentryOn;
        forEachSelected([&](int32_t i) {
            if (i < 0 || static_cast<size_t>(i) >= scene->Entities().size()) return;
            if (scene->Entities()[static_cast<size_t>(i)].dead) return;
            scene->SetSentry(i, m);
        });
        return true;
    }
    // -----------------------------------------------------------------------
    // MILESTONE 18: the eight skilldex skills.  keys.cfg binds `1`..`8` to
    // them in exactly this order, and so does the pointer table at 0x008a907c
    // that `WGameWorld::ProcessCombatTargetingAndTriggers` indexes when it
    // auto-arms Traps over a trap.  All eight dispatch to the SAME function,
    // `WGameWorld::EvaluateSneakSkillCondition @0049de10` -- misnamed; it is
    // "begin a skill use" -- which
    //   * clears the use-item mode (`field_0x1d5 = 0`),
    //   * for a name that is NOT "sneak", sets `field_0x1d3 = 1` and stores
    //     the skill at `+0x1cf`, then refreshes the cursor: a SKILL CURSOR
    //     that waits for a target click;
    //   * for "sneak", takes no target at all -- it walks the selection,
    //     counts hidden versus visible, and toggles every selected member
    //     (all of them ON when the selection is mixed).
    if (action == "sneak" || action == "lockpick" || action == "steal" || action == "traps" ||
        action == "firstAid" || action == "doctor" || action == "science" ||
        action == "repair") {
        if (action == "sneak") {
            // The mixed-selection rule, verbatim.
            int hidden = 0, visible = 0;
            forEachSelected([&](int32_t i) {
                if (i < 0 || static_cast<size_t>(i) >= scene->Entities().size()) return;
                if (scene->Entities()[static_cast<size_t>(i)].sneaking) ++hidden;
                else ++visible;
            });
            const bool uniform = !(hidden != 0 && visible != 0);
            forEachSelected([&](int32_t i) {
                if (i < 0 || static_cast<size_t>(i) >= scene->Entities().size()) return;
                if (!uniform && scene->Entities()[static_cast<size_t>(i)].sneaking) return;
                std::string why;
                scene->UseSkill(i, "sneak", -1, &why);
                if (!why.empty()) PushMessage(why);
            });
            m_skillCursor.clear();
            return true;
        }
        m_skillCursor = action;
        m_screen = UiScreen::None;      // the skilldex closes behind the cursor
        return true;
    }
    if (action == "reload") {
        forEachSelected([&](int32_t i) { scene->Reload(i); });
        return true;
    }
    if (action == "swapHands") {
        // `WGameMain::ToggleActiveWeaponHandForPlayers @00477cd0`.
        forEachSelected([&](int32_t i) { scene->SwapHands(i); });
        return true;
    }
    if (action == "stop") {
        forEachSelected([&](int32_t i) { scene->StopMove(i); });
        return true;
    }
    if (action == "characterEndTurn/stop" || action == "squadEndTurn") {
        if (scene->TurnBased()) scene->EndTurn();
        else forEachSelected([&](int32_t i) { scene->StopMove(i); });
        return true;
    }
    if (action == "cycleWeaponModes") {
        // `WGameMain::UpdateWeaponFireModeForPlayers @00477dd0`.
        forEachSelected([&](int32_t i) {
            if (i < 0 || static_cast<size_t>(i) >= scene->Entities().size()) return;
            const LiveEntity& e = scene->Entities()[static_cast<size_t>(i)];
            const WeaponDef& w = scene->ActiveWeapon(e);
            if (w.valid && w.modes.size() > 1) {
                const size_t next = (static_cast<size_t>(w.selectedMode) + 1) % w.modes.size();
                scene->SetWeaponMode(i, w.modes[next].modeName);
            }
        });
        return true;
    }
    // MILESTONE 16: the whole `SQUAD CONTROLS` block of `locale/gui/help.txt`.
    // keys.cfg binds F1-F6 to `select1..6`, <shift>F1-F6 to `modSelect1..6`,
    // F12 to `selectAll`, F7-F11 to `recallGroup1..5` and <ctrl>F7-F11 to
    // `setGroup1..5`.  The two group verbs are
    // `WGameWorld::SaveSelectionToControlGroup @0049f7c0` and
    // `RecallControlGroupSelection @0049f810`, both of which take an index
    // 0..5 and refuse anything outside it; the storage itself is the player
    // object's own slot and is not in the dumps, so it lives here.
    if (action.rfind("select", 0) == 0 && action.size() == 7) {
        const int k = action[6] - '1';
        const std::vector<int32_t> squad = SquadList(scene);
        if (k >= 0 && k < static_cast<int>(squad.size())) scene->Select(squad[static_cast<size_t>(k)]);
        return true;
    }
    if (action.rfind("modSelect", 0) == 0 && action.size() == 10) {
        const int k = action[9] - '1';
        const std::vector<int32_t> squad = SquadList(scene);
        if (k >= 0 && k < static_cast<int>(squad.size())) {
            scene->SelectToggle(squad[static_cast<size_t>(k)]);
        }
        return true;
    }
    if (action == "selectAll") {
        scene->SelectMany(SquadList(scene));
        return true;
    }
    if (action.rfind("setGroup", 0) == 0 && action.size() == 9) {
        const int k = action[8] - '1';
        if (k >= 0 && k < 5) m_groups[k] = scene->Selection();
        return true;
    }
    if (action.rfind("recallGroup", 0) == 0 && action.size() == 12) {
        const int k = action[11] - '1';
        if (k >= 0 && k < 5 && !m_groups[k].empty()) scene->SelectMany(m_groups[k]);
        return true;
    }
    if (action == "cycleMembers") {
        const std::vector<int32_t> squad = SquadList(scene);
        if (!squad.empty()) {
            size_t at = 0;
            for (size_t i = 0; i < squad.size(); ++i) {
                if (squad[i] == sel) { at = i + 1; break; }
            }
            scene->Select(squad[at % squad.size()]);
            // "Space: Next character" only makes sense if you can see him.
            // RECONSTRUCTION -- no dump carries the recentre.
            m_centreRequest = squad[at % squad.size()];
        }
        return true;
    }
    return false;
}

std::string Ui::TraceLayout() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "ui: screen=%d bar=(%d,%d,800x128) viewport=%dx%d mouse=(%d,%d) drag=%d "
                  "cursor=%s skill='%s' tooltip='%s'\n",
                  static_cast<int>(m_screen), BarX(), BarY(), m_W, ViewportHeight(m_H), m_mouseX,
                  m_mouseY, m_drag.active ? 1 : 0, m_cursorName.c_str(), m_skillCursor.c_str(),
                  m_tooltip.c_str());
    return std::string(buf) + m_trace;
}



// ===========================================================================
// MILESTONE 10 -- what the mission script puts on screen.
//
// Four surfaces, and every one of them is a reconstruction of geometry only:
// the CONTENT is the binary's (which key, which text, when it appears) and the
// RECTANGLES are not, because none of the four window classes has a body in the
// decompilation.  What IS ported is where each string comes from:
//
//   * the speech bubble text is a `<speech_node>`'s line key, resolved in the
//     mission's own locale table;
//   * the world-text banner is `ActionDisplayWorldText::Vtable15 @00683920`,
//     which looks up `"title_" + key` for the heading and `key` for the body,
//     and picks the success/failure styling by searching the key for "success"
//     and "failure";
//   * the objective list is `ActionAddObjective::Vtable15 @0067fe70` pushing
//     the key onto World+0x40 and `ActionCompleteObjective::Vtable15
//     @00680310` moving it to the completed list at +0x54, which also emits
//     the localized `objective_complete` line;
//   * the debrief is `ActionDisplayDebrief::Vtable15 @00687f50`, which sorts
//     the (position, key) pairs `ActionAddDebriefText::Vtable15 @00687c30`
//     collected, takes `"title_" + firstKey` as the heading and concatenates
//     the rest.
// ===========================================================================

std::vector<std::string> Ui::Wrap(const Font& f, const std::string& s, int32_t width) const {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&]() {
        if (!line.empty()) out.push_back(line);
        line.clear();
    };
    for (size_t i = 0; i <= s.size(); ++i) {
        // The shipped strings carry literal "\n" escapes for paragraph breaks.
        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'n') {
            if (!word.empty()) {
                if (!line.empty()) line += ' ';
                line += word;
                word.clear();
            }
            flush();
            out.push_back(std::string());
            ++i;
            continue;
        }
        const char c = i < s.size() ? s[i] : ' ';
        if (c == ' ' || c == '\t' || i == s.size()) {
            if (word.empty()) continue;
            std::string cand = line.empty() ? word : line + " " + word;
            if (f.Measure(cand) > width && !line.empty()) {
                flush();
                line = word;
            } else {
                line = cand;
            }
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    flush();
    return out;
}

void Ui::LoadMissionStrings(const std::string& misPath) {
    if (!m_ready) return;
    size_t a = misPath.find_last_of("/\\");
    std::string base = a == std::string::npos ? misPath : misPath.substr(a + 1);
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    size_t n = 0;
    for (const std::string& f : Res()->List("locale/missions/" + base + "/", ".txt")) {
        m_loc.LoadMissionFile(f);
        ++n;
    }
    // VIDEO_GAPS L7.  The four tutorial maps do NOT follow the rule every core
    // mission follows -- `mission01.mis` reads `locale/missions/mission01/`, but
    // `tut1.mis` names `locale/missions/tutorial1/tutorial1.txt` and
    // `tut2a|b|c.mis` name `locale/missions/tutorial2/tut2a|b|c.txt`, so the
    // stem and the directory differ and every tutorial popup rendered its raw
    // label (`TUTO1_TUTOR_1_B`) instead of its text.  Nothing in any dump names
    // the mapping, so this is a LABELLED reconstruction: a tutorial map loads
    // both `tutorial1/` and `tutorial2/` whole.  The keys are prefixed
    // (`TUTO1_`, `TUT2A_`) so the two cannot collide.
    if (n == 0 && base.compare(0, 3, "tut") == 0) {
        for (const char* dir : {"locale/missions/tutorial1/", "locale/missions/tutorial2/"}) {
            for (const std::string& f : Res()->List(dir, ".txt")) m_loc.LoadMissionFile(f);
        }
    }
}

// Speech, and floating status text -- VIDEO_GAPS M3 and M4.
//
// `WSpeechBubble` is a call site only, so milestone 10 invented a green-bordered
// box with a tail.  The capture says there is NO box: f0348 (two lines), f0350
// (one line) and f0610 all show bare text, centre-aligned, wrapped, in white
// with a hard one-pixel black outline, floating over the world with nothing
// behind it.  That is exactly what the shipped outlined font is for --
// `font_consoleOutline` = `gui/font/fnt_monitor_outline.png`, which
// `def_style.cfg` names and which stores a grey ramp under a binary alpha, so a
// white tint leaves the glyph white and its outline black (see Font::Draw).
// The only geometry in any dump is `WGameWorld::Update`'s anchor
// (`WGameWorld::Update@004a1240`); the LIFT is MEASURED: on f0348 the speaker's feet are
// at y 434 and the last line's baseline at y 380, i.e. the block's bottom sits
// 54 px above the actor's ground point, which is inside the row's 40-70 px.
//
// M4: the same treatment carries floating STATUS text -- "Detected" over a
// squad member's head on f0246.  It is the same draw, so both lists go through
// this one function.  The capture has NO floating damage numbers anywhere, and
// none are drawn.
namespace {
constexpr int32_t kFloatLift = 54;     // MEASURED off f0348 (px above the feet)
constexpr int32_t kFloatWrap = 320;    // MEASURED: f0348's longest line is 285 px
}  // namespace

void Ui::DrawFloatingText(Canvas* c, const LiveEntity& e, const std::string& text,
                          uint32_t rgb, const char* traceTag) {
    const Font& f = m_style.consoleOutline.Ok() ? m_style.consoleOutline : m_style.normal;
    const std::vector<std::string> lines = Wrap(f, text, kFloatWrap);
    if (lines.empty()) return;
    const int32_t lh = f.Height() + 1;
    // The same ground anchor the selection ring uses: the bottom-centre of the
    // entity's own drawn rect, not the projection of its world cell.
    const int32_t cx = static_cast<int32_t>((e.rect[0] + e.rect[2]) / 2 - m_camX);
    const int32_t foot = static_cast<int32_t>(e.rect[3] - m_camY);
    int32_t y = foot - kFloatLift - static_cast<int32_t>(lines.size()) * lh;
    y = std::max<int32_t>(2, y);
    for (const std::string& l : lines) {
        const int32_t tw = f.Measure(l);
        int32_t x = cx - tw / 2;
        x = std::max<int32_t>(2, std::min<int32_t>(x, m_W - tw - 2));
        f.Draw(c, x, y, l, rgb);
        y += lh;
    }
    m_trace += std::string(traceTag) + " \"" + text + "\" at (" + std::to_string(cx) + "," +
               std::to_string(foot - kFloatLift) + ")\n";
}

void Ui::DrawSpeech(Scene* scene, Canvas* c) {
    const MissionScript& sc = scene->Script();
    const std::vector<LiveEntity>& ents = scene->Entities();
    if (sc.Bubbles().empty() && scene->StatusFloats().empty()) return;
    const int32_t vh = ViewportHeight(m_H);
    c->SetClip(0, 0, m_W, vh);
    for (const FloatingText& f : sc.Bubbles()) {
        if (f.entity < 0 || f.entity >= static_cast<int32_t>(ents.size())) continue;
        DrawFloatingText(c, ents[static_cast<size_t>(f.entity)], m_loc.Get(f.text), 0xFFFFFF,
                         "speech");
    }
    // M4.  The status word is the shipped string when the locale has one and
    // the bare word otherwise; the capture's is "Detected".
    for (const Scene::StatusFloat& s : scene->StatusFloats()) {
        if (s.entity < 0 || s.entity >= static_cast<int32_t>(ents.size())) continue;
        DrawFloatingText(c, ents[static_cast<size_t>(s.entity)], m_loc.Get(s.text), 0xFFFFFF,
                         "status");
    }
    c->SetClip(0, 0, m_W, m_H);
}

// ActionDisplayWorldText: a banner across the top of the viewport.
// `ActionDisplayWorldText` -- and, VIDEO_GAPS L7, THE TUTORIAL POPUP.
//
// The brief asked what a tutorial popup is.  It is this action and nothing
// else: `--triggers` over the shipped `campaigns/missions/tutorials/tut1.mis`
// shows all twenty-nine of its triggers doing `Display World Text` with a
// `Text Label` (`TUTO1_TUTOR_1_B`, `_C`, `_E`, ...), never a dialogue node and
// never a `WDialog`.  So the tutorial's whole scripted chain is already this
// port's `ActionDisplayWorldText` path; what was missing was the WINDOW.
//
// f0353-f0355, f0373 and f0442 all show the same panel, and it ships:
// `gui/back/Tutor.zar` is 400 x 472, which is the row's "about 400 x 475 at
// x 762-1160, y 235-710" exactly.  Its upper two thirds are a black CRT behind
// a brass bezel and its lower third an olive riveted apron carrying one DONE
// button.  The interior geometry is MEASURED off f0373 against the art: the CRT
// runs (38, 38) to (385, 332), the text column x 48..355 from y 48, the green
// scroll thumb sits at x 360..368 over a track y 45..325, and DONE is centred
// at y 447 with a dark-red domed lens at its left.  The title line is the
// shipped `title_<label>` when the mission's table has one; the body is the
// label itself.  Both are the console green the capture reads.
void Ui::DrawWorldText(Scene* scene, Canvas* c) {
    const MissionScript& sc = scene->Script();
    if (sc.WorldText().empty()) return;
    const std::string key = sc.WorldText();
    // The shipped tutorial strings carry `WBOSText`'s own markup -- tut1's
    // body ends "click <Cy>DONE<C->" -- so both go through the same stripper
    // the popup help uses (NOTES.md 105).
    const std::string title = HelpText("title_" + key);
    const std::string body = HelpText(key);
    const Art* back = Res()->Image("gui/back/Tutor.zar");
    const int32_t w = back->ok ? back->w : 400, h = back->ok ? back->h : 472;
    const Rect r = Modal(w, h);
    if (back->ok) {
        c->Blit(*back, r.x0, r.y0);
    } else {
        c->Fill(r.x0, r.y0, r.x1, r.y1, 0x000000, 230);
    }
    // The CRT.
    const int32_t tx0 = r.x0 + 48, tx1 = r.x0 + 355;
    const int32_t cy0 = r.y0 + 38, cy1 = r.y0 + 332;
    c->SetClip(r.x0 + 38, cy0, r.x0 + 385, cy1);
    const int32_t lh = m_style.normal.Height() + 3;
    int32_t y = r.y0 + 48;
    if (title != "title_" + key) {
        m_style.normal.Draw(c, tx0, y, title, m_style.consoleGreen);
        y += lh + 4;
    }
    const std::vector<std::string> lines = Wrap(m_style.normal, body, tx1 - tx0);
    for (const std::string& l : lines) {
        m_style.normal.Draw(c, tx0, y, l, m_style.consoleGreen);
        y += lh;
    }
    c->SetClip(0, 0, m_W, m_H);
    // The thumb: a narrow solid green bar hard against the CRT's right inside
    // edge, sized to the fraction of the body that fits -- the same treatment
    // the briefing page uses (NOTES.md 113).
    const int32_t track0 = r.y0 + 45, track1 = r.y0 + 325;
    const int32_t need = static_cast<int32_t>(lines.size()) * lh + 12;
    const int32_t have = cy1 - cy0;
    if (need > have) {
        const int32_t th = std::max<int32_t>(12, (track1 - track0) * have / need);
        c->Fill(r.x0 + 360, track0, r.x0 + 369, track1, 0x000000);
        c->Fill(r.x0 + 360, track0, r.x0 + 369, track0 + th, m_style.consoleGreen);
    }
    // DONE, with its lens.
    const int32_t dy = r.y0 + 437;
    c->Fill(r.x0 + 232, dy + 3, r.x0 + 242, dy + 13, 0x8A1410u);
    c->Fill(r.x0 + 234, dy + 5, r.x0 + 240, dy + 11, 0xD02018u);
    m_style.medium.DrawAligned(c, r.x0 + 248, dy, r.x0 + 330, dy + 18,
                               m_loc.Get("genericGui_done"), m_style.bosText, 0);
    m_worldTextDone[0] = r.x0 + 228;
    m_worldTextDone[1] = dy - 2;
    m_worldTextDone[2] = r.x0 + 335;
    m_worldTextDone[3] = dy + 20;
    m_trace += "worldtext \"" + body + "\"\n";
}

void Ui::DrawObjectiveList(Scene* scene, Canvas* c, int32_t x, int32_t y, int32_t w) {
    const MissionScript& sc = scene->Script();
    const int32_t lh = m_style.medium.Height() + 3;
    for (const Objective& o : sc.Objectives()) {
        const std::string text = m_loc.Get(o.key);
        const std::vector<std::string> lines = Wrap(m_style.medium, text, w - 24);
        // The completed marker: the engine emits the localized
        // `objective_complete` line; the tick here is this port's.
        m_style.medium.Draw(c, x, y, o.complete ? "[X]" : "[ ]",
                            o.complete ? m_style.buttonText : m_style.consoleGreen);
        for (const std::string& l : lines) {
            m_style.medium.Draw(c, x + 40, y, l,
                                o.complete ? m_style.buttonText : m_style.consoleGreen);
            y += lh;
        }
        if (lines.empty()) y += lh;
        m_trace += std::string("objective ") + (o.complete ? "[X] " : "[ ] ") + o.key + " \"" +
                   text + "\"\n";
    }
}

// The popup box `ActionAddObjective`'s `with popup box` flag raises, and that
// `ActionCompleteObjective` raises unconditionally.
void Ui::DrawObjectivePopup(Scene* scene, Canvas* c) {
    MissionScript& sc = scene->Script();
    if (sc.ObjectivePopup()) {
        m_objectivePopupUntil = scene->NowMs() + 6000;
        sc.ClearObjectivePopup();
    }
    if (scene->NowMs() > m_objectivePopupUntil) return;
    const int32_t w = 420;
    int32_t h = 24 + m_style.large.Height();
    for (const Objective& o : sc.Objectives()) {
        h += (m_style.medium.Height() + 3) *
             static_cast<int32_t>(std::max<size_t>(1, Wrap(m_style.medium, m_loc.Get(o.key),
                                                           w - 48).size()));
    }
    const int32_t x = (m_W - w) / 2, y = 200;
    c->Fill(x, y, x + w, y + h, 0x000000, 225);
    c->Fill(x, y, x + w, y + 1, m_style.consoleGreen);
    c->Fill(x, y + h - 1, x + w, y + h, m_style.consoleGreen);
    c->Fill(x, y, x + 1, y + h, m_style.consoleGreen);
    c->Fill(x + w - 1, y, x + w, y + h, m_style.consoleGreen);
    m_style.large.DrawAligned(c, x, y + 6, x + w, y + 6 + m_style.large.Height(),
                              m_loc.Get("objective_complete"), m_style.buttonText, 2);
    DrawObjectiveList(scene, c, x + 16, y + 12 + m_style.large.Height(), w - 32);
}

// ActionDisplayDebrief: the full-screen debrief.
void Ui::DrawDebrief(Scene* scene, Canvas* c) {
    const MissionScript& sc = scene->Script();
    const int32_t w = std::min<int32_t>(760, m_W - 40);
    const int32_t x = (m_W - w) / 2, y = 30;
    const int32_t h = std::min<int32_t>(m_H - 60, ViewportHeight(m_H) - 20);
    c->Fill(x, y, x + w, y + h, 0x000000, 240);
    c->Fill(x, y, x + w, y + 2, m_style.buttonText);
    c->Fill(x, y + h - 2, x + w, y + h, m_style.buttonText);
    c->Fill(x, y, x + 2, y + h, m_style.buttonText);
    c->Fill(x + w - 2, y, x + w, y + h, m_style.buttonText);
    c->SetClip(x, y, x + w, y + h);

    int32_t ty = y + 12;
    const std::vector<std::pair<int32_t, std::string>>& lines = sc.DebriefLines();
    std::string title = "MISSION DEBRIEFING";
    if (!lines.empty()) {
        const std::string t = m_loc.Get("title_" + lines.front().second);
        if (t != "title_" + lines.front().second) title = t;
    }
    m_style.large.DrawAligned(c, x, ty, x + w, ty + m_style.large.Height(), title,
                              m_style.buttonText, 2);
    ty += m_style.large.Height() + 10;
    m_trace += "debrief title \"" + title + "\"\n";

    const int32_t lh = m_style.medium.Height() + 3;
    for (const std::pair<int32_t, std::string>& kv : lines) {
        const std::string text = m_loc.Get(kv.second);
        for (const std::string& l : Wrap(m_style.medium, text, w - 48)) {
            m_style.medium.Draw(c, x + 24, ty, l, m_style.consoleGreen);
            ty += lh;
        }
        ty += lh / 2;
        m_trace += "debrief line " + std::to_string(kv.first) + " " + kv.second + "\n";
    }
    ty += lh;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "EXPERIENCE  %d          REPUTATION  %+d", sc.Experience(),
                  sc.Reputation());
    m_style.medium.DrawAligned(c, x, ty, x + w, ty + lh, buf, m_style.buttonText, 2);
    c->SetClip(0, 0, m_W, m_H);
}

}  // namespace game
