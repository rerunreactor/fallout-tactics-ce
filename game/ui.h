// ui.h -- milestone 9: the in-game interface, drawn with the game's own art.
//
// SOURCES.  Everything in this file is ported from the shipped executable
// (via the Ghidra export; the BOS.exe and BOS_HR.exe 1.27 builds differ only
// in two display-mode functions), from the shipped data under `data/core/gui`,
// and from four `.data` tables read straight out of the executable.  Every
// coordinate below names the function and address it came from; anything
// that is a reconstruction says so.
//
// The five things the coordinator asked to have established:
//
//  * FONT.  A font is a plain 32-bit RGBA **PNG strip**, one row of glyphs,
//    every glyph the full strip height, separated by full-height magenta
//    (0xFF00FF) columns.  `FontManager::GetOrCreateFont @0075e740` loads it
//    through the image-format manager and splits it into an 8-bit paletted
//    colour image plus an 8-bit grey alpha image;
//    `AntiFont::BuildFontKerningPairs @0077f0e0` (misnamed -- it builds no
//    kerning table) finds the separator palette index by scanning for the
//    entry whose bytes are FF 00 FF and then walks row 0 pushing every x whose
//    pixel is that index, bracketed by an explicit `push(0)` and `push(width)`.
//    Glyph i is columns [sep[i], sep[i+1]).  `AntiFont::MeasureTextExtent
//    @0077fcb0` gives the advance without ambiguity: `w += glyphWidth +
//    spacing`, and `AntiFont::GetGlyphRenderOffset @0077fb50` blits one glyph
//    and returns the same advance.  There is NO pair kerning anywhere.
//    Colour is three floats at `AntiFont+0x20/24/28` installed by
//    `SetAmbientLightColor @00709820` before each glyph, and the blit is the
//    tinting one, so a glyph is `dst = lerp(dst, srcRGB * tint, srcAlpha)`.
//    The shipped normal fonts store RGB = white with the antialias ramp in
//    alpha; `fnt_monitor_outline` inverts that (binary alpha, a grey ramp in
//    RGB) so that multiplying by the tint keeps its black outline black.
//    Char -> glyph index is `ch - 32` (`FUN_0077faa0`, body absent; the
//    mapping is read off the shipped strips), with a case-swap retry in
//    `AntiFont::GetGlyphIndexForChar @0077fad0`.
//
//  * STYLE.  There is no hardcoded skin.  `DefaultStyle::DefaultStyle
//    @0072fed0` parses **`gui/def_style.cfg`**, which ships, and that file is
//    the authority for the eight font paths, the sixteen colours (written as
//    `Color(r,g,b)` with 0..255 components, kept internally as float triples
//    and packed to BGRA at draw time by `DefaultStyle::DrawStyleColorEdge
//    @00733a80`), the cursors and the wallpaper.  Widget CHROME is not art at
//    all: `DefaultStyle::DrawButtonBorder @00733f50` and its four siblings
//    draw six one-pixel rectangles from the four-colour bevel ramp
//    panelLight / panelFace / panelShadow / panelDkShadow.  Only buttons,
//    checkboxes, scrollers and the pulldown load `.zar` art, and those paths
//    are in the widget constructors (`WBOSButton::WBOSButton @0065bcb0`
//    picks one of eleven facings at random with `gui\btn\<size>\%d`).
//
//  * GAME BAR.  `WGameBar::WGameBar @0047c3b0` is 12 kB of constructor whose
//    inline coordinates are a seed; its last statement calls
//    `WGameBar::LayoutGameBarControls @0047f420`, which is authoritative.  The
//    owning screen (`WGameMain::WGameMain@00470990`) sizes the world viewport
//    `(0, 0, screenW, screenH - 128)` and the bar window
//    `((screenW-800)/2, screenH-128, +800, screenH)`: **the bar is always
//    exactly 800 x 128, bottom-anchored and centred**, and every widget
//    coordinate inside it is a hardcoded literal in that 800 x 128 space.
//    The hi-res trick is one line: `WGameBar::CenterChildHorizontally
//    @0047fa20` centres the 1024 x 128 `gui/back/game_bar_long.zar` on the
//    800-wide window, i.e. at x = -112, so the backdrop covers a 1024 screen
//    while nothing else moves.
//
//  * INVENTORY.  The equip screen's own window class is in NO dump -- there is
//    no `WIIBox` and no `WIISlot` in the decompilation, and nothing references
//    `gui/back/equip`.  What IS ported is the equip SLOT, `WIIBase` /
//    `WIIHand`, which the game bar and the equip screen share:
//    `WIIBase::Draw @00490b50` (the item icon: the item entity's own sprite,
//    animation `guibig` or `guismall` picked by a flag, NOT scaled, centred
//    vertically, with the count in a strip at y 62..74), `WIIHand::Draw
//    @00492d20` (the mode name, the AP line `"%d%s"` + `mode_AP`, the
//    `desc_noammo` banner, the 39-tick ammo bar at x 134/135),
//    `WIIBase::HandleMouseEvent @00490870` and
//    `WIIBase::CheckDragThresholdExceeded @00490600` (a drag starts once the
//    squared distance from the press point exceeds 100, i.e. 10 px) and
//    `WIIBase::SetChildWindowVisible @00490690` (the cursor carries a
//    140 x 69 ghost of the slot, offset so the grab point stays under the
//    pointer).  There is no size grid and no type mask: legality is one
//    `Entity::CanReachInteractable` / `Entity::CanUseItem` call on the
//    receiving side.  **FT's container view is a scrolling LIST, not a grid**
//    (`WIIBase` derives from `WListItem`; the panes are `WInvActor`), so the
//    "backpack grid" this milestone was asked for does not exist in this game
//    and is drawn here as the list the art shows.
//
// The `.data` tables the decompilation left as bare `DAT_`/`PTR_` symbols were
// read out of the executable and are transcribed at their point of use:
//   0089a784 "MNU" 0089a788 "PIP" 0089a78c "CHR" 0089a790 "INV" 0089a794 "SKL"
//   00895448 "%d"  0089b848 "x%d" 0089b84c "%d%s" 0089b5b8 "x %d"
//   008a6b70  the 7 SPECIAL keys, 008a6b18 the 18 skill keys,
//   00898f34  the 12 derived-stat keys and 00898f68 their percent flags.

