// campaign_ui.cpp -- milestone 12's four full-screen campaign windows.
//
// ART.  Every background here ships and is named by its own window class:
//   gui/back/Menu.zar          the main menu (also milestone 9's in-game menu)
//   gui/back/WorldMap.zar      `WWorldWindow` / `WorldMapWindow @006b4cc0`
//   gui/back/RecruitSingle.zar `WRecruit::WRecruit @004d4320`
//   gui/back/saveload.zar      the save/load browser, `WSaveGameItem`'s list
//   gui/misc/worldm/*.zar      the world-map markers: `location10`, `location20`
//                              and `locationL` (the three marker sizes),
//                              `dest`, `enter`, `encounter`, plus the
//                              top-level `gui/misc/world_map_pos.zar` and
//                              `world_map_moving.zar` for the squad icon.
//
// LAYOUT.  None of the four window classes has a body in the decompilation --
// `WWorldWindow::RenderWorldView @006bbb00` draws the tile grid but its own
// rectangle comes from the window, and `WRecruit::WRecruit @004d4320` is a
// constructor whose child coordinates Ghidra dropped.  So the CONTENT below is
// the binary's and the RECTANGLES are this port's, exactly as milestone 9 §30
// had to do for the four in-mission screens.  What is not invented: the marker
// positions and radii (CoreMission "Loc x"/"Loc y"/"Radius" out of the `.cam`),
// the recruit list's membership (campaign.txt's `recruitsPool`, gated by tag),
// the portraits (`gui/char/<Display Name>.zar`) and the rank badges
// (`gui/rank/<Rank>.zar`), all of which milestone 9 already established.

#include "campaign_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "campaign.h"
#include "ui.h"

namespace game {
namespace {

constexpr int32_t kRowH = 22;

bool In(int32_t x, int32_t y, const int32_t r[4]) {
    return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
}

}  // namespace

CampaignUi::CampaignUi(Ui* ui) : m_ui(ui) {}

void CampaignUi::Button(Canvas* c, int32_t x, int32_t y, int32_t w, int32_t h,
                        const std::string& label, int id) {
    const Style& st = m_ui->Styles();
    // MILESTONE 16: pick the button art that FITS, and put the label in that
    // size's own child rect (`WBOSButton::WBOSButton @0065bcb0`, ui.h).  The
    // old code blitted the 152 px med art into any rect at all -- including
    // the 100 px world-map buttons, where it overflowed by 52 px -- and then
    // centred the text on the rect, which is not what the original does.
    const Art* med = m_ui->Resources()->Image("gui/btn/med/0.zar");
    const Art* small = m_ui->Resources()->Image("gui/btn/small/0.zar");
    const Art* btn = nullptr;
    const char* size = "med";
    BosLabelInset in = kBosLabelMed;
    if (med->ok && med->w <= w) {
        btn = med;
        in = kBosLabelMed;
        size = "med";
    } else if (small->ok && small->w <= w) {
        btn = small;
        in = kBosLabelSmall;
        size = "small";
    }
    int32_t lx0 = x, ly0 = y, lx1 = x + w, ly1 = y + h;
    const bool overPlate = m_mx >= x && m_mx < x + w && m_my >= y && m_my < y + h;
    if (btn != nullptr) {
        c->Blit(*btn, x, y);
        // MILESTONE 20: `WBOSButton::WBOSButton @0065bcb0` is a `WImageButton`
        // over the numbered plate whose UP / DOWN images are
        // `gui/btn/<size>/up.zar` and `dn.zar` -- the domed red lens at the
        // plate's left end.  Without it the capture's red lens is a grey
        // recess, which is what this port drew.
        const Art* lamp = m_ui->Resources()->Image(std::string("gui/btn/") + size +
                                                   (overPlate ? "/dn.zar" : "/up.zar"));
        if (lamp->ok) c->Blit(*lamp, x, y);
        lx0 = x + in.x0;
        ly0 = y + in.y0;
        lx1 = x + in.x1;
        ly1 = y + in.y1;
    } else {
        // No art narrow enough: `DefaultStyle::DrawButtonBorder @00733f50`'s
        // synthesised bevel, and the label centred on the whole rect.
        c->Fill(x, y, x + w, y + h, st.panelFace);
        c->Fill(x, y, x + w, y + 1, st.panelLight);
        c->Fill(x, y, x + 1, y + h, st.panelLight);
        c->Fill(x + w - 1, y, x + w, y + h, st.panelDkShadow);
        c->Fill(x, y + h - 1, x + w, y + h, st.panelDkShadow);
    }
    // MILESTONE 20: align 0, not 2.  On f0036 the plate spans x 108..260 and
    // BOTH labels -- the four-letter TAKE and the six-letter CREATE -- start at
    // x 140, i.e. exactly the inset's own left edge, so a BOS label is LEFT
    // aligned in its child rect and not centred in it.
    st.medium.DrawAligned(c, lx0, ly0, lx1, ly1, label,
                          overPlate ? st.selectionFill : st.bosText, btn != nullptr ? 0 : 2);
    Hot hot;
    hot.r[0] = x;
    hot.r[1] = y;
    hot.r[2] = x + w;
    hot.r[3] = y + h;
    hot.id = id;
    m_hot.push_back(hot);
}

// ---------------------------------------------------------------------------
// MILESTONE 20: the front end -- the two menus, the character browser and the
// Pip-Boy briefing.
//
// SOURCES.
//
//  * The TOP-LEVEL menu is `WMainMenu::InitChildControls @006c00f0`, read
//    whole: it makes a `WPicture` on `gui/back/title_screen.jpg`, calls
//    `WMainMenu::CreateTitleAndVersionText @006beee0` (a title `WBOSText` and
//    a version line built by concatenating four string fragments), and then
//    builds SIX `WBOSButton`s from `coord = (0x226, 0x32)` = (550, 50) with
//    `coord.y += 6 + (button.bottom - button.top)` after each -- captions
//    `mainMenu_intro`, `mainMenu_singlePlayer`, `mainMenu_multiplayer`,
//    `mainMenu_options`, `mainMenu_credits`, `mainMenu_exit`, handlers
//    `PlayIntroMovieSequence`, `CreateWSingleMain`, `LAB_006c1ba0`,
//    `CreateAndExecuteMenuCommand`, `ShowMainMenuBackdropOrCredits`,
//    `WWindow::Back`.  The window itself is (0, 0)-(800, 600)
//    (`WMainMenu::WMainMenu @006bff50`).
//
//    MILESTONE 21: `gui/back/title_screen.JPG` is a baseline JPEG, and the
//    port now decodes it -- `ft::DetectImageFormat` (common/image.cpp) sniffs
//    FF D8 the way `FormatManager::DetectImageFormatFromStream @ 006d2cb0`
//    does, and stb_image decodes it.  The backdrop is the game's own 800 x 600
//    painting again; the `flaminghelmet.zar` line below is left as a fallback
//    for a tree that ships without it, not as the normal path.
//
//  * The SINGLE PLAYER submenu -- the screen the capture opens on -- is
//    `WSingleMain`, whose body is in NO dump (`CreateWSingleMain` is a
//    function pointer with no definition).  Its layout is therefore MEASURED
//    OFF THE CAPTURE (f0026 and its neighbours, VIDEO_GAPS M11): six
//    `gui/btn/big` plates, 240 x 40, at x 547, tops 46 / 92 / 138 / 184, two
//    rows skipped, then 276 / 322 -- i.e. exactly the parent's own `y += 6 +
//    height` pitch of 46, with a two-row gap.  The captions are the shipped
//    keys `demoMenu_tutorials`, `mainMenu_new`, `mainMenu_load`,
//    `singleMenu_back`, `singleMenu_customCampaign`, `singleMenu_customMission`
//    -- which is the capture's word order exactly.
//
//  * The TUTORIALS submenu (f0359) is the same art with five contiguous
//    plates: `demoMenu_tut1` BASIC, `demoMenu_tut2a` AMBUSH, `demoMenu_tut2b`
//    STEALTH, `demoMenu_tut2c` SNIPER, `singleMenu_back`.
//
//  * CHARACTER SELECTION is `gui/back/sel_char.zar` (800 x 600).  Its window
//    class is in no dump; the LIST is the campaign file's own `prefabs`
//    vector, the PORTRAITS are `gui/char/big/<Display Name>.zar` (280 x 300,
//    which is the bust the capture shows), the strings are
//    `charSelectMenu_title` / `_select` / `_modify` / `_new` / `_back` and
//    `charSelect_story` = "'S STORY:", and the stat block is the same
//    `Attributes` the character sheet draws with `stat_rate1..10` for the
//    adjective column.  The rectangles are measured off f0030-f0049.
//
//  * The BRIEFING is `gui/back/Pipboy.zar` (800 x 472) with the five
//    `pipboy_*` captions in the capture's own order, `pipboy_startMission`
//    ("START MISSION") under the text, and the body out of the mission's own
//    locale table under the key `MISSION_BRIEF` -- which every shipped
//    `locale/missions/<mission>/*_Speech.txt` defines.  The debrief variant
//    shows the keys `ActionAddDebriefText::Vtable15 @00687c30` collected, in
//    the order `ActionDisplayDebrief::Vtable15 @00687f50` sorts them.
// ---------------------------------------------------------------------------

namespace {

// The capture's plate column.  See the note above: the pitch is the parent's
// own `6 + height`; the two-row gap is the capture's.
constexpr int32_t kPlateX = 547;
constexpr int32_t kPlateW = 240, kPlateH = 40;
constexpr int32_t kPlatePitch = 46;

std::vector<std::string> WrapText(const Font& f, const std::string& s, int32_t width) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush = [&]() {
        out.push_back(line);
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
            ++i;
            continue;
        }
        const char c = i < s.size() ? s[i] : ' ';
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || i == s.size()) {
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
    if (!line.empty()) out.push_back(line);
    // Collapse the runs of blank lines a "\n\n" pair leaves to a single one.
    std::vector<std::string> tidy;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].empty() && !tidy.empty() && tidy.back().empty()) continue;
        if (out[i].empty() && tidy.empty()) continue;
        tidy.push_back(out[i]);
    }
    return tidy;
}

}  // namespace

