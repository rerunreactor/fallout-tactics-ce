// light.h -- milestone 5: the scene lighting, ported.
//
// Sources, all Ghidra output from the retail binary unless a raw disassembly is
// named (those were taken from the shipped BOS.exe with capstone, because the
// two functions concerned are not in any supplied decompilation):
//
//   Light::InitDefaultEntityFields  @0059bee0  `Light::InitDefaultEntityFields@0059bee0`
//   Light::EntityStreamStruct::Serialize @0059c1c0  -- the property NAME and
//       COMPILE-TIME DEFAULT table; disassembled, not decompiled.
//   Light::Vtable521...            @0059c430  l.1610  -- the read-back path and
//       the two derived fields at Light+0x211 (radius^2) and +0x215 (1/radius^2).
//   Light::DoEngineThink           @0059cb40  -- the flicker clock.
//   Light::GetRenderLightLevel     @0059cc50  l.1986  -- vtable slot 0, the
//       function the RENDER path calls.
//   Light::GetLightEffectAtLocation@0059cfe0  l.2113  -- vtable slot 525
//       (+0x834), the FOG path.
//   RefreshEntityVisionAndLighting @006e3260  l.2321  -- the per-element
//       accumulation, both of it (see NOTES.md section 25).
//   SoftwareRender::RenderLitObject@00783340  l.4226  -- the four-corner
//       average that becomes the ambient of ApplyPaletteColorAdjustments.
//   ComputeTimeOfDayLightingState  @004f2770  l.997   -- the day/night flags.
//   DateTime::GetMinutesToSunrise  @006ad7b0, GetMinutesToSunset @006ad820,
//   IsDaylight @006ad770, IsNight @006ad7a0, IsNearSunrise @006ad8c0,
//   IsNearSunset @006ad8a0, ConvertMsecToDaysHMS @006ad6a0,
//   NormalizeTimeForYearsAndMonths @006ada90,
//   ConvertDaysToYearsMonthsDays   @006adbc0 -- all disassembled.

#ifndef MAPVIEW_LIGHT_H
#define MAPVIEW_LIGHT_H

#include <cstdint>
#include <string>
#include <vector>

namespace ft {

// FOTTimeStruct as ConvertMsecToDaysHMS @006ad6a0 fills it (+0 qword days, +8
// hour, +0xa minute, +0xc second, +0xe ms) and NormalizeTimeForYearsAndMonths
// @006ada90 finishes it (+0x10 year, +0x12 month).
struct WorldTime {
    int64_t days = 0;
    int hour = 0, minute = 0, second = 0, milli = 0;
    int year = 0, month = 0;

    // The raw day count, before NormalizeTimeForYearsAndMonths folds the
    // +6h29m in.  `(short)local_14.days + _DAT_0082cba0` at 004f27ce is
    // `add edi, dword ptr [0x82cba0]` with 0x00060000 stored there, i.e. +0 in
    // the ushort the moon-phase modulo then uses.
    int64_t rawDays = 0;

    // ComputeTimeOfDayLightingState @004f2770's flag word (World+0x43f), now
    // filled in FULL.  SumLightContributionsAtPoint / GatherVisibleLights test
    // only 0x61; the lighting branch tests all of it.
    //   0x0001  night           IsNight       @006ad7a0
    //   0x001e  moon phase 0-4  ((phase & 0xf) << 1)
    //   0x0020  near sunrise    IsNearSunrise @006ad8c0  (< 25 min after)
    //   0x0040  near sunset     IsNearSunset  @006ad8a0  (< 45 min before)
    //   0x0080  storm           the weekday-5/6 arm; scales the colour by 0.7
    //   0x0100  overcast        the weekday-3 arm
    //   0x0200  weekday 1 late / weekday 2 early
    uint16_t flags = 0;
    bool Dark() const { return (flags & 0x61) != 0; }
    int MoonPhase() const { return (flags >> 1) & 0xf; }

    // Day length in hours: 12.5 when year <= 0, else 16 - |6 - month|
    // (GetMinutesToSunrise @006ad7b0, first block).
    float DayLength() const;
    float SunriseHour() const;   // (24 - dayLen)/2 - 0.5
    float SunsetHour() const;    // (24 + dayLen)/2

    static WorldTime FromMillis(int64_t ms);          // the World clock, ported
    static WorldTime FromHourMinute(int h, int m);    // --time HH:MM
    std::string Describe() const;
};

// ComputeTimeOfDayLightingState @004f2770's second half, ported whole.
// `diffuse` is its third parameter -- the colour World::SetOrientationIfChanged
// @006e5b20 writes into the rig at World+0x69 -- and `ambient` its fourth,
// `f * diffuse`, which World::SetPositionIfChanged @006e5ad0 writes at
// World+0x5D.  `World::UpdateWorldTimeAndLighting @004f2c10` performs BOTH
// writes every frame and `UpdateEntityRenderTransform @006e1c10` reads exactly
// those two fields, so this -- not the `.mis`'s stored <mapManager> triple --
// is the rig the renderer sees.  The stored triple is the EDITOR's snapshot of
// this same computation.  See NOTES.md 25.13.
void ComputeTimeOfDayLighting(const WorldTime& t, float diffuse[3], float ambient[3]);

// Where the rig comes from -- `--light-base`.
enum class LightBase {
    kClock,   // ComputeTimeOfDayLighting: what the shipped game does
    kMap,     // the `.mis`'s stored <mapManager> triple: the editor's snapshot
    kWhite,   // UpdateEntityRenderTransform's literal DAT_008bda50 == 0 branch
};

// One Light entity, with the fields Light::Serialize registers and the two
// derived ones Light::Vtable521 computes at the end of every load.
struct SceneLight {
    float pos[3] = {0.0f, 0.0f, 0.0f};      // Entity+0x9A, in metres
    float dir[3] = {0.0f, 0.0f, 1.0f};      // EntityMembers.pad2d[3..5], the arc axis