#ifndef GAME_UI_H
#define GAME_UI_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {

class Scene;
struct LiveEntity;

// ---------------------------------------------------------------------------
// Pixels.
// ---------------------------------------------------------------------------
struct Art {
    int32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;      // straight alpha, RGBA byte order
    bool ok = false;
};

// The window framebuffer, with a clip rectangle.  `fb` is the same RGBA buffer
// `Scene::Render` paints into.
struct Canvas {
    uint8_t* fb = nullptr;
    int32_t w = 0, h = 0;
    int32_t clip[4] = {0, 0, 0, 0};     // x0, y0, x1, y1

    void SetClip(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void Pixel(int32_t x, int32_t y, uint32_t rgb, uint8_t a);
    void Fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t rgb, uint8_t a = 255);
    void Blit(const Art& a, int32_t x, int32_t y);
    // `WIIBase::Draw`'s colour modulation: the icon is multiplied by a per
    // channel scale before the alpha blend.
    void BlitTinted(const Art& a, int32_t x, int32_t y, float r, float g, float b);
    // MILESTONE 18: the same blit over a raw RGBA buffer, which is what
    // `Scene::ItemIcon` hands back (a `guibig`/`guismall` collection frame
    // composited out of the item sprite's four layers).
    void BlitRGBA(const uint8_t* rgba, int32_t w, int32_t h, int32_t x, int32_t y);
};