void CampaignUi::BigButton(Canvas* c, int32_t x, int32_t y, const std::string& label, int id) {
    const Style& st = m_ui->Styles();
    // `WBOSButton::WBOSButton @0065bcb0` picks one of eleven weathering
    // variants at random; the choice is made once per plate here, off the id,
    // so a redraw does not flicker.
    const Art* a = m_ui->Resources()->Image(
        "gui/btn/big/" + std::to_string(((id * 7 + y / kPlatePitch) % 11 + 11) % 11) + ".zar");
    const int32_t w = a->ok ? a->w : kPlateW, h = a->ok ? a->h : kPlateH;
    if (a->ok) {
        c->Blit(*a, x, y);
    } else {
        c->Fill(x, y, x + w, y + h, st.panelFace);
    }
    const bool over = m_mx >= x && m_mx < x + w && m_my >= y && m_my < y + h;
    // `WBOSButton::WBOSButton @0065bcb0` builds a `WImageButton` whose
    // BACKGROUND is the numbered plate and whose UP / DOWN images are
    // `gui/btn/big/Up.zar` and `Dn.zar` -- the 43 x 35 domed red lens at the
    // plate's left end.  The port drew the plate and not the lamp, so the
    // capture's red lens was a grey recess.
    const Art* lamp = m_ui->Resources()->Image(over ? "gui/btn/big/Dn.zar"
                                                    : "gui/btn/big/Up.zar");
    if (lamp->ok) c->Blit(*lamp, x, y);
    // The label goes in the button's own child rect, NOT centred on the plate
    // (ui.h, `kBosLabelBig`), and the capture shows it left aligned there.
    st.large.DrawAligned(c, x + kBosLabelBig.x0, y + kBosLabelBig.y0, x + kBosLabelBig.x1,
                         y + kBosLabelBig.y1, label, over ? st.selectionFill : st.bosText, 0);
    Hot hot{{x, y, x + w, y + h}, id};
    m_hot.push_back(hot);
}

// ---------------------------------------------------------------------------
// `WMainMenu` itself.
// ---------------------------------------------------------------------------
void CampaignUi::DrawTitleMenu(Canvas* c, int32_t W, int32_t H) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    // `WPicture` on `gui/back/title_screen.jpg`, at the window origin --
    // `WMainMenu::WMainMenu @006bff50` makes the window (0, 0)-(800, 600) and
    // `InitChildControls @006c00f0` gives the picture that whole rect, so the
    // 800 x 600 JPEG lands at (0, 0) inside it and `Origin` letterboxes the
    // pair on black.  Decoded since milestone 21 (common/image.cpp); the
    // helmet is only a fallback now.
    const Art* back = m_ui->Resources()->Image("gui/back/title_screen.JPG");
    if (!back->ok) back = m_ui->Resources()->Image("gui/back/flaminghelmet.zar");
    if (back->ok) c->Blit(*back, ox, oy);

    static const char* const kKeys[6] = {"mainMenu_intro",   "mainMenu_singlePlayer",
                                         "mainMenu_multiplayer", "mainMenu_options",
                                         "mainMenu_credits", "mainMenu_exit"};
    static const int kIds[6] = {kCmdIntro, kCmdSinglePlayer, kCmdMultiplayer,
                                kCmdOptions, kCmdCredits, kCmdExit};
    // (0x226, 0x32), then `y += 6 + height`.
    int32_t y = oy + 0x32;
    for (int32_t i = 0; i < 6; ++i) {
        BigButton(c, ox + 0x226, y, m_ui->Strings().Get(kKeys[i]), kIds[i]);
        y += 6 + kPlateH;
    }
    // `WMainMenu::CreateTitleAndVersionText @006beee0`: two `WBOSText`s, the
    // second built by concatenating four fragments and placed at
    // (?, height - 32)-(790, height - 20).
    st.lite.DrawAligned(c, ox + 10, oy + 600 - 32, ox + 790, oy + 600 - 20,
                        "Fallout Tactics: Brotherhood of Steel", st.bosText, 1);
    st.lite.Draw(c, ox + 10, oy + 600 - 20, m_status, st.windowText);
}

