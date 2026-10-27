#include "light.h"

#include <cmath>
#include <cstdio>

namespace ft {
namespace {

// The float constants of the lighting path, all read straight out of the
// shipped BOS.exe's .rdata.
constexpr float kOne = 1.0f;            // _DAT_007fc564
constexpr float kHalf = 0.5f;           // _DAT_007fd93c
constexpr float kQuarter = 0.25f;       // _DAT_007fd598
constexpr float kAmbientCap = 2.0f;     // FLOAT_007fded8
constexpr float kIntensityStep = 0.04f; // _DAT_0080c194 == 1/25
constexpr float kRadToDeg = 180.0f * 0.31830987f;  // _DAT_00810d5c * _DAT_00810d60
constexpr float kMinusOne = -1.0f;      // _DAT_00809e54
constexpr float kDayShort = 12.5f;      // _DAT_0082cc2c
constexpr float kDayBase = 16.0f;       // _DAT_0082cc28
constexpr float kDayFull = 24.0f;       // _DAT_0082cc24

// ComputeTimeOfDayLightingState @004f2770's own constants, all read out of the
// shipped BOS.exe's .rdata.
constexpr float kByteToUnit = 1.0f / 255.0f;   // _DAT_007fd78c  0.00392157
constexpr float kFDay = 0.65f;                 // _DAT_00809550
constexpr float kFDayClear = 0.45f;            // _DAT_00809554
constexpr float kNightScaleStep = 0.0909091f;  // _DAT_00809558  == 1/11
constexpr float kPhaseScale = 0.285714f;       // _DAT_0080955c  == 2/7
constexpr float kFNightStep = 0.1f;            // _DAT_007ff0c8
constexpr float kStorm = 0.7f;                 // the literal 0.7 at 004f28f0

// The four packed 0x00RRGGBB day-phase colours.  `../lighting_followup.md` §3
// recorded these as UNRECOVERABLE because they live in .bss at
// 0x008bc4ec/f0/f4/500 and no decompiled *function* writes them -- but, exactly
// as with `light_level_maybe` in NOTES.md 24.2, the writes are C++ dynamic
// initialisers that Ghidra never turned into functions.  Disassembled straight
// out of BOS.exe; each is an incremental-link thunk into a four-byte store:
//
//   004f1ec0  movb $0x46,0x8bc4ec  $0x73,+1     $0x86,+2  $0xff,+3   -> sunset
//   004f1e90  movb $0x73,0x8bc4f0  al=$0x7c,+1/+2         $0xff,+3   -> sunrise
//   004f1ef0  movb $0x60,0x8bc4f4  $0x3c,+1     $0x37,+2  $0xff,+3   -> night
//   004f1f20  al=$0xfa -> 0x8bc500/1/2                    $0xff,+3   -> day
//
// The function reads them as (v>>16)&0xff = R, (v>>8)&0xff = G, v&0xff = B.
//
// Two of the four are confirmed INDEPENDENTLY by the shipped map data, which
// is what makes the whole reconstruction check out.  Ten maps (mission01,
// Brahmin Wood, among them) store the ambient (0.102941, 0.112299, 0.179679)
// with a diffuse exactly twice it; (55, 60, 96)/255 * (1 + 4/11) * 0.7 is that
// diffuse and f = 0.5 that ratio -- moon phase 4, a storm day -- to seven
// figures on all three channels.  Twenty-three maps store 0.446078, which is
// (250,250,250)/255 * 0.7 * 0.65, and two store 0.441176, which is the same
// colour * 1.0 * 0.45.  So the editor's snapshots are this function's output.
constexpr uint32_t kColSunset = 0x00867346;   // _DAT_008bc4ec  134, 115,  70
constexpr uint32_t kColSunrise = 0x007c7c73;  // _DAT_008bc4f0  124, 124, 115
constexpr uint32_t kColNight = 0x00373c60;    // _DAT_008bc4f4   55,  60,  96
constexpr uint32_t kColDay = 0x00fafafa;      // _DAT_008bc500  250, 250, 250

void UnpackColor(uint32_t v, float out[3]) {
    out[0] = static_cast<float>((v >> 16) & 0xff) * kByteToUnit;
    out[1] = static_cast<float>((v >> 8) & 0xff) * kByteToUnit;
    out[2] = static_cast<float>(v & 0xff) * kByteToUnit;
}

// MSVC's __ftol: truncation toward zero.
int32_t Ftol(double v) { return static_cast<int32_t>(v); }

// The per-month day table at 0x0082cbf0 that ConvertDaysToYearsMonthsDays
// walks.  Read out of the binary; it is the ordinary Gregorian common year.
const int32_t kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

}  // namespace

// ---------------------------------------------------------------------------
// The world clock
// ---------------------------------------------------------------------------

// ConvertMsecToDaysHMS @006ad6a0 then NormalizeTimeForYearsAndMonths @006ada90.
// The normaliser is not a no-op: it folds a fixed +29 minutes and +6 hours into
// the time (0059ada b7 `add ebp, 0x1d` and 0059adae4 `add edi, 6`) and adds
// 0x895 == 2197 to the year, so world time 0 is 06:29 on day 1 of 2197.
WorldTime WorldTime::FromMillis(int64_t ms) {
    WorldTime t;
    t.days = ms / 86400000LL;
    t.rawDays = t.days;
    int64_t rem = ms % 86400000LL;
    t.hour = static_cast<int>(rem / 3600000LL);
    rem %= 3600000LL;
    t.minute = static_cast<int>(rem / 60000LL);
    rem %= 60000LL;
    t.second = static_cast<int>(rem / 1000LL);
    t.milli = static_cast<int>(rem % 1000LL);

    // NormalizeTimeForYearsAndMonths
    int carry = t.second / 60;
    t.second %= 60;
    t.minute += carry + 29;
    carry = t.minute / 60;
    t.minute %= 60;
    t.hour += carry + 6;
    carry = t.hour / 24;
    t.hour %= 24;
    t.days += carry;
    // ConvertDaysToYearsMonthsDays @006adbc0
    int64_t d = t.days;
    t.year = static_cast<int>(d / 365);
    d %= 365;
    int m = 0;
    while (m < 12 && d >= kMonthDays[m]) {
        d -= kMonthDays[m];
        ++m;
    }
    t.days = d + 1;
    t.month = m + 1;
    t.year += 2197;

    // ComputeTimeOfDayLightingState @004f2770's flag word, in its own order.
    //
    // First the weather block (l.2297-2327): the "weekday" is
    // `((month * year) * days) % 7` over the day-of-month, and each residue
    // sets a different bit.  Residues 0 and 4 set nothing.
    const int64_t weekday =
        (static_cast<int64_t>(t.month) * static_cast<int64_t>(t.year) * t.days) % 7;
    if (weekday == 1) {
        if (t.hour > 0x14) t.flags |= 0x200;
    } else if (weekday == 2) {
        if (t.hour < 7) t.flags |= 0x200;
    } else if (weekday == 3) {
        t.flags |= 0x100;
    } else if (weekday >= 5) {
        // `hour % 3 == 0 || (hour & 1) == 0 || hour % 5 == 0`.  The middle term
        // is how MSVC compiles a signed `hour % 2 == 0`.
        if (t.hour % 3 == 0 || (t.hour & 1) == 0 || t.hour % 5 == 0) t.flags |= 0x80;
    }

    // Then the three the light gate reads.
    const int sinceSunrise = t.minute - Ftol(60.0 * (t.SunriseHour() - t.hour));
    const int toSunset = 60 - Ftol(60.0 * (t.hour + 0.5 - t.SunsetHour())) - t.minute;
    const bool daylight = sinceSunrise > 0 && toSunset > 0;   // IsDaylight @006ad770
    if (!daylight) {
        t.flags |= 1;                                          // IsNight @006ad7a0
    }
    if (sinceSunrise > 0 && sinceSunrise < 25) {               // @006ad8c0
        t.flags |= 0x20;
    } else if (toSunset > 0 && toSunset < 45) {                // @006ad8a0
        t.flags |= 0x40;
    }

    // Then the moon phase, bits 1..4 (l.2345-2357).  A 29-day cycle off the RAW
    // day count, offset half a cycle and folded; 2/7 maps the 0..14 distance
    // onto 0..4.
    {
        const uint16_t d = static_cast<uint16_t>(t.rawDays);
        const int base = (t.hour < 12) ? (d % 29) - 14 : ((d + 1) % 29) - 14;
        const int dist = base < 0 ? -base : base;
        const int phase = Ftol(static_cast<double>(dist) * kPhaseScale + kHalf) & 0xf;
        t.flags = static_cast<uint16_t>((t.flags & 0xffe1) | (phase << 1));
    }
    return t;
}

// ---------------------------------------------------------------------------
// ComputeTimeOfDayLightingState @004f2770 -- the colour half
// ---------------------------------------------------------------------------

void ComputeTimeOfDayLighting(const WorldTime& t, float diffuse[3], float ambient[3]) {
    // `param_1 = (flags & 0x80) ? 0.7 : 1.0`, the storm scale.
    float scale = (t.flags & 0x80) != 0 ? kStorm : kOne;

    if ((t.flags & 0x20) != 0) {
        UnpackColor(kColSunrise, diffuse);      // near sunrise: unscaled
    } else if ((t.flags & 0x40) != 0) {
        UnpackColor(kColSunset, diffuse);       // near sunset: likewise
    } else if ((t.flags & 1) != 0) {
        UnpackColor(kColNight, diffuse);        // night: brightens with the moon
        scale *= static_cast<float>(t.MoonPhase()) * kNightScaleStep + kOne;
        for (int i = 0; i < 3; ++i) diffuse[i] *= scale;
    } else {
        UnpackColor(kColDay, diffuse);          // plain day
        for (int i = 0; i < 3; ++i) diffuse[i] *= scale;
    }

    // `f`, the ambient/diffuse ratio (l.2400-2410).
    float f;
    if ((t.flags & 1) != 0) {
        f = static_cast<float>(4 - t.MoonPhase()) * kFNightStep + kHalf;
    } else if ((t.flags & 0x80) == 0 && (t.flags & 0x300) == 0) {
        f = kFDayClear;
    } else {
        f = kFDay;
    }
    for (int i = 0; i < 3; ++i) ambient[i] = f * diffuse[i];
}

WorldTime WorldTime::FromHourMinute(int h, int m) {
    // World time 0 already carries the +6h29m the normaliser adds, so subtract
    // it before converting and let FromMillis put it back.  This keeps day 1 of
    // 2197 and lands exactly on the requested wall-clock time.
    int64_t want = (static_cast<int64_t>(h) * 60 + m) - (6 * 60 + 29);
    while (want < 0) {
        want += 24 * 60;
    }
    return FromMillis(want * 60000LL);
}

float WorldTime::DayLength() const {
    if (year <= 0) {
        return kDayShort;
    }
    const int d = month - 6;
    return kDayBase - static_cast<float>(d < 0 ? -d : d);
}

float WorldTime::SunriseHour() const { return (kDayFull - DayLength()) * kHalf - kHalf; }
float WorldTime::SunsetHour() const { return (kDayFull + DayLength()) * kHalf; }

std::string WorldTime::Describe() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%02d:%02d day %lld month %d year %d  (%s%s%s) sunrise %.2f sunset %.2f",
                  hour, minute, static_cast<long long>(days), month, year,
                  (flags & 1) ? "night" : "day", (flags & 0x20) ? " sunrise" : "",
                  (flags & 0x40) ? " sunset" : "", SunriseHour(), SunsetHour());
    return buf;
}