// ---------------------------------------------------------------------------
// MILESTONE 16: where a BOS button's label actually goes.
//
// `WBOSButton::WBOSButton @0065bcb0` does NOT draw its own text.  It news a
// child `WBOSLargeText` / `WBOSMediumText` window and gives it a rect that is
// a hardcoded inset of the button, and only THEN resizes the button itself to
// the art (`WWindow::ResizeWindow(this, {img.w, img.h})` at the tail) --
// leaving that inset alone.  The three literals, straight out of the
// constructor's three arms:
//
//   size 0 "big"    (0x39, 2, 0xe3, 0x27) = (57, 2)-(227, 39)   art 240 x 40
//   size 1 "med"    (0x20, 1, 0x92, 0x15) = (32, 1)-(146, 21)   art 152 x 24
//   size 2 "small"  (0x20, 1, 0x7a, 0x15) = (32, 1)-(122, 21)   art 125 x 24
//
// so a label is NOT centred on the button: the art's decorative left cap is
// excluded, and the text sits right of the button's own centre by 22 px (big),
// 13 px (med) and 14 px (small).  Centring on the whole rect -- which is what
// this port did everywhere -- puts every label too far left, which is what the
// player saw on both menus.
//
// Within that rect `DefaultStyle::DrawStyledText @00731bc0` does the placing:
//   x = left                              (align 0, left)
//     = left + (width - textWidth)        (align 1, right)
//     = left + (width - textWidth) / 2    (align 2, centre)
// and in ALL THREE cases
//   y = top + ((bottom - top) - fontHeight) / 2
// with NO baseline offset.  `Font::DrawAligned` below is that function.
// (Aligns 3/4/5 are the word-wrapping forms; their line pitch is
// `fontHeight + fontHeight/4`, which closes NOTES.md 35's "the line spacing
// between two drawn lines is not recoverable".)
struct BosLabelInset {
    int32_t x0, y0, x1, y1;
};
constexpr BosLabelInset kBosLabelBig{57, 2, 227, 39};
constexpr BosLabelInset kBosLabelMed{32, 1, 146, 21};
constexpr BosLabelInset kBosLabelSmall{32, 1, 122, 21};

// ---------------------------------------------------------------------------
// Fonts.  See the header comment.
// ---------------------------------------------------------------------------
class Font {
public:
    // `spacing` is FontManager::GetOrCreateFont's second argument: 0 for every
    // font except `font.lite` (-1) and `font.consoleOutline` (-2)
    // (`DefaultStyle::DefaultStyle@0072fed0`).
    bool Load(const std::string& pngPath, int32_t spacing, std::string* errorOut);
    bool Ok() const { return m_ok; }
    int32_t Height() const { return m_height; }       // AntiFont+0x1c
    int32_t Measure(const std::string& s) const;      // AntiFont::MeasureTextExtent
    // `AntiFont::ProcessCharacterSequence` threading the pen through
    // `GetGlyphRenderOffset`.  Returns the pen's end x.
    int32_t Draw(Canvas* c, int32_t x, int32_t y, const std::string& s, uint32_t rgb) const;
    // `DefaultStyle::DrawStyledText @00731bc0`: align 0 left, 1 right,
    // 2 centre, inside [x0, x1).
    void DrawAligned(Canvas* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const std::string& s, uint32_t rgb, int align) const;

private:
    struct Glyph { int32_t x0 = 0, w = 0; };
    std::vector<Glyph> m_glyphs;
    std::vector<uint8_t> m_rgba;    // the whole strip
    int32_t m_w = 0, m_height = 0;
    int32_t m_spacing = 0;
    bool m_ok = false;
};

// ---------------------------------------------------------------------------
// The resource root.  Windows lookup is case-insensitive and the shipped cfg
// disagrees with the shipped filenames in case on essentially every font, so
// every lookup here folds case (see NOTES.md).
// ---------------------------------------------------------------------------
class ResourceRoot {
public:
    explicit ResourceRoot(const std::string& assetsDir);
    // "gui/back/game_bar_long.zar" -> the real path, or "".
    std::string Resolve(const std::string& rel) const;
    // Loads and caches a .zar (or a .png).  Never returns null; a miss yields
    // an Art with ok == false, which the drawing code skips.
    const Art* Image(const std::string& rel);
    const std::vector<std::string>& Missing() const { return m_missing; }
    // Every indexed file whose relative path starts with `relDir` and ends
    // with `ext`, real paths, sorted.
    std::vector<std::string> List(const std::string& relDir, const std::string& ext) const;

private:
    std::string m_dir;
    std::unordered_map<std::string, std::string> m_index;   // lowercased -> real
    std::map<std::string, Art> m_cache;
    mutable std::vector<std::string> m_missing;
};