// ---------------------------------------------------------------------------
// MILESTONE 21: the two full-screen JPEG pages.
//
// Both exist because `common/image.cpp` now decodes JPEG; before it, the art
// was unreadable and `VIDEO_GAPS.md` L2 / L4 recorded them as "nothing".
//
// THE LOADING SCREEN (L2, f0050).
//
//   The exe carries the format string `gui/wait/wait%d.jpg` (BOS.exe @ the
//   string block around 43326) and the archive ships seven of them,
//   `wait1..wait7` -- so the picture is one of seven, chosen per load.  f0050
//   is `wait3.jpg`: resampled to 800 x 600 it differs from the frame by a mean
//   6.6/255 per channel, which is the capture's own MPEG noise; the next best
//   candidate is 48.  The progress bar is NOT in the art -- it is drawn over
//   it, and its geometry and colours are MEASURED OFF f0050 (no window class
//   for it appears in any dump):
//
//       outer rect   (20, 524)-(780, 543)   in the 800 x 600 surface
//       border       2 px, (90, 183, 60)
//       fill         (37, 142, 5), from the inside left edge rightwards
//
//   f0050 catches the bar about 80 % across, which is what pins the fill as a
//   left-anchored bar rather than a chaser.
//
// THE MISSION-END ART (L4, f0357).
//
//   Five pictures ship and the exe names all five together --
//   `gui/back/deathscreen2.jpg`, `deathscreen.jpg`, `failscreenfemale.jpg`,
//   `failscreenmale.jpg`, `winscreen.jpg`.  f0357 is `DEATHscreen.JPG` (mean
//   5.4/255; the next candidate is 77), so `Exit to = "Death Screen"` -- the
//   `ActionEndMission` argument script.cpp already parses -- is that one.
//   `"Fail Screen"` is one of the two FailScreen halves and WHICH ONE IS NOT
//   RECOVERED: the names say the player character's sex picks it, and this
//   port has no sex on a character, so it takes the male plate and says so.
//   `DEATHscreen2.jpg` and `WINscreen.JPG` have no observed trigger at all.
//
//   The page itself is bare: f0357 is the picture and nothing else, no plate,
//   no caption.  A click or a key dismisses it, which is the only behaviour
//   the capture shows.
// ---------------------------------------------------------------------------
void CampaignUi::DrawLoading(Canvas* c, int32_t W, int32_t H) {
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    const Art* back = m_ui->Resources()->Image(m_loadingImage);
    if (back->ok) c->Blit(*back, ox, oy);
    // The bar, measured off f0050.  Clamped so a caller cannot draw outside it.
    float p = m_loadingProgress;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const int32_t x0 = ox + 20, x1 = ox + 780, y0 = oy + 524, y1 = oy + 543;
    const uint32_t kBorder = 0x5ab73c;   // (90, 183, 60)
    const uint32_t kFill = 0x258e05;     // (37, 142, 5)
    c->Fill(x0, y0, x1, y0 + 2, kBorder);
    c->Fill(x0, y1 - 2, x1, y1, kBorder);
    c->Fill(x0, y0, x0 + 2, y1, kBorder);
    c->Fill(x1 - 2, y0, x1, y1, kBorder);
    const int32_t inner = (x1 - 2) - (x0 + 2);
    const int32_t fill = static_cast<int32_t>(static_cast<float>(inner) * p + 0.5f);
    if (fill > 0) c->Fill(x0 + 2, y0 + 2, x0 + 2 + fill, y1 - 2, kFill);
}

void CampaignUi::DrawEndScreen(Canvas* c, int32_t W, int32_t H) {
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    const Art* back = m_ui->Resources()->Image(m_endImage);
    if (back->ok) c->Blit(*back, ox, oy);
    // The whole picture is the hit area: f0357 carries no control of its own,
    // and the capture leaves it with a click.
    Hot hot{{ox, oy, ox + 800, oy + 600}, kCmdBack};
    m_hot.push_back(hot);
}

// ---------------------------------------------------------------------------
// `WSingleMain` -- the screen the capture opens on.
// ---------------------------------------------------------------------------
void CampaignUi::DrawMainMenu(Canvas* c, int32_t W, int32_t H) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    const Art* back = m_ui->Resources()->Image("gui/back/flaminghelmet.zar");
    if (back->ok) c->Blit(*back, ox, oy);

    static const char* const kKeys[6] = {"demoMenu_tutorials", "mainMenu_new", "mainMenu_load",
                                         "singleMenu_back", "singleMenu_customCampaign",
                                         "singleMenu_customMission"};
    static const int kIds[6] = {kCmdTutorials, kCmdNewGame, kCmdLoadGame, kCmdBack,
                                kCmdCustomCampaign, kCmdCustomMission};
    // Rows 0..3, then two rows skipped, then rows 6..7 -- the capture's own
    // spacing (VIDEO_GAPS M11).
    static const int32_t kRow[6] = {0, 1, 2, 3, 5, 6};
    for (int32_t i = 0; i < 6; ++i) {
        BigButton(c, ox + kPlateX, oy + 46 + kRow[i] * kPlatePitch,
                  m_ui->Strings().Get(kKeys[i]), kIds[i]);
    }
    // The capture has no title and no version text on this screen.
    if (!m_status.empty()) {
        st.lite.Draw(c, ox + 10, oy + 578, m_status, st.windowText);
    }
}

