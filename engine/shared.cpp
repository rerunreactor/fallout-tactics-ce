// engine/shared.cpp -- see shared.h.

#include "shared.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

#include "file.h"

namespace ft {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool ParseInts(const char* s, int n, int64_t* out) {
    const char* p = s;
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = std::strtoll(p, &end, 10);
        if (end == p) {
            return false;
        }
        p = end;
        if (i + 1 < n) {
            if (*p != ',') {
                return false;
            }
            ++p;
        }
    }
    return true;
}

uint8_t ColourFloatToByte(float f) {
    const int v = static_cast<int>(std::lround(f * 255.0f));
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void ApplyPaletteColorAdjustments(const DrawTint& t, uint8_t* rgb) {
    float c[3] = {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f};
    if (t.specular && t.s0 > 0.0f) {                                    // l.21509
        const float L = kLumaR * c[0] + kLumaG * c[1] + kLumaB * c[2];  // l.21513
        for (int i = 0; i < 3; ++i) {
            const float d = (L - c[i]) * t.s1 + c[i];                   // l.21516
            c[i] = (d * t.S[i] - c[i]) * t.s0 + c[i];                   // l.21518-21524
        }
    }
    for (int i = 0; i < 3; ++i) {
        c[i] *= t.A[i];                                                 // l.21544-21547
        rgb[i] = ColourFloatToByte(c[i]);                               // l.21611-21651
    }
}

LoadedSprite* SpriteCache::Get(const std::string& name) {
    const std::string key = NormaliseSpriteName(name);
    auto it = m_byName.find(key);
    if (it != m_byName.end()) {
        return &it->second;
    }
    LoadedSprite ls;
    std::string full;
    if (m_index == nullptr || m_index->Root().empty()) {
        ls.error = "no --sprites directory";
    } else if (!m_index->Find(key, &full)) {
        ls.error = "not found in sprite directory";
    } else if (!ReadWholeFile(full, &ls.file, &ls.error)) {
        // ls.error already set
    } else if (!ls.sprite.Load(ls.file, /*loadImages=*/false, &ls.error)) {
        ls.file.clear();
    } else {
        ls.ok = true;
    }
    ls.id = m_next++;
    return &m_byName.emplace(key, std::move(ls)).first->second;
}

SceneLight BuildSceneLight(const EntityInstance& inst, const Esh* proto) {
    SceneLight L;
    L.entPath = inst.entPath;
    for (int i = 0; i < 3; ++i) {
        L.pos[i] = inst.pos[i];
        // The arc axis is EntityMembers.pad2d[3..5], which Light::CheckThinkTimers
        // fills by rotating the attached entity's basis through `unk1 + 6` --
        // the third row of the same 3x3 the "frame" property carries.
        L.dir[i] = inst.rot[6 + i];
    }
    const Esh* mis = &inst.esh;
    auto B = [&](const char* n, bool iw, bool* out) {
        LightGet(mis, proto, n, iw, [&](const Esh& e, const char* nm) { return e.GetBool(nm, out); });
    };
    auto F = [&](const char* n, bool iw, float* out) {
        LightGet(mis, proto, n, iw, [&](const Esh& e, const char* nm) { return e.GetFloat(nm, out); });
    };
    auto I = [&](const char* n, bool iw, int32_t* out) {
        LightGet(mis, proto, n, iw, [&](const Esh& e, const char* nm) { return e.GetInt(nm, out); });
    };
    auto S = [&](const char* n, bool iw, std::string* out) {
        LightGet(mis, proto, n, iw, [&](const Esh& e, const char* nm) { return e.GetString(nm, out); });
    };
    auto V = [&](const char* n, bool iw, float* out) {
        LightGet(mis, proto, n, iw, [&](const Esh& e, const char* nm) { return e.GetVec3(nm, out); });
    };
    B("On", false, &L.on);
    B("Night Only", true, &L.nightOnly);
    F("Radius (meters)", true, &L.radius);
    I("Rate of change (ms)", true, &L.rateChange);
    V("Color1", true, L.color1);
    S("Intensity string (a-z)", true, &L.intensity);
    B("Repeating light intenstity", false, &L.repeating);
    B("Use Color2", false, &L.useColor2);
    V("Color2", false, L.color2);
    B("Use Arc", true, &L.useArc);
    I("Arc (degrees)", true, &L.arc);
    F("High Bound", true, &L.highBound);
    F("Low Bound", true, &L.lowBound);
    B("Use Fog", true, &L.useFog);
    F("Fog Alpha", true, &L.fogAlpha);
    L.Derive();
    return L;
}

bool TileInstanceRoofHidden(const World& w, const TileInstance& inst, const int64_t inside[3],
                            double k) {
    const int64_t refX = inside[0], refY = inside[1], refZ = inside[2];
    const TileHeader& ph = w.protos[static_cast<size_t>(inst.protoId)].header;
    if (refY + 8 <= inst.min[1] && inst.min[0] < refX - 4 && inst.min[2] < refZ - 4 &&
        static_cast<int>(ph.bboxX) * static_cast<int>(ph.bboxZ) > 4) {   // l.4489-4503
        const double cx = (inst.min[0] + inst.max[0]) / 2;
        const double cz = (inst.min[2] + inst.max[2]) / 2;
        const double r = (inst.max[1] - refY) * k;                       // l.4505-4508
        if ((cz - refZ) * (cz - refZ) + (cx - refX) * (cx - refX) < r * r) {
            return true;
        }
    }
    return false;
}

void BuildLayerTints(const EntityInstance& inst, const float ambient[3], const int* layerSlot,
                     bool tintOn, DrawTint out[kLayerCount]) {
    for (int l = 0; l < kLayerCount; ++l) {
        DrawTint& t = out[l];
        t = DrawTint();
        for (int k = 0; k < 3; ++k) {
            t.A[k] = ambient[k];
        }
        const int slot = layerSlot != nullptr ? layerSlot[l] : l;
        if (tintOn && slot >= 0 && slot < 4 && inst.hasColour[slot]) {
            t.specular = true;
            for (int k = 0; k < 3; ++k) {
                t.S[k] = inst.colour[slot][k];
            }
            t.s0 = inst.colour[slot][3];
            t.s1 = inst.colour[slot][4];
        }
        // An absent record means "equal to the compile-time default", which
        // InitDefaultEntityFields sets to the global ambient with both scalars
        // 0 -- and s0 == 0 skips the whole specular block, so the layer gets
        // `c * A` like a tile.
    }
}

// ClassifyByteTripleRelation @006f02d0, ported exactly.
// The argument there is `Tile + 0x12`, whose byte order our .til reading fixes
// as (x, y, z) -- so byte 0 is bbox.x, byte 1 is bbox.y and byte 2 is bbox.z.
// Two independent checks agree with that reading: its `< 2` test
// returning 2 makes every 6x1x6 floor tile flat, and class 1 (`bboxX < bboxZ`,
// thin in x) is the case BuildShapeVertices answers with a quad in the plane
// x = min.x.  Section 24.6's `proto.bbox.x * proto.bbox.z` reads Tile+0x12 and
// Tile+0x14 as x and z, which is the same assignment.
int ClassifyTileShape(uint8_t bboxX, uint8_t bboxY, uint8_t bboxZ) {
    if (bboxX == bboxY && bboxX == bboxZ) {
        return 5;
    }
    if (bboxY < 2) {
        return 2;
    }
    if (bboxZ < bboxX) {
        return 3;
    }
    if (bboxX < bboxZ) {
        return 1;
    }
    const unsigned x = bboxX, y = bboxY, z = bboxZ;
    if (y * 2 < x && y * 2 < z) {
        return 2;
    }
    if (x * 2 < y && x * 2 < z) {
        return 1;
    }
    if (z * 2 < y && z * 2 < x) {
        return 3;
    }
    if (x * 2 < y && z * 2 < y) {
        return 4;
    }
    return 0;
}

// BuildShapeVertices @006e4220, ported.  The four vertices it writes are lit by
// SceneLighting and then interpolated across the element's sprite; the quad's
// (u, v) is the inverse of the iso projection restricted to the plane the four
// vertices lie in, so the Gouraud interpolation runs over the real surface.
//
//   ProjectWorldToScreen:  SX = 6*(x - z),  SY = -3*(x + z) - 7*y
//
// so on a plane of constant x the remaining two world axes are recoverable from
// (SX, SY) affinely, and likewise for constant z and for the degenerate edge.
void BuildLightQuad(const SceneLighting& scene, const TileInstance& in, int shapeClass,
                    LightQuad* q) {
    const double mnx = in.min[0], mny = in.min[1], mnz = in.min[2];
    const double mxx = in.max[0], mxy = in.max[1], mxz = in.max[2];
    const double dx = mxx - mnx, dy = mxy - mny, dz = mxz - mnz;

    // Light one world point and store it at c[a][b].
    auto vertexAt = [&](double wx, double wy, double wz, int a, int b) {
        const float p[3] = {static_cast<float>(wx / kEntityWorldScale),
                            static_cast<float>(wy / kEntityWorldScale),
                            static_cast<float>(wz / kEntityWorldScale)};
        float lit[3];
        scene.Vertex(p, lit);
        for (int k = 0; k < 3; ++k) {
            q->c[a][b][k] = lit[k];
        }
    };

    q->ax = q->bx = q->cx = 0.0;
    q->az = q->bz = q->cz = 0.0;

    if (shapeClass == 1 && dy > 0.0 && dz > 0.0) {
        // The wall in the plane x = min.x.  u runs along y, v along z, which is
        // the order BuildShapeVertices writes:
        //   v0 (max.y, max.z)  v1 (max.y, min.z)  v2 (min.y, min.z)  v3 (min.y, max.z)
        vertexAt(mnx, mxy, mxz, 1, 1);
        vertexAt(mnx, mxy, mnz, 1, 0);
        vertexAt(mnx, mny, mnz, 0, 0);
        vertexAt(mnx, mny, mxz, 0, 1);
        // x is fixed, so  z = min.x - SX/6  and  y = (SX/2 - SY - 6*min.x)/7.
        q->ax = 1.0 / (14.0 * dy);
        q->bx = -1.0 / (7.0 * dy);
        q->cx = ((-6.0 * mnx / 7.0) - mny) / dy;
        q->az = -1.0 / (6.0 * dz);
        q->bz = 0.0;
        q->cz = (mnx - mnz) / dz;
        q->on = true;
        return;
    }
    if (shapeClass == 3 && dy > 0.0 && dx > 0.0) {
        // The wall in the plane z = min.z.  u runs along y, v along x:
        //   v0 (max.y, min.x)  v1 (max.y, max.x)  v2 (min.y, max.x)  v3 (min.y, min.x)
        vertexAt(mnx, mxy, mnz, 1, 0);
        vertexAt(mxx, mxy, mnz, 1, 1);
        vertexAt(mxx, mny, mnz, 0, 1);
        vertexAt(mnx, mny, mnz, 0, 0);
        // z is fixed, so  x = min.z + SX/6  and  y = (-SX/2 - SY - 6*min.z)/7.
        q->ax = -1.0 / (14.0 * dy);
        q->bx = -1.0 / (7.0 * dy);
        q->cx = ((-6.0 * mnz / 7.0) - mny) / dy;
        q->az = 1.0 / (6.0 * dx);
        q->bz = 0.0;
        q->cz = (mnz - mnx) / dx;
        q->on = true;
        return;
    }
    if (shapeClass == 4 && dy > 0.0) {
        // The billboard edge at (min.x, *, min.z): v0 == v1 and v2 == v3, so
        // only the vertical gradient survives.  x and z are both fixed, so
        // y = (-SY - 3*(min.x + min.z))/7 and v never varies.
        vertexAt(mnx, mny, mnz, 0, 0);
        for (int k = 0; k < 3; ++k) {
            q->c[0][1][k] = q->c[0][0][k];
        }
        vertexAt(mnx, mxy, mnz, 1, 0);
        for (int k = 0; k < 3; ++k) {
            q->c[1][1][k] = q->c[1][0][k];
        }
        q->ax = 0.0;
        q->bx = -1.0 / (7.0 * dy);
        q->cx = ((-3.0 * (mnx + mnz) / 7.0) - mny) / dy;
        q->on = true;
        return;
    }

    // The default case: the TOP face of the box, at y = max[1].
    const float lo[3] = {in.min[0] / kEntityWorldScale, in.min[1] / kEntityWorldScale,
                         in.min[2] / kEntityWorldScale};
    const float hi[3] = {in.max[0] / kEntityWorldScale, in.max[1] / kEntityWorldScale,
                         in.max[2] / kEntityWorldScale};
    scene.CornersForBox(lo, hi, q->c);
    // Invert the iso projection on the top-face plane y = max[1]:
    //   SX = 6*(x - z)                 -> x - z = SX/6
    //   SY = -3*(x + z) - 7*y          -> x + z = (-SY - 7*y)/3
    const double y = static_cast<double>(in.max[1]);
    const double dxr = static_cast<double>(in.max[0] - in.min[0]);
    const double dzr = static_cast<double>(in.max[2] - in.min[2]);
    const double kx0 = 1.0 / 12.0, kx1 = -1.0 / 6.0, kx2 = -7.0 * y / 6.0;
    const double kz0 = -1.0 / 12.0, kz1 = -1.0 / 6.0, kz2 = -7.0 * y / 6.0;
    if (dxr > 0.0) {
        q->ax = kx0 / dxr;
        q->bx = kx1 / dxr;
        q->cx = (kx2 - in.min[0]) / dxr;
    } else {
        q->ax = q->bx = 0.0;
        q->cx = 0.0;
    }
    if (dzr > 0.0) {
        q->az = kz0 / dzr;
        q->bz = kz1 / dzr;
        q->cz = (kz2 - in.min[2]) / dzr;
    } else {
        q->az = q->bz = 0.0;
        q->cz = 0.0;
    }
    q->on = true;
}

void BlitRGBA(const BlitTarget& t, const uint8_t* rgba, int32_t width, int32_t height, int64_t dx,
              int64_t dy, const DrawTint& tint, const LightQuad* quad) {
    const bool identity = tint.Identity() && (quad == nullptr || !quad->on);
    for (int32_t row = 0; row < height; ++row) {
        const int64_t ty = dy + row;
        if (ty < 0 || ty >= t.h) {
            continue;
        }
        const uint8_t* src = rgba + static_cast<size_t>(row) * width * 4;
        uint8_t* dst = t.pixels + (static_cast<size_t>(ty) * t.w + dx) * 4;
        for (int32_t col = 0; col < width; ++col, src += 4, dst += 4) {
            const int64_t tx = dx + col;
            if (tx < 0 || tx >= t.w) {
                continue;
            }
            uint8_t px[3] = {src[0], src[1], src[2]};
            if (!identity) {
                if (quad != nullptr && quad->on) {
                    // Gouraud: bilinear of the four corner colours.  The iso
                    // projection is affine, so (u, v) are affine in screen space
                    // and this is exactly what the hardware interpolator
                    // produces across the quad.
                    const double SX = static_cast<double>(tx + t.originX);
                    const double SY = static_cast<double>(ty + t.originY);
                    double u = quad->ax * SX + quad->bx * SY + quad->cx;
                    double v = quad->az * SX + quad->bz * SY + quad->cz;
                    u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
                    v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
                    DrawTint g = tint;
                    for (int k = 0; k < 3; ++k) {
                        const double a0 =
                            quad->c[0][0][k] + (quad->c[0][1][k] - quad->c[0][0][k]) * v;
                        const double a1 =
                            quad->c[1][0][k] + (quad->c[1][1][k] - quad->c[1][0][k]) * v;
                        g.A[k] = static_cast<float>(tint.A[k] * (a0 + (a1 - a0) * u));
                    }
                    ApplyPaletteColorAdjustments(g, px);
                } else {
                    ApplyPaletteColorAdjustments(tint, px);
                }
            }
            const uint8_t s0 = px[0], s1 = px[1], s2 = px[2];
            const uint32_t a = src[3];
            if (a == 0) {
                continue;
            }
            if (a == 255) {
                dst[0] = s0;
                dst[1] = s1;
                dst[2] = s2;
                dst[3] = 255;
                continue;
            }
            const uint32_t ia = 255 - a;
            dst[0] = static_cast<uint8_t>((s0 * a + dst[0] * ia) / 255);
            dst[1] = static_cast<uint8_t>((s1 * a + dst[1] * ia) / 255);
            dst[2] = static_cast<uint8_t>((s2 * a + dst[2] * ia) / 255);
            dst[3] = static_cast<uint8_t>(a + dst[3] * ia / 255);
        }
    }
}

}  // namespace ft