// ---------------------------------------------------------------------------
// MILESTONE 18: the mouse cursors.
//
// The art and the hotspots are SHIPPED DATA, not inference: `gui/cursors.cfg`
// (inside `gui_0.bos`) is a table of
//
//     cursor_move = {Cursor("gui/cursor/move", Point(15,9) )};
//
// and `LoadCursorDefinitions @006605f0` -- called once from
// `GameApplication::MainInit @006c4180` with the array at 0x008bdaf0 -- reads
// exactly that file and fills the array IN THIS ORDER:
//
//   0 hand   1 skill   2 use   3 attack   4 attackFriendly   5 move
//   6 moveFormation    7 run   8 moveBad  9 cancelUse       10 select
//  11 speech 12 drag  13 climbUp 14 climbDown 15 vehicleIn  16 vehicleOut
//  17..24  scroll0..scroll7   (`"cursor_scroll%d"`, the eight edge arrows)
//
// which is what pins every `DAT_008bdaf4..008bdb50` the two cursor-choosing
// functions return to a name.  `cursor_normal` and `cursor_caret` come from
// `gui/def_style.cfg` instead.  **`cursor_loot` is in `cursors.cfg` and
// `LoadCursorDefinitions` never loads it**, and **`cursor_run` (0x008bdb0c) is
// loaded and then referenced by NOTHING in the whole executable** -- a run
// order shows the plain `move` cursor.  Both were checked with a full
// data-reference scan over BOS_HR.exe.
struct CursorArt {
    const Art* art = nullptr;
    int32_t hotX = 0, hotY = 0;
};

// ---------------------------------------------------------------------------
// `gui/def_style.cfg`, parsed.  DefaultStyle::DefaultStyle @0072fed0.
// ---------------------------------------------------------------------------
struct Style {
    // The eight fonts, by the logical key the UI asks for.
    Font normal, console, consoleOutline, consoleLarge, lite, medium, large, tallThin;

    // Colours, as 0xRRGGBB.  Missing keys keep the fallback named beside them.
    uint32_t textTBD = 0xFF0000;
    uint32_t buttonYellow = 0xC4A028;
    uint32_t buttonText = 0x3CF800;
    uint32_t panelLight = 0x808080, panelFace = 0x404040;
    uint32_t panelShadow = 0x202020, panelDkShadow = 0x000000;
    uint32_t titlebarFill = 0x00FF00, titlebarText = 0x000000;
    uint32_t windowFill = 0x000000, windowText = 0xC4C4C4;
    uint32_t selectionFill = 0x00FF00, selectionText = 0x000000;
    // `color_bosText`, `color_consoleGreen` and `color_consoleSelect` are read
    // by the constructor but are absent from the shipped cfg and have no
    // visible fallback assignment, so what the original ends up with is not
    // recoverable FROM THE BINARY.
    //
    // MILESTONE 20: `bosText` is recoverable from the CAPTURE, which is full of
    // BOS-button labels.  Measured on the brightest 3 % of the glyph cores over
    // three separate plates -- the main menu's TUTORIALS (f0026), CHARACTER
    // SELECTION's TAKE and its title plate (f0036) -- the label reads
    // (224, 208, 120), (215, 203, 132) and (227, 209, 125): a khaki-cream, not
    // a green.  Every BOS button in the game therefore carries khaki-cream
    // text, which is what the capture shows and what this port drew green.
    // `consoleGreen` stays the cfg's green: the Pip-Boy body text and the AP
    // pips (S8) both measure green.
    uint32_t bosText = 0xE0D078;
    uint32_t consoleGreen = 0x3CF800;
    uint32_t consoleSelect = 0x00FF00;

    bool Load(ResourceRoot* res, std::string* errorOut);
};

// ---------------------------------------------------------------------------
// The localised strings.  `GetLocalizedString` is `FUN_00420760 @00420760`
// (body not in any dump); the tables are the shipped `locale/**/*.txt`, whose
// format is `key = {value}` with `//` comments and CRLF line ends.
// ---------------------------------------------------------------------------
class Locale {
public:
    void LoadDir(const ResourceRoot& res, const std::string& relDir);
    // MILESTONE 10.  The mission's own strings -- objective, world text,
    // debrief and speech keys -- do not live in `locale_0.bos`; they ship in
    // `loc-mis_<letter>.bos` as `locale/missions/<mission>/*.txt`, and their
    // values span several lines, so `key = {` ... `}` is scanned as a block
    // rather than a line.  `ActionDisplayWorldText::Vtable15 @00683920` reads
    // both `<key>` and `"title_" + <key>` out of that table.
    void LoadMissionFile(const std::string& path);
    // Returns the value, or the key itself when it is missing -- which is what
    // the original shows too (the lookup leaves the out string untouched and
    // the callers pre-seed it with the key).
    const std::string& Get(const std::string& key) const;
    bool Has(const std::string& key) const;

private:
    std::unordered_map<std::string, std::string> m_map;
    mutable std::string m_scratch;
};