// ---------------------------------------------------------------------------
// The two list submenus: TUTORIALS, and the CUSTOM CAMPAIGN / CUSTOM MISSION
// pickers.  Same art, same plate column; the rows are the shipped files.
// ---------------------------------------------------------------------------
void CampaignUi::DrawListMenu(Canvas* c, int32_t W, int32_t H) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    const Art* back = m_ui->Resources()->Image("gui/back/flaminghelmet.zar");
    if (back->ok) c->Blit(*back, ox, oy);
    // Eleven plates fit between y 46 and the bottom; BACK is always last.
    const size_t maxRows = 10;
    const size_t n = std::min(m_list.size(), maxRows);
    for (size_t i = 0; i < n; ++i) {
        BigButton(c, ox + kPlateX, oy + 46 + static_cast<int32_t>(i) * kPlatePitch,
                  m_list[i].first, kCmdListPick + static_cast<int>(i));
    }
    BigButton(c, ox + kPlateX, oy + 46 + static_cast<int32_t>(n) * kPlatePitch,
              m_ui->Strings().Get("singleMenu_back"), kCmdBack);
    // f0359 carries no title and no other text: the plates are the whole
    // screen.  `m_listTitle` is kept for the status line only.
    if (!m_status.empty()) st.lite.Draw(c, ox + 10, oy + 578, m_status, st.windowText);
    (void)st;
}

// ---------------------------------------------------------------------------
// CHARACTER SELECTION.
// ---------------------------------------------------------------------------
void CampaignUi::DrawCharSelect(Canvas* c, int32_t W, int32_t H) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    const Art* back = m_ui->Resources()->Image("gui/back/sel_char.zar");
    if (back->ok) c->Blit(*back, ox, oy);

    const PrefabChar* p = SelectedChar();
    const uint32_t green = st.consoleGreen;
    if (p != nullptr) {
        // The bust.  `gui/char/big/<Display Name>.zar` is 280 x 300 and the
        // CRT's own interior is (30, 30)-(760, 345), so the portrait sits at
        // its left end.
        const Art* port = m_ui->Resources()->Image("gui/char/big/" + p->displayName + ".zar");
        if (!port->ok) port = m_ui->Resources()->Image("gui/char/" + p->displayName + ".zar");
        if (port->ok) {
            // MILESTONE 20: the CRT treatment.  `gui/char/big/*.zar` is PURE
            // GREYSCALE (every pixel has R == G == B in all five), and the
            // capture's bust is green with faint scanlines: over f0036's
            // portrait well the mean reads (39.7, 58.3, 29.0), i.e. a tint of
            // (0.68, 1.00, 0.50) on the grey, and the odd rows read 6 % darker
            // than the even ones.  `WPortrait::RenderPortraitAndPointsLabel
            // @004bc7d0` is the draw and its blit is the tinting one, so the
            // tint is where it belongs; the scanline pitch and depth are
            // MEASURED off the capture, not recovered.
            c->SetClip(ox + 32, oy + 32, ox + 330, oy + 344);
            c->BlitTinted(*port, ox + 36, oy + 36, 0.68f, 1.00f, 0.50f);
            for (int32_t sy = oy + 37; sy < oy + 344; sy += 2) {
                c->Fill(ox + 32, sy, ox + 330, sy + 1, 0x000000, 16);
            }
            c->SetClip(0, 0, W, H);
        }
        st.normal.DrawAligned(c, ox + 300, oy + 40, ox + 520, oy + 56, p->name, green, 2);

        // SPECIAL, with `stat_rate<value>` in the adjective column -- the same
        // table `locale/game/stat.txt` gives the character sheet.
        static const char* const kSpecial[7] = {"strength",     "perception", "endurance",
                                                "charisma",     "intelligence", "agility",
                                                "luck"};
        const int32_t v[7] = {p->attr.strength,     p->attr.perception, p->attr.endurance,
                              p->attr.charisma,     p->attr.intelligence,
                              p->attr.agility,      p->attr.luck};
        char buf[96];
        for (int32_t i = 0; i < 7; ++i) {
            const int32_t y = oy + 84 + i * 12;
            st.normal.DrawAligned(c, ox + 340, y, ox + 446, y + 11,
                                  m_ui->Strings().Get(std::string("name_") + kSpecial[i]), green,
                                  1);
            std::snprintf(buf, sizeof(buf), "%02d", v[i]);
            st.normal.DrawAligned(c, ox + 450, y, ox + 470, y + 11, buf, green, 1);
            const int32_t r = v[i] < 1 ? 1 : (v[i] > 10 ? 10 : v[i]);
            st.normal.Draw(c, ox + 478, y,
                           m_ui->Strings().Get("stat_rate" + std::to_string(r)), green);
        }

        // The four derived rows the capture shows.
        const int32_t d[4] = {p->maxHp, p->attr.armorClass, p->attr.maxActionPoints,
                              p->attr.meleeDamage};
        static const char* const kDer[4] = {"name_hitPoints", "name_armorClass",
                                            "name_maxActionPoints", "name_meleeDamage"};
        static const char* const kDerFallback[4] = {"Hit Points", "Armor Class", "Action Points",
                                                    "Melee Damage"};
        for (int32_t i = 0; i < 4; ++i) {
            const int32_t y = oy + 186 + i * 12;
            std::string label = m_ui->Strings().Get(kDer[i]);
            if (label == kDer[i]) label = kDerFallback[i];
            st.normal.DrawAligned(c, ox + 340, y, ox + 470, y + 11, label, green, 1);
            if (i == 0) {
                std::snprintf(buf, sizeof(buf), "%d/%d", p->hp, p->maxHp);
            } else {
                std::snprintf(buf, sizeof(buf), "%d", d[i]);
            }
            st.normal.Draw(c, ox + 478, y, buf, green);
        }

        // The tagged skills -- which is what the capture lists, three of them.
        static const char* const kSkillKeys[18] = {
            "smallGuns", "bigGuns",  "energyWeapons", "unarmed", "meleeWeapons", "throwing",
            "firstAid",  "doctor",   "sneak",         "lockpick", "steal",       "traps",
            "science",   "repair",   "pilot",         "barter",  "gambling",     "outdoorsman"};
        int32_t row = 0;
        for (int32_t i = 0; i < 18 && row < 4; ++i) {
            if (!p->attr.tagged[i]) continue;
            const int32_t y = oy + 246 + row * 12;
            st.normal.DrawAligned(c, ox + 340, y, ox + 470, y + 11,
                                  m_ui->Strings().Get(std::string("name_") + kSkillKeys[i]),
                                  green, 1);
            std::snprintf(buf, sizeof(buf), "%d%%", p->attr.skills[i]);
            st.normal.Draw(c, ox + 478, y, buf, green);
            ++row;
        }

        // `<NAME>` + `charSelect_story`, then the biography.
        st.normal.Draw(c, ox + 543, oy + 66, p->name + m_ui->Strings().Get("charSelect_story"),
                       green);
        int32_t y = oy + 82;
        for (const std::string& l : WrapText(st.normal, p->story, 180)) {
            if (y > oy + 336) break;
            st.normal.Draw(c, ox + 543, y, l, green);
            y += 12;
        }
    } else {
        st.medium.DrawAligned(c, ox + 40, oy + 180, ox + 760, oy + 200,
                              "no pre-made characters in this campaign", green, 2);
    }

    // The rusted apron: TAKE / arrows / MODIFY, the title plate, CREATE / BACK.
    Button(c, ox + 108, oy + 389, 152, 24, m_ui->Strings().Get("charSelectMenu_select"),
           kCmdCharTake);
    Button(c, ox + 521, oy + 389, 152, 24, m_ui->Strings().Get("charSelectMenu_modify"),
           kCmdCharModify);
    Button(c, ox + 108, oy + 517, 152, 24, m_ui->Strings().Get("charSelectMenu_new"),
           kCmdCharCreate);
    Button(c, ox + 521, oy + 517, 152, 24, m_ui->Strings().Get("charSelectMenu_back"), kCmdBack);
    {
        const Art* l = m_ui->Resources()->Image("gui/btn/ArrowL_Up.zar");
        const Art* r = m_ui->Resources()->Image("gui/btn/ArrowR_Up.zar");
        const int32_t lw = l->ok ? l->w : 29, lh = l->ok ? l->h : 24;
        if (l->ok) c->Blit(*l, ox + 361, oy + 396);
        if (r->ok) c->Blit(*r, ox + 410, oy + 396);
        m_hot.push_back(Hot{{ox + 361, oy + 396, ox + 361 + lw, oy + 396 + lh}, kCmdCharPrev});
        m_hot.push_back(Hot{{ox + 410, oy + 396, ox + 410 + lw, oy + 396 + lh}, kCmdCharNext});
    }
    st.large.DrawAligned(c, ox + 210, oy + 448, ox + 590, oy + 472,
                         m_ui->Strings().Get("charSelectMenu_title"), st.bosText, 2);
    if (!m_status.empty()) {
        st.lite.DrawAligned(c, ox + 0, oy + 578, ox + 800, oy + 594, m_status, st.windowText, 2);
    }
}

