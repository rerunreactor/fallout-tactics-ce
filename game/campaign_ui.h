// campaign_ui.h -- milestone 12's campaign screens.  See campaign_ui.cpp.

#ifndef GAME_CAMPAIGN_UI_H
#define GAME_CAMPAIGN_UI_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "campaign.h"
#include "combat.h"

namespace game {

class Ui;
struct Canvas;
struct Art;

struct SaveSlot {
    std::string path, file;
    SaveGameHeader header;
};

// MILESTONE 20: one pre-made character, as the CHARACTER SELECTION screen
// shows it.  The five of them are `campaigns/bos.cam`'s own `prefabs` vector
// (Campaign+0x1bd, campaign.cpp) -- `entities/special/prefab/prefab1..5.ent`
// -- and every field below is read out of that `.ent` or out of
// `locale/game/characters.txt` under the `.ent`'s own Display Name.
struct PrefabChar {
    std::string entPath;
    std::string displayName;    // "CORE_prefab1"
    std::string name;           // name_CORE_prefab1  = {Mick}
    std::string story;          // desc_CORE_prefab1  = {Mick is not ...}
    Attributes attr;
    int32_t hp = 0, maxHp = 0;
    bool ok = false;
};

// MILESTONE 20: what the briefing / debrief Pip-Boy page is showing.  Filled by
// main.cpp, which owns the mission locale table; the five tabs are
// `pipboy_briefings` / `_dialogue` / `_map` / `_archives` / `_off`.
struct BriefingData {
    bool debrief = false;             // the after-mission variant
    std::string missionKey;           // "mission01", for the title line
    std::string title;
    std::string brief;                // MISSION_BRIEF, or the joined debrief text
    std::vector<std::string> dialogue;
    std::vector<std::string> archives;
    std::vector<std::string> map;
    std::string misPath;              // what START MISSION enters
    int32_t core = -1;
};

// Which campaign-layer screen is up.  The in-mission screens stay `Ui`'s.
//
// MILESTONE 20.  `TitleMenu` is `WMainMenu` (the executable's own top-level
// menu, @006c00f0); `MainMenu` is the SINGLE PLAYER submenu the capture opens
// on; `Tutorials` and `Custom` are the two list submenus; `CharSelect` is the
// pre-made character browser; `Briefing` is the Pip-Boy page shown before a
// mission and again, in its debrief form, after one.
enum class CampaignScreen { None, TitleMenu, MainMenu, Tutorials, Custom, CharSelect,
                            Briefing, WorldMap, Recruit, SaveGame, LoadGame,
                            // MILESTONE 21 -- the two full-screen JPEG pages the
                            // decoder unblocked (VIDEO_GAPS.md L2 and L4).
                            Loading, EndScreen };

class CampaignUi {
public:
    explicit CampaignUi(Ui* ui);

    // Command ids the hit test returns.
    enum {
        kCmdNone = -1,
        kCmdNewGame = 1,
        kCmdLoadGame,
        kCmdSaveGame,
        kCmdQuit,
        kCmdMainMenu,
        kCmdWorldMap,
        kCmdQuartermaster,
        kCmdBack,
        kCmdDoSave,
        kCmdDoLoad,
        kCmdEnterMission,
        kCmdGotoCore = 1000,      // + core index
        kCmdTakeRecruit = 2000,   // + pool index
        kCmdDropRecruit = 3000,   // + roster index
        kCmdSlot = 4000,          // + slot index

        // MILESTONE 20 -- the front end.
        kCmdIntro = 20,
        kCmdSinglePlayer,
        kCmdMultiplayer,
        kCmdOptions,
        kCmdCredits,
        kCmdExit,
        kCmdTutorials,
        kCmdCustomCampaign,
        kCmdCustomMission,
        kCmdCharPrev,
        kCmdCharNext,
        kCmdCharTake,
        kCmdCharModify,
        kCmdCharCreate,
        kCmdStartMission,
        kCmdPipTab = 5000,        // + tab index 0..4
        kCmdListPick = 6000,      // + list row
    };

    // The 800 x 600 surface every front-end window is laid out in, centred in
    // the window -- which is what the capture shows (VIDEO_GAPS.md: the
    // front-end screens are an 800 x 600 image letterboxed on black).
    static void Origin(int32_t W, int32_t H, int32_t* ox, int32_t* oy) {
        *ox = (W - 800) / 2;
        *oy = (H - 600) / 2;
    }

    // MILESTONE 21: the loading screen (`gui/wait/wait%d.jpg`, the exe's own
    // format string) and the mission-end art (`gui/back/DEATHscreen.jpg` and
    // its four siblings).  Both are plain 800 x 600 JPEGs blitted at the
    // window origin; the loading screen adds the progress bar measured off
    // f0050.  See campaign_ui.cpp for the provenance of every number.
    void DrawLoading(Canvas* c, int32_t W, int32_t H);
    void DrawEndScreen(Canvas* c, int32_t W, int32_t H);
    // `image` is a resource path; `progress` is 0..1.
    void SetLoading(const std::string& image, float progress) {
        m_loadingImage = image;
        m_loadingProgress = progress;
    }
    void SetEndScreen(const std::string& image) { m_endImage = image; }
    const std::string& LoadingImage() const { return m_loadingImage; }
    const std::string& EndImage() const { return m_endImage; }