// ---------------------------------------------------------------------------
// The interface.
// ---------------------------------------------------------------------------
// Which full-screen window is up.  `WGameMain`'s own switcher uses numbered
// modes (10 skilldex, 11 pipboy, 9 options, 15 quit-confirm; `WGameMain::SetActiveGameWindowMode@00471ad0`
// l.19298-19531) and the game bar's five buttons are `skilldex`, `inventory`,
// `character`, `pipboy` and `menu` (the command strings at 0089a054 and the
// captions at 0089a784..794).
// MILESTONE 16: MenuKeys and MenuOptions are the two in-mission menu pages
// that had nothing behind them (`gameMenu_keysHelp` and `gameMenu_options`).
// MILESTONE 17: Loot is `WLoot` + `WMultiEquipLoot`, the two-window container
// screen on `gui/back/rloot.zar`.
enum class UiScreen { None, Inventory, Character, Pipboy, Skilldex, Menu, Debrief,
                      MenuKeys, MenuOptions, Loot };

class Ui {
public:
    // MILESTONE 15.  The in-mission menu (MNU) drew six entries and wired
    // exactly one of them -- "done" closed the panel and the other five set a
    // hover field nothing ever read.  A player in the browser therefore had no
    // way to save, load or leave a mission once inside it: `CampaignLeaveMission`
    // and the save/load screens existed but their only caller was the `--cam`
    // command-line script.
    //
    // The menu is drawn by `Ui`, which knows nothing about the campaign, and
    // acted on by main.cpp, which does.  So the click records a choice here and
    // main.cpp takes it on the next tick.  Returns -1 when there is nothing
    // pending; the index is into `kMenuKeys` (1 = save, 2 = load, 4 = exit).
    int32_t TakeMenuChoice() {
        const int32_t c = m_menuChoice;
        m_menuChoice = -1;
        return c;
    }

    // The screen rectangle of in-mission menu entry `i` (0..5), for the same
    // reason `CampaignUi::Hots()` exists: the browser lane cannot see inside
    // the canvas, and the geometry depends on the size of the shipped
    // `gui/back/Menu.zar`, so hard-coding it in a test would be a guess. Returns
    // false when the menu is not up. Defined in ui_screens.cpp beside the draw
    // and hit-test code it must agree with.
    bool MenuButtonRect(int32_t i, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1);
    // MILESTONE 18: the same for the skilldex's eight entries, so the click
    // and the draw share one layout (and the verification can find them).
    bool SkilldexButtonRect(int32_t i, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1);

    Ui();
    ~Ui();

    // `assetsDir` is the extracted `data/core` tree: it must contain `gui/` and
    // `locale/`.
    bool Init(const std::string& assetsDir, std::string* errorOut);
    bool Ready() const { return m_ready; }
    const std::string& Info() const { return m_info; }

    // The world viewport, `WGameMain::WGameMain@00470990`: full width, screenH - 128.
    static int32_t BarHeight() { return 128; }
    static int32_t ViewportHeight(int32_t screenH) { return screenH - BarHeight(); }

    // MILESTONE 10: the mission locale table, and the camera the speech
    // bubbles are placed against.
    void LoadMissionStrings(const std::string& misPath);
    // MILESTONE 12: the campaign screens (game/campaign_ui.cpp) draw with the
    // same resource root, style and string table as the in-mission interface.
    ResourceRoot* Resources() { return m_res.get(); }
    const Style& Styles() const { return m_style; }
    void SetCamera(int64_t x, int64_t y) { m_camX = x; m_camY = y; }
    const Locale& Strings() const { return m_loc; }