// ---------------------------------------------------------------------------
// The Pip-Boy briefing / debrief page.
// ---------------------------------------------------------------------------
std::vector<std::string> CampaignUi::BriefTabLines(int32_t tab, int32_t width) const {
    const Style& st = m_ui->Styles();
    std::vector<std::string> out;
    auto push = [&](const std::vector<std::string>& v) {
        for (const std::string& s : v) {
            const std::vector<std::string> w = WrapText(st.normal, s, width);
            out.insert(out.end(), w.begin(), w.end());
            out.push_back(std::string());
        }
    };
    switch (tab) {
        case 0: return WrapText(st.normal, m_brief.brief, width);
        case 1: push(m_brief.dialogue); break;
        case 2: push(m_brief.map); break;
        case 3: push(m_brief.archives); break;
        default: break;
    }
    return out;
}

void CampaignUi::ScrollBriefing(int32_t d) {
    m_briefScroll += d;
    const int32_t maxTop = std::max(0, m_briefLines - m_briefPage);
    if (m_briefScroll > maxTop) m_briefScroll = maxTop;
    if (m_briefScroll < 0) m_briefScroll = 0;
}

void CampaignUi::DrawBriefing(Canvas* c, int32_t W, int32_t H) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    int32_t ox = 0, oy = 0;
    Origin(W, H, &ox, &oy);
    const Art* back = m_ui->Resources()->Image("gui/back/Pipboy.zar");
    if (back->ok) c->Blit(*back, ox, oy);

    // The five buttons down the casing's left face.  The rects are this port's
    // (no `WPipboy` child list survives in the dumps); the captions and their
    // order are the shipped keys, and they are the capture's order exactly.
    static const char* const kTabs[5] = {"pipboy_briefings", "pipboy_dialogue", "pipboy_map",
                                         "pipboy_archives", "pipboy_off"};
    static const int kTabIds[5] = {kCmdPipTab + 0, kCmdPipTab + 1, kCmdPipTab + 2,
                                   kCmdPipTab + 3, kCmdPipTab + 4};
    for (int32_t i = 0; i < 5; ++i) {
        const int32_t bx = ox + 16, by = oy + 322 + i * 30;
        const Art* b = m_ui->Resources()->Image("gui/btn/small/0.zar");
        const int32_t bw = b->ok ? b->w : 125, bh = b->ok ? b->h : 24;
        if (b->ok) c->Blit(*b, bx, by);
        // The capture draws all five in the same khaki-cream; there is no
        // selected-tab highlight on any frame, so only the hover differs.
        const bool over = m_mx >= bx && m_mx < bx + bw && m_my >= by && m_my < by + bh;
        st.medium.DrawAligned(c, bx + kBosLabelSmall.x0, by + kBosLabelSmall.y0,
                              bx + kBosLabelSmall.x1, by + kBosLabelSmall.y1,
                              m_ui->Strings().Get(kTabs[i]),
                              over ? st.selectionFill : st.bosText, 0);
        m_hot.push_back(Hot{{bx, by, bx + bw, by + bh}, kTabIds[i]});
    }

    // The CRT.  `gui/back/Pipboy.zar`'s own black interior measures
    // (230, 32)-(771, 447); the capture's text column runs x 275..690 with the
    // scroll thumb hard against x 698 (VIDEO_GAPS M13).
    // MEASURED on f0104: the green text occupies x 275..687 and its rows start
    // at y 67 with a pitch of 11.25 px.  The pitch here is the engine's own
    // wrapping rule instead -- `DefaultStyle::DrawStyledText @00731bc0`'s
    // aligns 3/4/5 use `fontHeight + fontHeight / 4` (ui.h) -- which for the
    // 10 px `font.normal` is 12.
    const int32_t tx0 = ox + 275, tx1 = ox + 688;
    const int32_t ty0 = oy + 67, ty1 = oy + 404;
    const int32_t lh = st.normal.Height() + st.normal.Height() / 4;
    m_briefPage = (ty1 - ty0) / lh;
    const std::vector<std::string> lines = BriefTabLines(m_pipTab, tx1 - tx0);
    m_briefLines = static_cast<int32_t>(lines.size());
    if (m_briefScroll > std::max(0, m_briefLines - m_briefPage)) {
        m_briefScroll = std::max(0, m_briefLines - m_briefPage);
    }
    c->SetClip(tx0, ty0, tx1 + 2, ty1);
    int32_t y = ty0;
    for (int32_t i = m_briefScroll; i < m_briefLines && y < ty1; ++i) {
        st.normal.Draw(c, tx0, y, lines[static_cast<size_t>(i)], st.consoleGreen);
        y += lh;
    }
    c->SetClip(0, 0, W, H);

    // The scrollbar: a bare green thumb against the screen's right inside
    // edge, no arrows and no trough art -- which is what the capture shows.
    if (m_briefLines > m_briefPage) {
        const int32_t trackY0 = oy + 55, trackY1 = oy + 395;
        const int32_t track = trackY1 - trackY0;
        int32_t th = track * m_briefPage / m_briefLines;
        if (th < 16) th = 16;
        const int32_t maxTop = m_briefLines - m_briefPage;
        const int32_t ty = trackY0 + (track - th) * m_briefScroll / std::max(1, maxTop);
        c->Fill(ox + 698, ty, ox + 703, ty + th, st.consoleGreen);
    }

    // `pipboy_startMission`, green, uppercase, underlined, centred under the
    // text -- and on the debrief page the same control is `genericGui_done`.
    const std::string go = m_brief.debrief ? m_ui->Strings().Get("genericGui_done")
                                           : m_ui->Strings().Get("pipboy_startMission");
    const int32_t gw = st.normal.Measure(go);
    const int32_t gx = ox + 275 + ((688 - 275) - gw) / 2, gy = oy + 421;
    st.normal.Draw(c, gx, gy, go, st.consoleGreen);
    c->Fill(gx, gy + st.normal.Height() + 1, gx + gw, gy + st.normal.Height() + 2,
            st.consoleGreen);
    m_hot.push_back(Hot{{gx - 8, gy - 4, gx + gw + 8, gy + st.normal.Height() + 6},
                        kCmdStartMission});

    if (!m_status.empty()) {
        st.lite.DrawAligned(c, ox, oy + 500, ox + 800, oy + 516, m_status, st.windowText, 2);
    }
}