// ---------------------------------------------------------------------------
// One light
// ---------------------------------------------------------------------------

// Light::Vtable521 @0059c724-0059c743, and the identical tail of
// Light::InitDefaultEntityFields.  Note that the constructor's version writes
// 1/r^2 into BOTH slots; the deserialiser, which runs afterwards for every
// placed light, writes r^2 then 1/r^2, and that is the pair the two lookups use.
void SceneLight::Derive() {
    radius2 = radius * radius;
    invRadius2 = (radius2 != 0.0f) ? kOne / radius2 : 0.0f;
}

namespace {

// The arc cone test, shared by the two lookups (l.2019-2054 / l.2126-2151).
// `arc * 0.5 < acos(cos) * 180/pi` rejects.
bool ArcRejects(const SceneLight& L, const float p[3]) {
    float dx = p[0] - L.pos[0], dy = p[1] - L.pos[1], dz = p[2] - L.pos[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    float u[3] = {0.0f, 0.0f, 0.0f};
    if (len != 0.0f) {
        const float k = kOne / len;
        u[0] = dx * k;
        u[1] = dz * k;   // the original stores (x, z, y) in this temporary
        u[2] = dy * k;
    }
    const float dLen = std::sqrt(L.dir[0] * L.dir[0] + L.dir[1] * L.dir[1] + L.dir[2] * L.dir[2]);
    const float uLen = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (dLen == 0.0f || uLen == 0.0f) {
        return false;
    }
    const float c = (u[2] * L.dir[1] + u[0] * L.dir[0] + u[1] * L.dir[2]) / (dLen * uLen);
    double ang = 0.0;
    if (c >= kMinusOne && c <= kOne) {
        ang = std::acos(static_cast<double>(c));
    }
    return static_cast<double>(L.arc) * kHalf < ang * kRadToDeg;
}

// The intensity-string factor of both lookups: `1 - (ch - 'a') / 25`.
float IntensityFactor(const SceneLight& L) {
    if (L.intensity.empty()) {
        return kOne;
    }
    int pos = L.intensityPos;
    if (pos >= static_cast<int>(L.intensity.size()) || pos < 0) {
        pos = 0;
    }
    const int step = static_cast<unsigned char>(L.intensity[static_cast<size_t>(pos)]) - 0x61;
    return kOne - static_cast<float>(step) * kIntensityStep;
}

}  // namespace

bool SceneLight::GetRenderLightLevel(const float p[3], float dist2, float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (radius2 < dist2) {                                        // l.2002
        return false;
    }
    if (highBound > 0.0f && pos[1] + highBound < p[1]) {          // l.2008
        return false;
    }
    if (lowBound > 0.0f && p[1] < pos[1] - lowBound) {            // l.2014
        return false;
    }
    if (useArc && ArcRejects(*this, p)) {                         // l.2019-2054
        return false;
    }
    const float t = kOne - dist2 * invRadius2;                    // l.2059
    float c[3];
    for (int i = 0; i < 3; ++i) {
        c[i] = useColor2 ? (color1[i] - color2[i]) * t + color2[i] : color1[i];
    }
    const float f = IntensityFactor(*this);                       // l.2070-2085
    for (int i = 0; i < 3; ++i) {
        out[i] = c[i] * f * t;                                    // l.2087-2089
    }
    return true;
}

bool SceneLight::GetLightEffectAtLocation(const float p[3], float dist2, float rgba[4]) const {
    if (radius2 < dist2) {
        return false;
    }
    if (highBound > 0.0f && pos[1] + highBound < p[1]) {
        return false;
    }
    if (lowBound > 0.0f && p[1] < pos[1] - lowBound) {
        return false;
    }
    if (useArc && ArcRejects(*this, p)) {
        return false;
    }
    rgba[0] = color1[0];
    rgba[1] = color1[1];
    rgba[2] = color1[2];
    // NOTE: the RGB is the raw Color1 here, with no falloff -- the falloff and
    // the flicker go into the ALPHA only.  That is the whole difference between
    // the two lookups.
    rgba[3] = (kOne - dist2 * invRadius2) * IntensityFactor(*this) * fogAlpha;
    return true;
}

void SceneLight::AdvanceTicks(int64_t ticks) {
    const int n = static_cast<int>(intensity.size());
    if (n == 0 || ticks <= 0) {
        return;
    }
    int64_t pos = static_cast<int64_t>(intensityPos) + ticks;
    if (repeating) {
        pos %= n;
    } else if (pos >= n) {
        pos = n - 1;   // the non-repeating branch destroys the light instead
    }
    intensityPos = static_cast<int32_t>(pos);
}

// ---------------------------------------------------------------------------
// The per-element accumulation
// ---------------------------------------------------------------------------

void SceneLighting::Vertex(const float p[3], float out[3]) const {
    for (int i = 0; i < 3; ++i) {
        out[i] = base[i];
    }
    if (enabled) {
        for (const SceneLight& L : lights) {
            if (!L.InRenderList() || !L.Active(time)) {
                continue;
            }
            const float dx = p[0] - L.pos[0], dy = p[1] - L.pos[1], dz = p[2] - L.pos[2];
            const float dist2 = dx * dx + dy * dy + dz * dz;
            if (dist2 > L.radius2) {
                continue;
            }
            float c[3];
            if (L.GetRenderLightLevel(p, dist2, c)) {
                for (int i = 0; i < 3; ++i) {
                    out[i] += c[i];
                }
            }
        }
    }
    if (addDiffuse) {
        for (int i = 0; i < 3; ++i) {
            out[i] += diffuse[i];
        }
    }
    // The upper clamp is the 2.0 of FLOAT_007fded8, not 1.0.
    //
    // RefreshEntityVisionAndLighting's own clamp (disassembly 006e3da0-006e3e6c)
    // is against 1.0, but the two consumers both cap the SAME quantity at 2.0
    // instead -- `if (avg >= FLOAT_007fded8) avg = 2.0` in
    // SoftwareRender::RenderLitObject @00783340 l.4290-4300 and
    // RenderAmbientLitObject @00783600 -- which is dead code if a corner can
    // never exceed 1, and the DX7 sprite path multiplies its colour by 0.5
    // before its own clamp to 1 (RenderItem_Sprite::Render, `RenderItem_Sprite::Render@00772540`
    // l.8367-8377), i.e. the vertex colour is carried at half scale into a
    // 2x-modulate blend.  Both say the useful range of a vertex colour is
    // [0, 2] with 1.0 neutral.  Clamping at 1.0 here is what produced the flat
    // (1.0, 1.0, 0.5) plateaus -- a yellow multiplier that turns grey and green
    // art olive -- because two overlapping fires push R and G past 1 while the
    // fire's own NEGATIVE blue (Color1.b = -0.024 on every `fire_*` prototype)
    // holds B down.  See NOTES.md 25.12.
    for (int i = 0; i < 3; ++i) {
        if (out[i] >= kAmbientCap) {
            out[i] = kAmbientCap;
        }
        if (!(out[i] >= 0.0f)) {
            out[i] = 0.0f;
        }
    }
}

void SceneLighting::Ambient(const float corners[4][3], float A[3]) const {
    for (int i = 0; i < 3; ++i) {
        const float s = corners[0][i] + corners[1][i] + corners[2][i] + corners[3][i];
        float v = s * kQuarter;
        if (v >= kAmbientCap) {
            v = kAmbientCap;
        }
        A[i] = v;
    }
}

void SceneLighting::AmbientForBox(const float lo[3], const float hi[3], float A[3]) const {
    const float xs[2] = {lo[0], hi[0]};
    const float zs[2] = {lo[2], hi[2]};
    float corners[4][3];
    for (int i = 0; i < 4; ++i) {
        const float p[3] = {xs[i & 1], lo[1], zs[(i >> 1) & 1]};
        Vertex(p, corners[i]);
    }
    Ambient(corners, A);
}

void SceneLighting::CornersForBox(const float lo[3], const float hi[3], float out[2][2][3]) const {
    const float xs[2] = {lo[0], hi[0]};
    const float zs[2] = {lo[2], hi[2]};
    for (int ix = 0; ix < 2; ++ix) {
        for (int iz = 0; iz < 2; ++iz) {
            const float p[3] = {xs[ix], hi[1], zs[iz]};
            Vertex(p, out[ix][iz]);
        }
    }
}

void SceneLighting::AmbientForPoint(const float p[3], float A[3]) const {
    float corners[4][3];
    Vertex(p, corners[0]);
    for (int i = 1; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) {
            corners[i][k] = corners[0][k];
        }
    }
    Ambient(corners, A);
}

size_t SceneLighting::ActiveCount() const {
    size_t n = 0;
    for (const SceneLight& L : lights) {
        if (L.InRenderList() && L.Active(time)) {
            ++n;
        }
    }
    return n;
}

}  // namespace ft