    // MILESTONE 16.
    // The wall clock, for the tab double-click, and the keyboard modifiers,
    // which the tab click needs for `modSelect` (keys.cfg's `<shift>F1..F6`).
    void SetClock(int64_t ms) { m_nowMs = ms; }
    void SetModifiers(bool shift, bool ctrl) { m_shift = shift; m_ctrl = ctrl; }
    void SetVolumes(int32_t m, int32_t mu, int32_t sfx, int32_t sp) {
        m_volumes[0] = m; m_volumes[1] = mu; m_volumes[2] = sfx; m_volumes[3] = sp;
    }
    // A double-clicked squad tab asks main.cpp to centre the view on that
    // member; -1 when there is nothing pending.
    int32_t TakeCentreRequest() {
        const int32_t c = m_centreRequest;
        m_centreRequest = -1;
        return c;
    }
    // The six squad tabs' roster, shared by the bar, the hit test and the
    // select1..6 / selectAll actions.
    static std::vector<int32_t> SquadList(const Scene* scene);

    // One frame.  `fb` is the window's RGBA buffer.
    void Draw(Scene* scene, uint8_t* fb, int32_t W, int32_t H);

    // --- MILESTONE 18: the cursor ------------------------------------------
    // Which cursor the pointer is showing, by its `cursors.cfg` name without
    // the `cursor_` prefix ("move", "attack", "speech", "scroll3", ...).  The
    // rule is `WGameWorld::GetSelectionStatusMessage @004a2c40` for the world
    // and `WGameWorld::GetSelectionCursorIcon @0065b300` for the eight edge
    // arrows; see ui_screens.cpp.
    const std::string& CursorName() const { return m_cursorName; }
    // Draws it at the pointer, offset by the cfg's own hotspot.  Called last,
    // over everything, because the original hides the OS cursor and draws its
    // own (`display.cursorMode` = software).
    void DrawCursor(Canvas* c);
    // How far from a screen edge the eight scroll arrows come up.  There is no
    // margin in any dump (the four flags at `WGameWorld+0x106..0x109` are set
    // by a function that is not in the decompilation), so this is a labelled
    // RECONSTRUCTION -- and it is deliberately the same 8 px as main.cpp's
    // `kEdgeBand`, the band that actually scrolls, so the arrow appears
    // exactly where the scroll starts.
    static int32_t EdgeScrollMargin() { return 8; }

    // MILESTONE 18: the skill cursor.  `WGameWorld+0x1d3` is the flag and
    // `+0x1cf` the skill index; `WGameWorld::EvaluateSneakSkillCondition
    // @0049de10` (really "begin a skill use") sets both for seven of the eight
    // skilldex skills and applies `sneak` immediately instead.  The world
    // click consumes it once and clears it.
    const std::string& SkillCursor() const { return m_skillCursor; }
    void SetSkillCursor(const std::string& s) { m_skillCursor = s; }
    // The same rule, answered for an arbitrary point -- the headless probe.
    std::string CursorAt(Scene* scene, int32_t x, int32_t y) {
        const int32_t sx = m_mouseX, sy = m_mouseY;
        m_mouseX = x;
        m_mouseY = y;
        const std::string n = PickCursor(scene);
        m_mouseX = sx;
        m_mouseY = sy;
        return n;
    }

    // Input.  Returns true when the UI consumed the event.
    bool MouseMove(int32_t x, int32_t y);
    bool MouseDown(Scene* scene, int32_t x, int32_t y, bool right);
    bool MouseUp(Scene* scene, int32_t x, int32_t y, bool right);
    // A keys.cfg action name: "inventory", "character", "pipboy", "skilldex",
    // "menu", "stand", "crouch", "prone", "reload", "cycleWeaponModes",
    // "swapHands", "characterEndTurn/stop", "select1".."select6".
    bool Action(Scene* scene, const std::string& action);