// ---------------------------------------------------------------------------
// The world map.
// ---------------------------------------------------------------------------
void CampaignUi::DrawWorldMap(Canvas* c, int32_t W, int32_t H, const CampaignState& s) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);

    // The viewport: the whole window minus a 96 px status strip at the bottom.
    const int32_t vx0 = 8, vy0 = 8, vx1 = W - 8, vy1 = H - 96;
    m_view[0] = vx0;
    m_view[1] = vy0;
    m_view[2] = vx1;
    m_view[3] = vy1;
    const int32_t vw = vx1 - vx0, vh = vy1 - vy0;

    // The world picture is the campaign's own 65 x 34 grid of 75 px `<zar>`
    // tiles, drawn 1:1 -- `WWorldWindow::RenderWorldView @006bbb00` blits one
    // cached icon per tile and blacks out any whose fog word is 0
    // (l.23925 / l.24116).
    const CampaignFile& f = s.file;
    int32_t ox = static_cast<int32_t>(s.posX) - vw / 2;
    int32_t oy = static_cast<int32_t>(s.posY) - vh / 2;
    ox = std::max(0, std::min(ox, std::max(0, f.PixelW() - vw)));
    oy = std::max(0, std::min(oy, std::max(0, f.PixelH() - vh)));
    m_mapOx = ox;
    m_mapOy = oy;

    c->SetClip(vx0, vy0, vx1, vy1);
    c->Fill(vx0, vy0, vx1, vy1, 0x000000);
    if (f.tilePx > 0) {
        const int32_t tx0 = ox / f.tilePx, ty0 = oy / f.tilePx;
        const int32_t tx1 = std::min(f.gridW - 1, (ox + vw) / f.tilePx);
        const int32_t ty1 = std::min(f.gridH - 1, (oy + vh) / f.tilePx);
        for (int32_t ty = ty0; ty <= ty1; ++ty) {
            for (int32_t tx = tx0; tx <= tx1; ++tx) {
                const int32_t px = vx0 + tx * f.tilePx - ox;
                const int32_t py = vy0 + ty * f.tilePx - oy;
                const CampaignTile& t = f.tiles[static_cast<size_t>(tx) * f.gridH + ty];
                if (t.fog == 0) {
                    c->Fill(px, py, px + f.tilePx, py + f.tilePx, 0x000000);
                    continue;
                }
                const std::vector<uint8_t>* art = TileArt(f, tx, ty);
                if (art == nullptr) {
                    c->Fill(px, py, px + f.tilePx, py + f.tilePx, 0x182818);
                    continue;
                }
                const int32_t aw = t.artW, ah = t.artH;
                for (int32_t y = 0; y < ah; ++y) {
                    for (int32_t x = 0; x < aw; ++x) {
                        const uint8_t* q = art->data() + (static_cast<size_t>(y) * aw + x) * 4;
                        if (q[3] == 0) continue;
                        c->Pixel(px + x, py + y,
                                 (static_cast<uint32_t>(q[0]) << 16) |
                                     (static_cast<uint32_t>(q[1]) << 8) | q[2],
                                 255);
                    }
                }
            }
        }
    }

    auto toScreen = [&](double px, double py, int32_t* outX, int32_t* outY) {
        *outX = vx0 + static_cast<int32_t>(px) - ox;
        *outY = vy0 + static_cast<int32_t>(py) - oy;
    };

    // The mission markers.  A `Covered` location is not drawn at all, which is
    // what the state name means; `UnCovered` is a live objective and `Visited`
    // one you have already been to.
    const Art* locSmall = m_ui->Resources()->Image("gui/misc/worldm/location10.zar");
    const Art* locBig = m_ui->Resources()->Image("gui/misc/worldm/location20.zar");
    for (size_t i = 0; i < s.cores.size(); ++i) {
        const CoreMission& m = s.cores[i];
        if (m.state == MissionState::Covered) continue;
        int32_t px, py;
        toScreen(m.locX, m.locY, &px, &py);
        if (px < vx0 - 40 || px > vx1 + 40 || py < vy0 - 40 || py > vy1 + 40) continue;
        const Art* a = m.isBunker ? locBig : locSmall;
        if (a->ok) {
            c->Blit(*a, px - a->w / 2, py - a->h / 2);
        } else {
            const uint32_t col = m.isBunker ? 0x00C0FF : 0xFFC000;
            c->Fill(px - 4, py - 4, px + 4, py + 4, col);
        }
        st.lite.Draw(c, px + 8, py - st.lite.Height() / 2, m_ui->Strings().Get(m.nameKey),
                     m.state == MissionState::Visited ? st.windowText : st.selectionFill);
        Hot hot;
        hot.r[0] = px - 12;
        hot.r[1] = py - 12;
        hot.r[2] = px + 12;
        hot.r[3] = py + 12;
        hot.id = kCmdGotoCore + static_cast<int>(i);
        m_hot.push_back(hot);
    }

    // The destination and the squad marker.
    if (s.travelling) {
        int32_t dx, dy;
        toScreen(s.destX, s.destY, &dx, &dy);
        const Art* d = m_ui->Resources()->Image("gui/misc/worldm/dest.zar");
        if (d->ok) c->Blit(*d, dx - d->w / 2, dy - d->h / 2);
        else c->Fill(dx - 3, dy - 3, dx + 3, dy + 3, 0x00FF00);
    }
    {
        int32_t px, py;
        toScreen(s.posX, s.posY, &px, &py);
        const Art* pos = m_ui->Resources()->Image(
            s.travelling ? "gui/misc/world_map_moving.zar" : "gui/misc/world_map_pos.zar");
        if (pos->ok) {
            c->Blit(*pos, px - pos->w / 2, py - pos->h / 2);
        } else {
            c->Fill(px - 5, py - 5, px + 5, py + 5, 0xFF0000);
        }
        m_squadScreen[0] = px;
        m_squadScreen[1] = py;
    }
    if (!m_encounterAt.empty()) {
        const Art* e = m_ui->Resources()->Image("gui/misc/worldm/encounter.zar");
        int32_t px, py;
        toScreen(m_encounterX, m_encounterY, &px, &py);
        if (e->ok) c->Blit(*e, px - e->w / 2, py - e->h / 2);
        else c->Fill(px - 6, py - 6, px + 6, py + 6, 0xFF0000);
    }
    c->SetClip(0, 0, W, H);

    // The status strip.
    c->Fill(0, vy1, W, H, st.windowFill);
    c->Fill(0, vy1, W, vy1 + 1, st.panelLight);
    const int64_t totalMin = s.timeMs / 60000;
    char clock[128];
    std::snprintf(clock, sizeof(clock), "Day %lld  %02lld:%02lld",
                  static_cast<long long>(totalMin / 1440 + 1),
                  static_cast<long long>((totalMin / 60) % 24),
                  static_cast<long long>(totalMin % 60));
    st.medium.Draw(c, 12, vy1 + 8, clock, st.bosText);
    char where[192];
    std::snprintf(where, sizeof(where), "(%.0f, %.0f)   squad %d   level %d",
                  s.posX, s.posY, static_cast<int>(s.DeployedPaths().size()), s.SquadLevel());
    st.lite.Draw(c, 12, vy1 + 30, where, st.windowText);
    st.lite.Draw(c, 12, vy1 + 48, m_status, st.selectionFill);
    {
        char t[256];
        std::snprintf(t, sizeof(t),
                      "map %dx%d px  view (%d,%d)  squad screen (%d,%d)  terrain %.2f",
                      s.file.PixelW(), s.file.PixelH(), m_mapOx, m_mapOy, m_squadScreen[0],
                      m_squadScreen[1], s.file.terrain.Speed(s.posX, s.posY));
        m_trace = t;
    }

    Button(c, W - 340, vy1 + 12, 100, 24, "Save", kCmdSaveGame);
    Button(c, W - 232, vy1 + 12, 100, 24, "Load", kCmdLoadGame);
    Button(c, W - 124, vy1 + 12, 100, 24, "Menu", kCmdMainMenu);
}