    // Registered properties, with Light::Serialize's compile-time defaults.
    bool on = true;                 // "On"                    +0x1D5  default 1
    bool nightOnly = false;         // "Night Only"            +0x1D6  default 0
    float radius = 2.0f;            // "Radius (meters)"       +0x1D7  default 2.0
    int32_t rateChange = 0;         // "Rate of change (ms)"   +0x1DB  default 0
    float color1[3] = {1, 1, 1};    // "Color1"                +0x1DF  default white
    std::string intensity;          // "Intensity string (a-z)"+0x1EB  default ""
    bool repeating = true;          // "Repeating light intenstity" +0x1EF default 1
    int32_t intensityPos = 0;       //                         +0x219  default 0
    bool useColor2 = false;         // "Use Color2"            +0x1F0  default 0
    float color2[3] = {1, 1, 1};    // "Color2"                +0x1F1  default white
    bool useArc = false;            // "Use Arc"               +0x1FD  default 0
    int32_t arc = 360;              // "Arc (degrees)"         +0x1FE  default 0x168
    float highBound = 0.0f;         // "High Bound"            +0x202  default 0
    float lowBound = 0.0f;          // "Low Bound"             +0x206  default 0
    bool useFog = false;            // "Use Fog"               +0x20A  default 0
    float fogAlpha = 0.0f;          // "Fog Alpha"             +0x20B  default 0

    // Light+0x211 and +0x215, recomputed after every load.
    float radius2 = 4.0f;
    float invRadius2 = 0.25f;
    void Derive();

    // Which of the two lists World::GatherVisibleLights @004f2000 puts it in:
    // useFog picks the second (M+0xE9), everything else the first (M+0xD9).
    bool InRenderList() const { return !useFog; }

    // Light::GetRenderLightLevel @0059cc50.  `dist2` is |p - pos|^2.
    // Returns false when the light contributes nothing.
    bool GetRenderLightLevel(const float p[3], float dist2, float out[3]) const;

    // Light::GetLightEffectAtLocation @0059cfe0 -- the fog list's function.
    bool GetLightEffectAtLocation(const float p[3], float dist2, float rgba[4]) const;

    // SumLightContributionsAtPoint's gate, which GatherVisibleLights repeats:
    // `on || (nightOnly && (World+0x43f & 0x61))`.
    bool Active(const WorldTime& t) const { return on || (nightOnly && t.Dark()); }

    // Light::DoEngineThink @0059cb40: one step of the flicker clock.
    void AdvanceTicks(int64_t ticks);

    std::string entPath;   // for --selftest reporting only
};

// The render manager's lighting inputs, M+0xA0..M+0xB4 as
// UpdateEntityRenderTransform @006e1c10 fills them each frame.
struct SceneLighting {
    bool enabled = true;
    // M+0xA0: `DAT_008bda50 ? World+0x5D : light_level_maybe`.  DAT_008bda50 is
    // an EDITOR MENU TOGGLE (its only writer is FUN_0041c130, reached from
    // WEditorMap::BuildMapEditorEditMenu) and lives in .bss, so it is 0 in the
    // shipped game and the base is the constant white.  `--light-base map`
    // is the other branch.
    float base[3] = {1.0f, 1.0f, 1.0f};
    // M+0xAC = World+0x69, the <mapManager> diffuse triple.
    float diffuse[3] = {1.0f, 1.0f, 1.0f};
    bool addDiffuse = true;
    WorldTime time;
    std::vector<SceneLight> lights;

    // One shape vertex: base + sum of the lights that reach it + diffuse,
    // clamped per channel to [0, 1].  RefreshEntityVisionAndLighting
    // l.2556-2612 and l.2823-2860.
    void Vertex(const float p[3], float out[3]) const;

    // RenderLitObject l.4290-4297: average the four corners, then replace any
    // channel >= 2.0 with 2.0.  With the clamp above the average is already
    // <= 1, so the second step never fires; it is ported for exactness.
    void Ambient(const float corners[4][3], float A[3]) const;

    // Convenience: the four horizontal corners of a world-space box.
    void AmbientForBox(const float lo[3], const float hi[3], float A[3]) const;
    void AmbientForPoint(const float p[3], float A[3]) const;

    // The four shape vertices of BuildShapeVertices @006e4220's default case:
    // the TOP face of the element's box, y = hi[1], in the order
    // (hiX,hiZ) (hiX,loZ) (loX,loZ) (loX,hiZ).  `out` is indexed
    // [xIsMax][zIsMax] so a bilinear can address it directly.
    void CornersForBox(const float lo[3], const float hi[3], float out[2][2][3]) const;

    size_t ActiveCount() const;
};

}  // namespace ft

#endif  // MAPVIEW_LIGHT_H