    UiScreen Screen() const { return m_screen; }
    void SetScreen(UiScreen s) { m_screen = s; m_drag.active = false; }
    // The combat console the game bar's message list shows.
    void PushMessage(const std::string& s);
    // A synthetic drag, for the headless verification runs: press at (x0,y0),
    // release at (x1,y1), with the drag threshold satisfied.
    void SyntheticDrag(Scene* scene, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    // Debug: what the last Draw laid out, for the verification harness.
    std::string TraceLayout() const;

private:
    struct Rect { int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
    struct DragState {
        bool pressed = false, active = false;
        int32_t pressX = 0, pressY = 0;
        int32_t x = 0, y = 0;
        int32_t fromEntity = -1;
        int32_t fromSlot = -1;      // 0 hand1, 1 hand2, 2 armour, >=3 inventory index
        std::string label;
        // MILESTONE 17: -1 when the drag did not start in the loot window, else
        // the side it started on (0 container, 1 actor).
        int32_t lootSide = -1;
        int32_t lootRow = -1;
    };

    void DrawGameBar(Scene* scene, Canvas* c);
    void DrawHandSlot(Scene* scene, Canvas* c, int32_t index, int32_t bx, int32_t by,
                      int32_t hand);
    // MILESTONE 18: `WIIBase::Draw @00490b50`'s icon.  Blits the item's own
    // `guibig` / `guismall` animation, unscaled, centred in the (w, h) cell at
    // (x, y).  Returns false when the item has no sprite or no such animation,
    // which is the caller's cue to fall back to the name.
    bool DrawItemIcon(Scene* scene, Canvas* c, const std::string& spritePath, bool big,
                      int32_t x, int32_t y, int32_t w, int32_t h);
    void DrawMinimap(Scene* scene, Canvas* c, int32_t bx, int32_t by);
    void DrawInventory(Scene* scene, Canvas* c);
    // MILESTONE 17: the loot window.  `WLoot::WLoot @00495940` is a `WPicture`
    // on `gui/back/rloot` carrying a `WInvActor` list at (0xd3, 0xe)-(0x187,
    // 0x1ca), a `WEntityDisplay` at (0x20, 0x24)-(0xaa, 0xa4) and a
    // `WBOSButton` captioned `equipMenu_takeAll` at (0x2a, 0x1b0);
    // `WMultiEquipLoot::WMultiEquipLoot @004962a0` is the right-hand half, a
    // window at (400, 0)-(800, 0x1d8) with the SAME `WInvActor` rect and a
    // `WItemDisplay` at (0x20, 0x24)-(0xb4, 0x10e).  All eight numbers are the
    // constructors' own literals.
    void DrawLoot(Scene* scene, Canvas* c);
    void DrawCharacter(Scene* scene, Canvas* c);
    void DrawPipboy(Scene* scene, Canvas* c);
    void DrawSkilldex(Scene* scene, Canvas* c);
    void DrawMenu(Scene* scene, Canvas* c);
    void DrawMenuPage(Scene* scene, Canvas* c);
    void LoadHelpLines();
    void DrawTooltip(Canvas* c);
    // MILESTONE 19: one `locale/gui/help_text.txt` popup-help string, demarked.
    std::string HelpText(const std::string& key) const;
    void DrawMessages(Canvas* c, int32_t bx, int32_t by);
    void DrawDragGhost(Scene* scene, Canvas* c);
    // milestone 10
    void DrawSpeech(Scene* scene, Canvas* c);
    // MILESTONE 22 (VIDEO_GAPS M3/M4): one white outlined, centred, wrapped
    // block of text floating over an entity, with no box behind it.  Speech
    // and status words ("Detected") are the same draw.
    void DrawFloatingText(Canvas* c, const LiveEntity& e, const std::string& text,
                          uint32_t rgb, const char* traceTag);
    void DrawWorldText(Scene* scene, Canvas* c);
    void DrawObjectivePopup(Scene* scene, Canvas* c);
    void DrawDebrief(Scene* scene, Canvas* c);
    void DrawObjectiveList(Scene* scene, Canvas* c, int32_t x, int32_t y, int32_t w);
    // Wraps `s` to `width` pixels with `f`.
    std::vector<std::string> Wrap(const Font& f, const std::string& s, int32_t width) const;

    // The bar window's own origin, `WGameMain::WGameMain@00470990`.
    int32_t BarX() const { return (m_W - 800) / 2; }
    int32_t BarY() const { return m_H - 128; }
    // The centred modal windows the screens use.
    Rect Modal(int32_t w, int32_t h) const;

    int32_t HitSlot(Scene* scene, int32_t x, int32_t y, int32_t* entityOut) const;
    // MILESTONE 17.  Which loot list row is under (x, y)?  `side` is 0 for the
    // container / corpse (the `WLoot` half) and 1 for the actor (the
    // `WMultiEquipLoot` half); returns -1 for neither.
    int32_t HitLootRow(int32_t x, int32_t y, int32_t* side) const;

    ResourceRoot* Res() { return m_res.get(); }

    // MILESTONE 18.  Parses `gui/cursors.cfg` (and def_style's two) into
    // `m_cursors`, and picks one for the current pointer position.
    void LoadCursors();
    std::string PickCursor(Scene* scene);

    std::unique_ptr<ResourceRoot> m_res;
    Style m_style;
    Locale m_loc;
    bool m_ready = false;
    std::string m_info;

    int32_t m_W = 1024, m_H = 768;
    int32_t m_mouseX = -1, m_mouseY = -1;
    UiScreen m_screen = UiScreen::None;
    DragState m_drag;
    std::string m_tooltip;
    std::vector<std::string> m_messages;
    std::string m_trace;
    // Which of the six squad tabs the pointer is over, for the highlight.
    int32_t m_hoverTab = -1;
    int32_t m_menuHover = -1;
    // MILESTONE 15: which in-mission menu entry was last clicked, consumed once
    // by `TakeMenuChoice`. See the note there.
    int32_t m_menuChoice = -1;
    // The `mmmap` / `mmtext` checkboxes at (233, 49) and (233, 79) swap the
    // minimap for the message list in the same rect.
    bool m_showText = false;
    // The inventory list's scroll offset and the row geometry the last draw used.
    int32_t m_invScroll = 0;
    std::vector<Rect> m_invRows;
    std::vector<Rect> m_slotRects;    // hand1, hand2, armour on the equip screen
    // MILESTONE 17.  The loot window's two lists, their rows' screen rects and
    // the Take All / Done buttons, filled by the draw and read by the hit test
    // (and by the verification harness through `TraceLayout`).
    std::vector<Rect> m_lootRowsL, m_lootRowsR;
    Rect m_lootTakeAll, m_lootDone;
    int32_t m_lootScrollL = 0, m_lootScrollR = 0;

    // milestone 10
    int64_t m_camX = 0, m_camY = 0;
    // MILESTONE 22 (VIDEO_GAPS M7): the minimap's zoom step (0/1/2 -- the
    // shipped `gui/misc/pmap/small|medium|large.zar` rings) and the rect the
    // last DrawMinimap used, so a click in the corners can change it.
    int32_t m_miniZoom = 1;
    int32_t m_miniRect[4] = {0, 0, 0, 0};
    // VIDEO_GAPS L7: the tutorial popup's DONE button, from the last draw.
    int32_t m_worldTextDone[4] = {0, 0, 0, 0};
    int64_t m_objectivePopupUntil = 0;

    // milestone 16
    int64_t m_nowMs = 0;
    bool m_shift = false, m_ctrl = false;
    int32_t m_lastTab = -1;
    int64_t m_tabClickMs = -100000;
    int32_t m_centreRequest = -1;
    // The five selection groupings keys.cfg binds to F7..F11
    // (`WGameWorld::SaveSelectionToControlGroup @0049f7c0` /
    // `RecallControlGroupSelection @0049f810` -- both take a group index 0..5).
    std::vector<int32_t> m_groups[5];
    // `locale/gui/help.txt`, the shipped key/mouse reference, one line per
    // entry -- the content of the `gameMenu_keysHelp` page.
    std::vector<std::string> m_helpLines;
    // The four bos.cfg buses the options page shows, pushed in by main.cpp.
    int32_t m_volumes[4] = {100, 100, 100, 100};
    int32_t m_pageDone[4] = {0, 0, 0, 0};

    // milestone 18
    std::map<std::string, CursorArt> m_cursors;
    std::string m_cursorName = "normal";
    std::string m_skillCursor;
    // `game.formationMode` and `game.runMode` out of `bos.cfg`, pushed in by
    // main.cpp; the formation flag picks `moveFormation` over `move`
    // (`GetSelectionStatusMessage`'s `field_0x1ce == (DAT_008bda1c != 0)`).
    int32_t m_formationMode = 0;
    bool m_formationInvert = false;   // the F key, optionsGameHelp_formationMode
    bool m_campaignShell = false;
public:
    void SetFormationMode(int32_t m) { m_formationMode = m; }
    // MILESTONE 20.  Under the campaign shell the debrief is the Pip-Boy page
    // `CampaignLeaveMission` opens (campaign_ui.cpp), not milestone 10's own
    // box, so the box does not auto-raise there.  A mission run straight from
    // the command line has no shell and keeps the box.
    void SetCampaignShell(bool on) { m_campaignShell = on; }
    void SetFormationInvert(bool v) { m_formationInvert = v; }
private:
};

}  // namespace game

#endif  // GAME_UI_H