    // MILESTONE 20.
    void DrawTitleMenu(Canvas* c, int32_t W, int32_t H);
    void DrawMainMenu(Canvas* c, int32_t W, int32_t H);
    void DrawListMenu(Canvas* c, int32_t W, int32_t H);
    void DrawCharSelect(Canvas* c, int32_t W, int32_t H);
    void DrawBriefing(Canvas* c, int32_t W, int32_t H);
    void DrawWorldMap(Canvas* c, int32_t W, int32_t H, const CampaignState& s);
    void DrawRecruit(Canvas* c, int32_t W, int32_t H, const CampaignState& s);
    void DrawSaveLoad(Canvas* c, int32_t W, int32_t H, bool saving);

    void SetMouse(int32_t x, int32_t y) { m_mx = x; m_my = y; }
    int HitTest(int32_t x, int32_t y) const;
    // Turns a click inside the world-map viewport into campaign pixels.
    bool MapPointAt(int32_t x, int32_t y, double* px, double* py) const;

    void SetStatus(const std::string& s) { m_status = s; }
    // MILESTONE 15: every save/load failure reason lands here, and the browser
    // lane has no other way to see one. Read by `rr_campaign_state`.
    const std::string& Status() const { return m_status; }
    // MILESTONE 20 state.
    std::vector<PrefabChar> m_chars;
    int32_t m_charSel = 0;
    // The tutorial / custom-campaign / custom-mission pickers: (caption, path).
    std::vector<std::pair<std::string, std::string>> m_list;
    std::string m_listTitle;
    BriefingData m_brief;
    int32_t m_pipTab = 0;
    int32_t m_briefScroll = 0;
    int32_t m_briefLines = 0, m_briefPage = 0;
    void ScrollBriefing(int32_t d);
    const PrefabChar* SelectedChar() const {
        return m_chars.empty() ? nullptr
                               : &m_chars[static_cast<size_t>(m_charSel) % m_chars.size()];
    }

    std::vector<SaveSlot> m_slots;
    int m_slotSel = 0;
    std::string m_status;
    // The encounter marker `WWorldWindow__ProcessWorldMapTick` blinks while
    // its eight-tick countdown runs.
    std::string m_encounterAt;
    double m_encounterX = 0, m_encounterY = 0;
    int32_t m_countdown = 0;

    // What the last DrawWorldMap laid out, for the verification harness.
    std::string Trace() const;

    // MILESTONE 15.  The browser lane cannot see inside the canvas: the shell
    // owns a WebGL surface and has no way to ask "where is the New Game
    // button".  `rr_campaign_state()` (main.cpp) answers that, and this is what
    // it reads -- the same hot-rect list `HitTest` consults, so a caller that
    // clicks the centre of a rect from here hits exactly what a player would.
    // It is the campaign-UI analogue of the Fallout lanes' `rr_resize_*`
    // exports, and like them it is an inspection surface, not a control one.
    struct Hot {
        int32_t r[4];
        int id;
    };
    const std::vector<Hot>& Hots() const { return m_hot; }
    void SquadScreen(int32_t* x, int32_t* y) const {
        *x = m_squadScreen[0];
        *y = m_squadScreen[1];
    }

private:
    void Button(Canvas* c, int32_t x, int32_t y, int32_t w, int32_t h, const std::string& label,
                int id);
    // MILESTONE 20: one `gui/btn/big` plate -- 240 x 40, the label in
    // `kBosLabelBig`'s inset, left aligned, which is where the capture puts it.
    void BigButton(Canvas* c, int32_t x, int32_t y, const std::string& label, int id);
    // The body text of the current Pip-Boy tab, already wrapped.
    std::vector<std::string> BriefTabLines(int32_t tab, int32_t width) const;
    const std::vector<uint8_t>* TileArt(const CampaignFile& f, int32_t tx, int32_t ty);

    Ui* m_ui = nullptr;
    // MILESTONE 21.
    std::string m_loadingImage = "gui/wait/wait1.jpg";
    float m_loadingProgress = 0.0f;
    std::string m_endImage = "gui/back/DEATHscreen.JPG";
    std::vector<Hot> m_hot;
    int32_t m_mx = -1, m_my = -1;
    int32_t m_view[4] = {0, 0, 0, 0};
    int32_t m_mapOx = 0, m_mapOy = 0;
    int32_t m_squadScreen[2] = {0, 0};
    std::map<uint32_t, std::vector<uint8_t>> m_tileCache;
    std::string m_trace;
};

}  // namespace game

#endif  // GAME_CAMPAIGN_UI_H