// ---------------------------------------------------------------------------
// The bunker's recruit screen.  `WRecruit::PopulateRecruitList @004d59a0` is
// the list builder and `WRecruit::UpdateRecruitButtonStates @004d4f40` the
// enable/disable rule: the two buttons are "take this one" and "drop that
// one", and the cap is the squad size.  Six is what the game bar has room for
// (`WGameBar::LayoutGameBarControls @0047f420` builds six squad tabs) and what
// `SaveGameHeader` carries six portraits for.
// ---------------------------------------------------------------------------
void CampaignUi::DrawRecruit(Canvas* c, int32_t W, int32_t H, const CampaignState& s) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    const Art* back = m_ui->Resources()->Image("gui/back/RecruitSingle.zar");
    const int32_t bw = back->ok ? back->w : 800, bh = back->ok ? back->h : 600;
    const int32_t x0 = (W - bw) / 2, y0 = (H - bh) / 2;
    if (back->ok) c->Blit(*back, x0, y0);
    else c->Fill(x0, y0, x0 + bw, y0 + bh, st.windowFill);

    // The three inset panels the shipped art has, in its own 800 x 600 space:
    // the wide list on the left, the narrow list in the middle, and the 2 x 3
    // portrait grid on the right, which is exactly the six squad slots
    // `WGameBar::LayoutGameBarControls @0047f420` builds tabs for and the six
    // portraits `SaveGameHeader` carries.
    const int32_t lx = x0 + 34, ly = y0 + 66, lw = 350, lh = 386;
    const int32_t mx = x0 + 428, my = y0 + 200, mw = 158, mh = 250;
    const int32_t px = x0 + 620, py = y0 + 62, pw = 178, ph = 396;

    st.large.DrawAligned(c, x0, y0 + 14, x0 + bw, y0 + 40, "RECRUITS", st.bosText, 2);
    st.lite.Draw(c, lx, ly - 14, "SQUAD", st.windowText);
    st.lite.Draw(c, mx, my - 14, "AVAILABLE", st.windowText);

    auto shortName = [](const std::string& p2) {
        std::string n = p2;
        const size_t sl = n.find_last_of('/');
        if (sl != std::string::npos) n = n.substr(sl + 1);
        const size_t d = n.find_last_of('.');
        if (d != std::string::npos) n = n.substr(0, d);
        return n;
    };
    // Milestone 9 established the portrait key: `gui/char/<Display Name>.zar`,
    // and every shipped recruit's Display Name is "CORE_rec_<file stem>".
    auto portraitKey = [&](const RosterMember& m) {
        return m.displayName.empty() ? ("CORE_rec_" + shortName(m.entPath)) : m.displayName;
    };

    int32_t row = 0, inSquad = 0;
    for (const RosterMember& m : s.roster) inSquad += (m.inSquad && !m.dead) ? 1 : 0;
    for (size_t i = 0; i < s.roster.size(); ++i) {
        const RosterMember& m = s.roster[i];
        if (!m.inSquad) continue;
        const int32_t y = ly + row * kRowH;
        if (y + kRowH > ly + lh) break;
        char line[192];
        std::snprintf(line, sizeof(line), "%-10s  %s", shortName(m.entPath).c_str(),
                      m.dead ? "KIA"
                             : (m.maxHp > 0 ? (std::to_string(m.hp) + "/" +
                                               std::to_string(m.maxHp) + " hp   " +
                                               std::to_string(m.xp) + " xp   L" +
                                               std::to_string(m.level))
                                            : std::string("ready")).c_str());
        st.lite.Draw(c, lx + 6, y + 3, line, m.dead ? st.textTBD : st.buttonText);
        Hot hot{{lx, y, lx + lw, y + kRowH}, kCmdDropRecruit + static_cast<int>(i)};
        m_hot.push_back(hot);
        ++row;
    }
    row = 0;
    for (size_t i = 0; i < s.pool.size(); ++i) {
        const int32_t y = my + row * kRowH;
        if (y + kRowH > my + mh) break;
        st.lite.Draw(c, mx + 6, y + 3, shortName(s.pool[i]),
                     inSquad >= 6 ? st.panelLight : st.windowText);
        Hot hot{{mx, y, mx + mw, y + kRowH}, kCmdTakeRecruit + static_cast<int>(i)};
        m_hot.push_back(hot);
        ++row;
    }

    // The six portrait cells.
    int32_t slot = 0;
    for (const RosterMember& m : s.roster) {
        if (!m.inSquad || slot >= 6) continue;
        const int32_t cx = px + (slot % 2) * (pw / 2);
        const int32_t cy = py + (slot / 2) * (ph / 3);
        const Art* a = m_ui->Resources()->Image("gui/char/" + portraitKey(m) + ".zar");
        if (a->ok) {
            c->Blit(*a, cx + (pw / 2 - a->w) / 2, cy + (ph / 3 - a->h) / 2);
        } else {
            st.lite.DrawAligned(c, cx, cy + ph / 6, cx + pw / 2, cy + ph / 3,
                                shortName(m.entPath), st.windowText, 2);
        }
        if (m.dead) {
            st.medium.DrawAligned(c, cx, cy + ph / 6, cx + pw / 2, cy + ph / 3, "KIA",
                                  st.textTBD, 2);
        }
        ++slot;
    }

    char line[160];
    std::snprintf(line, sizeof(line), "%d of 6 deployed    %zu in the pool", inSquad,
                  s.pool.size());
    st.medium.Draw(c, x0 + 470, y0 + bh - 96, line, st.bosText);
    st.lite.Draw(c, x0 + 470, y0 + bh - 74, m_status.substr(0, 46), st.selectionFill);
    Button(c, x0 + 470, y0 + bh - 46, 100, 22, "Quarterm.", kCmdQuartermaster);
    Button(c, x0 + 578, y0 + bh - 46, 100, 22, "World Map", kCmdWorldMap);
    Button(c, x0 + 686, y0 + bh - 46, 100, 22, "Save", kCmdSaveGame);
}

// ---------------------------------------------------------------------------
// The save/load browser.  `WSaveGameItem::EnsureSaveGameHeaderLoaded
// @004bf560` reads a slot's `<saveh>` block straight from offset 0 without
// loading the save, which is what fills each row here.
// ---------------------------------------------------------------------------
void CampaignUi::DrawSaveLoad(Canvas* c, int32_t W, int32_t H, bool saving) {
    const Style& st = m_ui->Styles();
    m_hot.clear();
    c->Fill(0, 0, W, H, 0x000000);
    const Art* back = m_ui->Resources()->Image("gui/back/saveload.zar");
    const int32_t bw = back->ok ? back->w : 800, bh = back->ok ? back->h : 600;
    const int32_t x0 = (W - bw) / 2, y0 = (H - bh) / 2;
    if (back->ok) c->Blit(*back, x0, y0);
    else c->Fill(x0, y0, x0 + bw, y0 + bh, st.windowFill);

    // The art's own two panels: the slot list on the left, and the note pad on
    // the right where the selected slot's detail goes.
    const int32_t lx = x0 + 30, ly = y0 + 62, lw = 356, lh = 480;
    const int32_t nx = x0 + 448, ny = y0 + 60;

    st.large.DrawAligned(c, x0, y0 + 16, x0 + bw, y0 + 44, saving ? "SAVE GAME" : "LOAD GAME",
                         st.bosText, 2);
    for (size_t i = 0; i < m_slots.size(); ++i) {
        const int32_t y = ly + static_cast<int32_t>(i) * (kRowH + 2);
        if (y + kRowH > ly + lh) break;
        const bool sel = static_cast<int>(i) == m_slotSel;
        if (sel) c->Fill(lx, y, lx + lw, y + kRowH, st.selectionFill, 90);
        const SaveSlot& sl = m_slots[i];
        st.lite.Draw(c, lx + 8, y + 3, sl.file, sel ? st.selectionText : st.windowText);
        st.lite.Draw(c, lx + 210, y + 3, sl.header.name, sel ? st.selectionText : st.buttonText);
        Hot hot{{lx, y, lx + lw, y + kRowH}, kCmdSlot + static_cast<int>(i)};
        m_hot.push_back(hot);
    }
    // The selected slot's `<saveh>` fields, which
    // `WSaveGameItem::EnsureSaveGameHeaderLoaded @004bf560` reads without
    // loading the save.
    if (m_slotSel >= 0 && m_slotSel < static_cast<int>(m_slots.size())) {
        const SaveGameHeader& h = m_slots[static_cast<size_t>(m_slotSel)].header;
        // MILESTONE 24: the three rows the shipped game draws are the three
        // strings the header now carries -- character, mission display name,
        // date and time -- not a path and a millisecond count.
        st.medium.Draw(c, nx, ny + 24, h.playerName.empty() ? h.name : h.playerName, 0x201808);
        st.lite.Draw(c, nx, ny + 52,
                     h.missionName.empty() ? std::string("world map") : h.missionName, 0x201808);
        st.lite.Draw(c, nx, ny + 74, h.timestamp, 0x201808);
    }
    st.lite.Draw(c, lx, y0 + bh - 42, m_status.substr(0, 44), st.selectionFill);
    Button(c, x0 + 578, y0 + bh - 46, 100, 22, saving ? "Save" : "Load",
           saving ? kCmdDoSave : kCmdDoLoad);
    Button(c, x0 + 686, y0 + bh - 46, 100, 22, "Back", kCmdBack);
}

std::string CampaignUi::Trace() const { return m_trace; }

const std::vector<uint8_t>* CampaignUi::TileArt(const CampaignFile& f, int32_t tx, int32_t ty) {
    const uint32_t key = static_cast<uint32_t>(tx) * 4096u + static_cast<uint32_t>(ty);
    auto it = m_tileCache.find(key);
    if (it != m_tileCache.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<uint8_t> rgba;
    if (!f.TileArt(tx, ty, &rgba)) rgba.clear();
    auto& slot = m_tileCache[key];
    slot = std::move(rgba);
    return slot.empty() ? nullptr : &slot;
}

int CampaignUi::HitTest(int32_t x, int32_t y) const {
    for (size_t i = m_hot.size(); i-- > 0;) {
        if (In(x, y, m_hot[i].r)) return m_hot[i].id;
    }
    return -1;
}

bool CampaignUi::MapPointAt(int32_t x, int32_t y, double* px, double* py) const {
    if (x < m_view[0] || x >= m_view[2] || y < m_view[1] || y >= m_view[3]) return false;
    *px = x - m_view[0] + m_mapOx;
    *py = y - m_view[1] + m_mapOy;
    return true;
}

}  // namespace game
