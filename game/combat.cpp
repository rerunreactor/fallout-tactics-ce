// combat.cpp -- the data side of milestone 8.  See combat.h for the sources.

#include "combat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace game {

// ---------------------------------------------------------------------------
// MILESTONE 25: the experience ladder.
// ---------------------------------------------------------------------------
// `GetLevelForXPAmount @ 00607950` and `GetNeededXPForLevel @ 00607a90` both
// walk one 15-entry `.rdata` table, `XPLevelValues @ 0x008a3e74` -- the XP at
// which levels 1..15 are reached.  Past the table both extrapolate:
// `170000 * 1.25^(L-15)` truncated to two significant figures
// (`floor(x / 10^floor(log10 x - 1)) * 10^...`, the `_DAT_0080c0c8 == 1.25`
// and `_DAT_0080c0d0 == 1.0` the dump leaves unresolved), and the level is
// capped at 51 (`0x33`).
namespace {
// `GetValueTierAndRemainder @0060f9f0`: the cost of one point of a skill and
// the distance to the next ceiling.  Every shipped percentage is under 100, so
// the cost is 1 and the ceiling 100 in practice.
void ValueTierAndRemainder(int32_t v, int32_t* tier, int32_t* remainder) {
    if (v < 100)  { *remainder = 100 - v;  *tier = 1;    return; }
    if (v < 125)  { *remainder = 125 - v;  *tier = 2;    return; }
    if (v < 150)  { *remainder = 150 - v;  *tier = 3;    return; }
    if (v < 175)  { *remainder = 175 - v;  *tier = 4;    return; }
    if (v < 200)  { *remainder = 200 - v;  *tier = 5;    return; }
    if (v < 300)  { *remainder = 300 - v;  *tier = 6;    return; }
    *remainder = 0; *tier = 1000;
}
}  // namespace

int SpendSkillPointsOnSkill(Attributes* a, int skill, int points) {
    if (skill < 0 || skill >= kSkillCount) return points;
    const bool tag = a->tagged[skill];
    for (int iter = 0; iter < 6; ++iter) {
        int32_t tier = 1, remainder = 0;
        ValueTierAndRemainder(a->skills[skill], &tier, &remainder);
        if (remainder < 1) return points;
        const int32_t n = points / tier;
        if (n < 1) break;
        // The remainder becomes `(r & 1) + r / 2` -- a tagged skill's
        // headroom is halved, ROUNDED UP, because each point buys two.
        if (tag) remainder = (remainder & 1) + remainder / 2;
        const int32_t take = std::min(n, remainder);
        a->skills[skill] += tag ? take * 2 : take;
        points -= take * tier;
        a->skillPoints -= take * tier;
    }
    return points;
}

void AutoAllocateLevelUpPoints(Attributes* a) {
    const int32_t budget = (a->skillPoints * 5) / 18;
    if (budget < 1) return;
    for (int i = 0; i < kSkillCount; ++i) {
        if (a->tagged[i]) SpendSkillPointsOnSkill(a, i, budget);
    }
}

int ApplyStoredLevel(Attributes* a, int npcLevel) {
    if (npcLevel < 1) return 0;
    const int32_t needed = ExperienceForLevel(npcLevel);
    if (a->experience >= needed) return 0;
    const int levels = LevelForExperience(needed) - LevelForExperience(a->experience);
    a->experience = needed;
    if (levels < 1) return 0;
    // The three terms the grant reads come off the SUMMED sheet, so they are
    // taken from a derived copy and the grant is written back to the base.
    Attributes eff = *a;
    eff.Derive();
    const int32_t hpPerLevel = eff.endurance / 2 + 3;
    const int32_t levelsPerPerk = eff.levelsPerPerk;
    for (int i = 0; i < levels; ++i) {
        a->levSincePerk += 1;
        if (levelsPerPerk > 0 && a->levSincePerk >= levelsPerPerk) {
            a->levSincePerk = 0;
            a->perksToTake += 1;
        }
        a->skillPoints += eff.skillPerLevel;
        a->maxHitPointsBonus += hpPerLevel;
    }
    return levels;
}

namespace {
const int32_t kXPLevelValues[15] = {
    0, 1000, 2000, 4000, 7000, 10000, 15000, 20000,
    30000, 45000, 60000, 80000, 100000, 130000, 170000,
};
}  // namespace

int32_t ExperienceForLevel(int level) {
    const int i = level - 1;
    if (i < 0) return 0;
    if (i <= 14) return kXPLevelValues[i];
    double x = 170000.0 * std::pow(1.25, static_cast<double>(level - 15));
    const double d = std::pow(10.0, std::floor(std::log10(x) - 1.0));
    return static_cast<int32_t>(std::floor(x / d) * d);
}

int LevelForExperience(int32_t xp) {
    if (xp < 0) return 0;
    if (xp < kXPLevelValues[14]) {
        int i = 14;
        do { --i; } while (i > 0 && xp < kXPLevelValues[i]);
        return i + 1;
    }
    if (xp == kXPLevelValues[14]) return 15;
    // The extrapolated arm, transcribed from the dump's own shape: the number
    // of 1.25 doublings past the table's last entry, then one step back if the
    // truncated threshold for that level is above the XP in hand.
    const int n = static_cast<int>(
        std::floor(std::log(static_cast<double>(xp) / kXPLevelValues[14]) / std::log(1.25))) + 1;
    const int lo = n + 14, hi = n + 15;
    const int level = (ExperienceForLevel(hi) <= xp) ? hi : lo;
    return level < 0x33 ? level : 0x33;
}

namespace {

// ---------------------------------------------------------------------------
// `Tables::ActionTypes @ 0062f090`: 41 {const char* name, int cost, int, int}
// records of 16 bytes at 0x008a8a38.  Transcribed from the executable's .rdata.
// `ActionPointCostAdjust @ 0062f1a0` adds a per-difficulty tweak on top; it is
// zero at the default setting, which is what fotgame runs.
// ---------------------------------------------------------------------------
struct ActionTypeRow { const char* name; int32_t cost; };
const ActionTypeRow kActionTypes[] = {
    {"H2H (Jab)", 6},                {"H2H (Kick)", 4},
    {"H2H (Punch)", 3},              {"H2H (Side Kick)", 4},
    {"H2H Expensive (Jab)", 8},      {"H2H Expensive (Punch)", 4},
    {"H2H Expensive (Side Kick)", 6},{"Melee Cheap", 3},
    {"Melee", 4},                    {"Melee Expensive", 5},
    {"Ranged Burst", 5},             {"Ranged Spray", 6},
    {"Ranged Single Cheap", 3},      {"Ranged Single", 4},
    {"Ranged Single Expensive", 6},  {"Set Trap", 4},
    {"Throw", 3},                    {"Throw Expensive", 6},
    {"drop", 1},                     {"pickup", 1},
    {"moveItem", 1},                 {"reload", 2},
    {"useItem", 1},                  {"activate", 2},
    {"equip", 2},                    {"unEquip", 2},
    {"disembark", 0},                {"embark", 2},
    {"driver", 3},                   {"getup", 2},
    {"stance", 2},                   {"targeted", 1},
    {"firstAid", 10},                {"doctor", 15},
    {"sneak", 2},                    {"lockpick", 2},
    {"steal", 2},                    {"traps", 4},
    {"science", 2},                  {"repair", 15},
    {"barter", 2},
};

// `LookupDamageThresholdByName @ 0062f9f0` / `...ResistanceByName @ 0062fad0`:
// 12 {const char* name, float damageMult, float drMod} records of 12 bytes at
// 0x008a8e60.  An unknown name returns 1.0 for both.
struct AmmoRow { const char* name; float dmg; float dr; };
const AmmoRow kAmmoVariants[] = {
    {"None", 1.00f, 1.00f},
    {"Ball", 0.90f, 1.20f},
    {"Armour Piercing", 0.75f, 0.50f},
    {"Jacketed Hollow Point", 1.60f, 1.20f},
    {"Full Metal Jacket", 1.00f, 0.85f},
    {"Explosive", 1.60f, 0.90f},
    {"Large Explosive", 2.80f, 0.80f},
    {"Composite", 0.90f, 0.70f},
    {"Non-Lethal", 0.00f, 1.50f},
    {"Flame", 1.00f, 0.80f},
    {"Refined", 1.50f, 0.60f},
    {"Depleted Uranuim", 1.00f, 0.50f},   // the game's own spelling
};

// The body-location table at 0x008a8da4, records of {int toHitPenalty,
// int critBonus, const char* name} of 12 bytes.  `FUN_0062f890` returns the
// first field and the call sites pass the string an uncalled shot leaves empty,
// which `GetBodyPartIndexByName @ 0062f760` maps to index 0.
struct BodyPartRow { const char* name; int32_t toHit; int32_t crit; };
const BodyPartRow kBodyParts[] = {
    {"Torso", 0, 0},      {"Head", 0, 3},       {"Eyes", 20, 10},
    {"Right Arm", 30, 30},{"Left Arm", 15, 7},  {"Groin", 15, 7},
    {"Left Leg", 15, 8},  {"Right Leg", 10, 5}, {"None", 10, 5},
};

// `RollCriticalEffectForBodyLocation @ 0061c550`: the cumulative weight table
// `byte[9][9]` at 0x008a6c54.  Row = body location, column = effect, and the
// walk stops at the first column whose running total exceeds the roll.
const uint8_t kCritEffect[9][9] = {
    {30, 20, 25, 13,  0,  0,  9,  0,  0},   // Torso
    {25, 25, 30, 13,  0,  0,  5,  0,  0},   // Head
    {25, 25, 13, 15,  0,  0, 18,  0,  1},   // Eyes
    {20, 30,  5, 28,  0,  0, 15,  0,  1},   // Right Arm
    {30, 20,  5, 28, 12,  0,  0,  5,  0},   // Left Arm
    {30, 20,  5, 28,  0, 12,  0,  5,  0},   // Groin
    {20, 35, 15, 15,  0,  0,  5,  0,  1},   // Left Leg
    {20, 15, 40, 22,  0,  0,  0,  0,  0},   // Right Leg
    {20, 15, 40, 22,  0,  0,  0,  0,  0},   // None
};

const char* const kSkillNames[kSkillCount] = {
    "smallGuns", "bigGuns", "energyWeapons", "unarmed", "meleeWeapons",
    "throwing", "firstAid", "doctor", "sneak", "lockpick", "steal", "traps",
    "science", "repair", "pilot", "barter", "gambling", "outdoorsman",
};

bool IEquals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

int32_t Int32LE(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

}  // namespace

// ---------------------------------------------------------------------------

DamageType DamageTypeFromName(const std::string& name) {
    if (IEquals(name, "energy") || IEquals(name, "laser") || IEquals(name, "plasma")) {
        return kDmgEnergy;
    }
    if (IEquals(name, "fire")) return kDmgFire;
    if (IEquals(name, "gas")) return kDmgGas;
    if (IEquals(name, "explode") || IEquals(name, "explosive")) return kDmgExplode;
    if (IEquals(name, "electrical") || IEquals(name, "electric")) return kDmgElectrical;
    return kDmgNormal;
}

const char* DamageTypeName(DamageType t) {
    static const char* const kNames[kDamageTypeCount] = {
        "normal", "energy", "fire", "gas", "explode", "electrical"};
    return kNames[static_cast<int>(t)];
}

int SkillIndexFromName(const std::string& name) {
    for (int i = 0; i < kSkillCount; ++i) {
        if (IEquals(name, kSkillNames[i])) return i;
    }
    return -1;
}

const char* SkillName(int index) {
    if (index < 0 || index >= kSkillCount) return "";
    return kSkillNames[index];
}

int32_t ActionTypeCost(const std::string& name) {
    for (const ActionTypeRow& r : kActionTypes) {
        if (IEquals(name, r.name)) return r.cost;
    }
    // `Tables::ActionTypes` reports "cannot find" and returns 0.
    return 0;
}

AmmoVariant AmmoVariantByName(const std::string& name) {
    for (const AmmoRow& r : kAmmoVariants) {
        if (IEquals(name, r.name)) {
            AmmoVariant v;
            v.damageMult = r.dmg;
            v.drMod = r.dr;
            return v;
        }
    }
    return AmmoVariant();
}

namespace {
int BodyPartIndex(const std::string& name) {
    if (name.empty()) return 0;   // GetBodyPartIndexByName's miss returns 0
    for (size_t i = 0; i < sizeof(kBodyParts) / sizeof(kBodyParts[0]); ++i) {
        if (IEquals(name, kBodyParts[i].name)) return static_cast<int>(i);
    }
    return 0;
}
}  // namespace

int32_t CalledShotToHitPenalty(const std::string& bodyPart) {
    return kBodyParts[BodyPartIndex(bodyPart)].toHit;
}
int32_t CalledShotCritBonus(const std::string& bodyPart) {
    return kBodyParts[BodyPartIndex(bodyPart)].crit;
}

int32_t RollCriticalEffect(Rng* rng, int32_t bodyLocation, int32_t roll) {
    int32_t flags = 0;
    int guard = 0;
    while (bodyLocation >= 0 && bodyLocation < 9 && guard++ < 8) {
        int32_t running = 0;
        int col = 0;
        for (; col < 9; ++col) {
            running += kCritEffect[bodyLocation][col];
            if (roll < running) break;
        }
        if (col < 9) {
            switch (col) {
                case 0: flags |= kCritBypassArmour; break;
                case 1: flags |= kCritDoubleDamage; break;
                case 2: flags |= kCritKnockOver; break;
                case 3: flags |= 0x08; break;
                case 4: flags |= 0x10; break;
                case 5: flags |= kCritDropWeapon; break;
                case 6: flags |= kCritKnockout; break;
                case 7: flags |= kCritWeaponDamage; break;
                default: flags |= kCritMegaDamage; break;
            }
            return flags;
        }
        // The roll fell past every bucket: the original re-rolls into a
        // shifted window and recurses, which can stack a second effect.
        const int32_t hi = std::max(51, running - 5);
        RollCriticalEffect(rng, bodyLocation, rng->RangeI(50.0f, static_cast<float>(hi)));
        roll = rng->RangeI(1.0f, 49.0f);
    }
    return flags;
}

int32_t CriticalDamageMultiplier(Rng* rng, int32_t critFlags, bool targetIsPlayer) {
    int32_t mult = 1;
    if ((critFlags & kCritDoubleDamage) != 0) mult = 2;
    if ((critFlags & kCritMegaDamage) == 0) return mult;
    // The MegaDamage branch: `powf` of a normalised roll scaled into
    // [kMegaDamageBase, kMegaDamageBase + kMegaDamageRange], times the running
    // multiplier; then forced to 4 for a player character.
    const float r = static_cast<float>(rng->Rand15()) / 32767.0f;
    const int32_t big = static_cast<int32_t>((r * k::kMegaDamageRange + k::kMegaDamageBase) *
                                             static_cast<float>(mult));
    if (targetIsPlayer) return 4;
    return big;
}

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

const char* OTraitName(int index) {
    // The `.rdata` table at 0x008a42d0, verbatim.
    static const char* const kNames[kOTraitCount] = {
        "fastMetabolism", "bruiser", "smallFrame", "oneHander", "finesse", "kamikaze",
        "heavyHanded", "fastShot", "bloodyMess", "jinxed", "goodNatured", "chemReliant",
        "chemResistant", "nightPerson", "skilled", "gifted", "glowingOne", "techWizard",
        "fearTheReaper", "vatSkin", "hamFisted", "domesticated", "rabid", "tightNuts",
        "targetingComputer", "betaSoftware", "empShielding", "Human", "Ghoul", "Mutant",
        "RobotHumanoid", "Deathclaw", "Dog", "doAdrenalineRush", "doDieHard", "doHthEvade",
        "doDrunkenMaster", "doNightPerson"};
    if (index < 0 || index >= kOTraitCount) return "";
    return kNames[index];
}

// MILESTONE 21.  `Attributes::InitCopyAttributes @ 006080c0` then
// `Attributes::SumFinalAttributes @ 006088d0`, in that order, over the sheet as
// the `.ent` stores it.  Every `Derived` and `Skills` value a `.ent` carries is
// a DELTA the two of them sum on top of -- which is why every shipped prefab
// carries `armorclass 0` and reads 8 on the character sheet, and why an actor
// with no `Skills` values at all still has eighteen non-zero percentages.
//
// The check that pins it: for every shipped actor whose `.ent` does NOT store a
// `DerivedStats/maxHitPoints` delta, the max hit points this computes are
// EXACTLY the `Current Attributes/hitPoints` the file was authored with --
// raider_04_range 30, radscorp_01 15, lizard_01 10, wasp_01 10, ldc_01 13,
// wolf_01 23 (Dog, +8), dog_01 22 (Dog), bdc_01 15, and all five `prefab*.ent`
// (Mick 37, and prefab3's 33, which only comes out right once `gifted`'s +1 to
// every SPECIAL is applied).  Thirteen for thirteen.
void Attributes::Derive() {
    // ---- Attributes::InitCopyAttributes @ 006080c0 -------------------------
    maxCarryWeight += 25;
    maxActionPoints = maxActionPointsBonus + 5;
    healRate += 1;
    meleeDamage += 1;
    skillPerLevel += 5;
    levelsPerPerk += 4;
    maxHitPoints = maxHitPointsBonus;
    skills[kSkillSmallGuns] += 5;
    skills[kSkillUnarmed] += 30;
    skills[kSkillMeleeWeapons] += 20;
    skills[kSkillFirstAid] += 30;
    skills[kSkillLockpick] += 10;
    skills[kSkillTraps] += 10;

    // The race block: one of six, tested in this order (human, ghoul, mutant,
    // deathclaw, robotHumanoid, dog), and a sheet with none of them -- every
    // shipped critter but the dogs and the deathclaws -- gets nothing.
    if (otrait[kOtHuman]) {
        maxHitPoints += 15;
        levelsPerPerk -= 1;
        resist[kDmgElectrical] += 10;
    } else if (otrait[kOtGhoul]) {
        maxHitPoints += 10;
        radResist += 40;
        poisonResist += 10;
        resist[kDmgElectrical] += 10;
    } else if (otrait[kOtMutant]) {
        resist[kDmgGas] += 30;   thresh[kDmgFire] += 5;
        resist[kDmgExplode] += 10;  thresh[kDmgGas] += 5;
        resist[kDmgElectrical] += 10;
        maxHitPoints += 25;      bonusDamage += 6;
        resist[kDmgNormal] += 25;   resist[kDmgEnergy] += 10;  resist[kDmgFire] += 15;
        thresh[kDmgNormal] += 5; thresh[kDmgExplode] += 3;
        thresh[kDmgEnergy] += 2; thresh[kDmgElectrical] += 2;
        radResist += 20;         poisonResist += 20;
    } else if (otrait[kOtDeathclaw]) {
        maxHitPoints += 30;      bonusDamage += 3;
        resist[kDmgNormal] += 15;   resist[kDmgEnergy] += 10;  resist[kDmgGas] += 25;
        resist[kDmgExplode] += 10;  resist[kDmgElectrical] += 10;
        thresh[kDmgNormal] += 5; thresh[kDmgEnergy] += 3;  thresh[kDmgGas] += 5;
        thresh[kDmgExplode] += 2; thresh[kDmgElectrical] += 2;
        radResist += 10;
    } else if (otrait[kOtRobotHumanoid]) {
        thresh[kDmgFire] += 20;  skillPerLevel += 5;
        resist[kDmgNormal] += 50;   resist[kDmgExplode] += 15; resist[kDmgEnergy] += 15;
        resist[kDmgElectrical] -= 50;  resist[kDmgFire] += 55;
        thresh[kDmgNormal] += 16;   resist[kDmgGas] += 100;
        thresh[kDmgEnergy] += 4;
        levelsPerPerk = 99;      thresh[kDmgExplode] += 4;
        poisonResist = 100;      radResist = 100;
    } else if (otrait[kOtDog]) {
        maxHitPoints += 8;
        levelsPerPerk -= 2;
        resist[kDmgElectrical] += 10;
    }

    // The optional traits, in the binary's own order.
    if (otrait[kOtBruiser]) { strength += 2; maxActionPoints -= 2; }
    if (otrait[kOtFastMetabolism]) healRate += 2;
    if (otrait[kOtSmallFrame]) { maxCarryWeight -= 50; agility += 1; }
    if (otrait[kOtFinesse]) { criticalChance += 10; bonusDamage -= 25; }
    if (otrait[kOtKamikaze]) bonusDamage += 25;
    if (otrait[kOtHeavyHanded]) meleeDamage += 4;
    if (otrait[kOtGoodNatured]) {
        skills[kSkillSmallGuns] -= 10;    skills[kSkillBigGuns] -= 10;
        skills[kSkillEnergyWeapons] -= 10; skills[kSkillUnarmed] -= 10;
        skills[kSkillMeleeWeapons] -= 10;  skills[kSkillThrowing] -= 10;
        skills[kSkillFirstAid] += 20;      skills[kSkillDoctor] += 20;
        skills[kSkillBarter] += 20;
    }
    if (otrait[kOtSkilled]) { skillPerLevel += 5; levelsPerPerk += 1; }
    if (otrait[kOtGifted]) {
        strength += 1; perception += 1; endurance += 1; charisma += 1;
        intelligence += 1; agility += 1; luck += 1;
        for (int i = 0; i < kSkillCount; ++i) skills[i] -= 10;
        skillPerLevel -= 5;
    }
    if (otrait[kOtGlowingOne]) radResist += 35;
    if (otrait[kOtTechWizard]) {
        skills[kSkillScience] += 20; skills[kSkillRepair] += 20;
        skills[kSkillLockpick] += 20; perception -= 1;
    }
    if (otrait[kOtOneHander]) {
        skills[kSkillSmallGuns] += 10; skills[kSkillThrowing] += 10;
        skills[kSkillUnarmed] += 10;   skills[kSkillBigGuns] -= 30;
    }
    if (otrait[kOtFearTheReaper]) levelsPerPerk -= 1;
    if (otrait[kOtVatSkin]) armorClass += 10;
    if (otrait[kOtHamFisted]) {
        skills[kSkillTraps] -= 10;  skills[kSkillRepair] -= 10;
        skills[kSkillLockpick] -= 10; skills[kSkillScience] -= 10;
        skills[kSkillDoctor] -= 10; skills[kSkillFirstAid] -= 10;
        tagged[kSkillUnarmed] = true;
    }
    if (otrait[kOtDomesticated]) intelligence += 2;
    if (otrait[kOtTightNuts]) {
        thresh[kDmgNormal] += 5; thresh[kDmgEnergy] += 5; thresh[kDmgFire] += 5;
        thresh[kDmgGas] += 5;    thresh[kDmgExplode] += 5;
    }
    if (otrait[kOtEmpShielding]) { agility -= 2; resist[kDmgElectrical] += 30; }

    // ---- Attributes::SumFinalAttributes @ 006088d0 -------------------------
    skillPerLevel += intelligence * 2;
    if (otrait[kOtDoAdrenalineRush]) strength += adrenalineRush;
    // (the race stat limits `FUN_00626e10(traits.race)` clamps to are a table
    // this port does not read; no shipped sheet is outside them.)
    maxCarryWeight += strength * 25;
    criticalChance += luck;
    maxActionPoints += agility / 2;
    healRate += std::max(0, (endurance - 3) / 3);
    maxHitPoints += endurance * 2 + strength;
    if (!otrait[kOtFastMetabolism]) {
        radResist += endurance * 2;
        poisonResist += endurance * 5;
    }
    if (!otrait[kOtKamikaze]) armorClass += agility;
    if (otrait[kOtDoHthEvade]) armorClass += (hthEvade * skills[kSkillUnarmed]) / 8;
    armorClass += bonusAc;
    meleeDamage += std::max(0, strength - (otrait[kOtDomesticated] ? 10 : 6));
    if (otrait[kOtDoDieHard]) {
        for (int i = 0; i < kDamageTypeCount; ++i) resist[i] += dieHard * 10;
        radResist += dieHard * 10;
        poisonResist += dieHard * 10;
    }
    if (otrait[kOtDoDrunkenMaster]) skills[kSkillUnarmed] += 20;
    if (otrait[kOtNightPerson]) {
        const int32_t d = otrait[kOtDoNightPerson] ? 1 : -1;
        intelligence += d;
        perception += d;
    }

    // `Skills::AddStatBonusesToSkills @ 0060f320`, the
    // last line of the sum: the SPECIAL term for each of the eighteen, then a
    // flat +20 for every tag.
    skills[kSkillSmallGuns] += agility * 4;
    skills[kSkillBigGuns] += agility * 2;
    skills[kSkillEnergyWeapons] += agility * 2;
    skills[kSkillUnarmed] += (strength + agility) * 2;
    skills[kSkillMeleeWeapons] += (strength + agility) * 2;
    skills[kSkillThrowing] += agility * 4;
    skills[kSkillFirstAid] += (perception + intelligence) * 2;
    skills[kSkillDoctor] += perception + intelligence;
    skills[kSkillSneak] += agility * 3;
    skills[kSkillLockpick] += perception + agility;
    skills[kSkillSteal] += agility * 3;
    skills[kSkillTraps] += perception + agility;
    skills[kSkillScience] += intelligence * 4;
    skills[kSkillRepair] += intelligence * 3;
    skills[kSkillPilot] += (perception + agility) * 2;
    skills[kSkillBarter] += charisma * 4;
    skills[kSkillGambling] += luck * 3 + intelligence * 2;
    skills[kSkillOutdoorsman] += (endurance + intelligence) * 2;
    for (int i = 0; i < kSkillCount; ++i) {
        if (tagged[i]) skills[i] += 20;
    }

    if (maxActionPoints < 1) maxActionPoints = 1;
    if (maxHitPoints < 1) maxHitPoints = 1;
}

namespace {

// The binary `<attribs>` v3 payload: the token, then 197 little-endian int32,
// then a run of single bytes for the boolean traits/tags.  The index map is the
// one the data survey established by pairing binary files against the named
// copies embedded in actor prototypes, and MILESTONE 21 closed it against the
// three `.rdata` name tables the accessors walk: `Stats` at 0x008a6b70+0
// (indices 0..6), `Traits` at 0x008a6b70+7*4 (7..17, so 15 is `bonusAC` and 17
// is `race`), `Derived` at 0x008a41a0 (18..43, so 18 is `maxHitPoints` and 21
// `radiationResist`), `Skills` at 0x008a6b18 (44..61); then, as bytes from
// offset 248, the 18 skill tags followed by the 38 `OTraits` of the table at
// 0x008a42d0.  prefab1.ent (Mick) is the check: its bytes are 0, 4, 5 and 45,
// which reads as tagged smallGuns / meleeWeapons / throwing plus `Human`
// (45 - 18 = 27), and Human is what makes his 37 hit points come out.
bool ReadAttribsBinary(const std::vector<uint8_t>& payload, Attributes* out, bool additive) {
    // The payload is `i32 innerLen` then "<attribs>\0" "3\0" then the ints.
    size_t p = 0;
    if (payload.size() < 8) return false;
    // Skip a leading length if present.
    const char* kTok = "<attribs>";
    const size_t tokLen = std::strlen(kTok);
    size_t found = std::string::npos;
    for (size_t i = 0; i + tokLen < payload.size() && i < 32; ++i) {
        if (std::memcmp(&payload[i], kTok, tokLen) == 0) {
            found = i;
            break;
        }
    }
    if (found == std::string::npos) return false;
    p = found + tokLen + 1;         // past "<attribs>\0"
    while (p < payload.size() && payload[p] != '\0') ++p;   // the version string
    ++p;
    if (p + 197 * 4 > payload.size()) return false;
    const uint8_t* v = payload.data() + p;
    auto get = [&](int i) { return Int32LE(v + i * 4); };
    auto put = [&](int32_t& dst, int32_t val) { dst = additive ? dst + val : val; };

    if (!additive) {
        out->strength = get(0);  out->perception = get(1);  out->endurance = get(2);
        out->charisma = get(3);  out->intelligence = get(4); out->agility = get(5);
        out->luck = get(6);
    } else {
        out->strength += get(0);  out->perception += get(1);  out->endurance += get(2);
        out->charisma += get(3);  out->intelligence += get(4); out->agility += get(5);
        out->luck += get(6);
    }
    put(out->experience, get(7));
    put(out->skillPoints, get(8));
    put(out->perksToTake, get(11));
    put(out->bonusAc, get(15));
    put(out->raceIndex, get(17));
    put(out->maxHitPointsBonus, get(18));
    put(out->maxCarryWeight, get(19));
    put(out->maxActionPointsBonus, get(20));
    put(out->radResist, get(21));
    put(out->poisonResist, get(22));
    put(out->armorClass, get(23));
    put(out->criticalChance, get(24));
    put(out->fallover, get(25));
    put(out->healRate, get(39));
    put(out->meleeDamage, get(40));
    put(out->bonusDamage, get(41));
    put(out->skillPerLevel, get(42));
    put(out->levelsPerPerk, get(43));
    static const int kThreshIdx[kDamageTypeCount] = {26, 27, 28, 29, 30, 31};
    static const int kResistIdx[kDamageTypeCount] = {32, 33, 34, 35, 36, 37};
    for (int i = 0; i < kDamageTypeCount; ++i) {
        put(out->thresh[i], get(kThreshIdx[i]));
        put(out->resist[i], get(kResistIdx[i]));
    }
    for (int i = 0; i < kSkillCount; ++i) {
        put(out->skills[i], get(44 + i));
    }
    // The 18 tag booleans are the single bytes at payload offset 248, and the
    // 38 OTraits follow them at 266.
    const size_t tagBase = p + 248;
    for (int i = 0; i < kSkillCount; ++i) {
        if (tagBase + static_cast<size_t>(i) < payload.size() && payload[tagBase + i] != 0) {
            out->tagged[i] = true;
        }
    }
    const size_t otBase = tagBase + kSkillCount;
    for (int i = 0; i < kOTraitCount; ++i) {
        if (otBase + static_cast<size_t>(i) < payload.size() && payload[otBase + i] != 0) {
            out->otrait[i] = true;
        }
    }
    return true;
}

// The named encoding: child `<esh>` blocks.
void ReadNamedInt(const Esh& e, const char* name, int32_t* dst, bool additive) {
    int32_t v = 0;
    if (e.GetInt(name, &v)) {
        *dst = additive ? *dst + v : v;
    }
}

}  // namespace

bool ReadAttributes(const Esh& block, Attributes* out, bool additive) {
    bool any = false;
    bool binary = false;
    block.GetBool("Binary", &binary);
    // `levSincePerk` is a property of the `Attributes` block itself in both
    // encodings (the binary blob's `<attribs>` payload does not carry it).
    ReadNamedInt(block, "levSincePerk", &out->levSincePerk, additive);
    const ft::EshProp* bin = block.Find("esbin");
    if (bin != nullptr && !bin->payload.empty()) {
        if (ReadAttribsBinary(bin->payload, out, additive)) return true;
    }

    std::string err;
    Esh sub;
    if (block.GetNested("Stats", &sub, &err)) {
        any = true;
        ReadNamedInt(sub, "strength", &out->strength, additive);
        ReadNamedInt(sub, "perception", &out->perception, additive);
        ReadNamedInt(sub, "endurance", &out->endurance, additive);
        ReadNamedInt(sub, "charisma", &out->charisma, additive);
        ReadNamedInt(sub, "intelligence", &out->intelligence, additive);
        ReadNamedInt(sub, "agility", &out->agility, additive);
        ReadNamedInt(sub, "luck", &out->luck, additive);
    }
    if (block.GetNested("DerivedStats", &sub, &err)) {
        any = true;
        ReadNamedInt(sub, "armorClass", &out->armorClass, additive);
        ReadNamedInt(sub, "fallover", &out->fallover, additive);
        ReadNamedInt(sub, "meleeDamage", &out->meleeDamage, additive);
        ReadNamedInt(sub, "criticalChance", &out->criticalChance, additive);
        ReadNamedInt(sub, "maxHitPoints", &out->maxHitPointsBonus, additive);
        ReadNamedInt(sub, "maxActionPoints", &out->maxActionPointsBonus, additive);
        static const char* const kT[kDamageTypeCount] = {
            "normalThresh", "energyThresh", "fireThresh",
            "gasThresh", "explodeThresh", "electricalThresh"};
        static const char* const kR[kDamageTypeCount] = {
            "normalResist", "energyResist", "fireResist",
            "gasResist", "explodeResist", "electricalResist"};
        for (int i = 0; i < kDamageTypeCount; ++i) {
            ReadNamedInt(sub, kT[i], &out->thresh[i], additive);
            ReadNamedInt(sub, kR[i], &out->resist[i], additive);
        }
        // The rest of the 0x008a41a0 table.  `radiationResist` and
        // `poisonResist` are the shipped spellings -- brahmin, radscorp, lizard
        // and ldc all carry them.
        ReadNamedInt(sub, "radiationResist", &out->radResist, additive);
        ReadNamedInt(sub, "poisonResist", &out->poisonResist, additive);
        ReadNamedInt(sub, "maxCarryWeight", &out->maxCarryWeight, additive);
        ReadNamedInt(sub, "healRate", &out->healRate, additive);
        ReadNamedInt(sub, "bonusDamage", &out->bonusDamage, additive);
        ReadNamedInt(sub, "skillPerLevel", &out->skillPerLevel, additive);
        ReadNamedInt(sub, "levelsPerPerk", &out->levelsPerPerk, additive);
    }
    if (block.GetNested("Traits", &sub, &err)) {
        any = true;
        ReadNamedInt(sub, "bonusAC", &out->bonusAc, additive);
        ReadNamedInt(sub, "race", &out->raceIndex, additive);
        ReadNamedInt(sub, "experience", &out->experience, additive);
        ReadNamedInt(sub, "skillPoints", &out->skillPoints, additive);
        ReadNamedInt(sub, "perksToTake", &out->perksToTake, additive);
    }
    if (block.GetNested("OTraits", &sub, &err)) {
        any = true;
        for (int i = 0; i < kOTraitCount; ++i) {
            bool on = false;
            if (sub.GetBool(OTraitName(i), &on) && on) out->otrait[i] = true;
        }
    }
    if (block.GetNested("Skills", &sub, &err)) {
        any = true;
        for (int i = 0; i < kSkillCount; ++i) {
            ReadNamedInt(sub, kSkillNames[i], &out->skills[i], additive);
            bool tag = false;
            if (sub.GetBool(std::string("tg") + kSkillNames[i], &tag) && tag) {
                out->tagged[i] = true;
            }
        }
    }
    if (block.GetNested("Perks", &sub, &err)) {
        any = true;
        ReadNamedInt(sub, "sharpshooter", &out->sharpshooter, additive);
        // MILESTONE 18: the run rule's one perk read.
        ReadNamedInt(sub, "silentrunning", &out->silentRunning, additive);
        ReadNamedInt(sub, "bonusRangedDamage", &out->bonusRangedDamage, additive);
        ReadNamedInt(sub, "betterCriticals", &out->betterCriticals, additive);
        ReadNamedInt(sub, "bonusMove", &out->bonusMove, additive);
        ReadNamedInt(sub, "steadyArm", &out->steadyArm, additive);
        // MILESTONE 21: the four `SumFinalAttributes` / heal-arm perk reads.
        // The spellings are the executable's own (`adrenalineRush`, `hthEvade`,
        // `dieHard`, `healer` all appear verbatim in `.rdata`).
        ReadNamedInt(sub, "adrenalineRush", &out->adrenalineRush, additive);
        ReadNamedInt(sub, "hthEvade", &out->hthEvade, additive);
        ReadNamedInt(sub, "dieHard", &out->dieHard, additive);
        ReadNamedInt(sub, "healer", &out->healer, additive);
    }
    return any;
}

// ---------------------------------------------------------------------------
// Weapons and armour
// ---------------------------------------------------------------------------

bool WeaponDef::IsMelee() const {
    const WeaponMode* m = Mode();
    if (m == nullptr) return false;
    // Weapon::IsMeleeAttack @ 005d9260 tests the mode's weaponskill against
    // "meleeWeapons"; Weapon::GetMinDmg's melee branch also accepts "unarmed"
    // and "throwing".
    return IEquals(m->weaponSkill, "meleeWeapons") || IEquals(m->weaponSkill, "unarmed") ||
           IEquals(m->weaponSkill, "throwing");
}

bool WeaponDef::IsUnarmed() const {
    const WeaponMode* m = Mode();
    return m != nullptr && IEquals(m->weaponSkill, "unarmed");
}

int32_t WeaponDef::ModeIndexByName(const std::string& name) const {
    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].enabled && IEquals(name, modes[i].modeName.c_str())) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

namespace {
void ReadMode(const Esh& m, WeaponMode* out) {
    m.GetBool("Enable this mode", &out->enabled);
    // The mode's name is its `Display Name` -- "single", "burst", "swing",
    // "thrust", "triple", "jab", "kick", "sideKick".
    m.GetString("Display Name", &out->modeName);
    // The shipped files omit `Weapon Skill` on every gun mode; the omit-default
    // rule (mapview/NOTES.md 12) means the default applies, and the default the
    // AI and to-hit paths assume is smallGuns.
    if (!m.GetString("Weapon Skill", &out->weaponSkill)) out->weaponSkill = "smallGuns";
    m.GetString("Animation Name", &out->animation);
    m.GetString("Range Class", &out->rangeType);
    m.GetString("Spread (Arc)", &out->spreadType);
    m.GetString("Action Cost (type)", &out->actionCostType);
    m.GetString("Impact fx", &out->impactFx);
    m.GetString("Impact Sound FX", &out->soundFx);
    m.GetString("Light FX", &out->lightFx);
    m.GetFloat("Maximum Range", &out->maxRange);
    // MILESTONE 11 correction.  A mode that omits `Maximum Range` takes the
    // property's compile-time default, which is in no dump -- but the shipped
    // data settles it for the melee case: EVERY one of the 18 weapon modes
    // across all of `entities/weapons/` whose `Range Class` is Melee and which
    // states a Maximum Range states 2, and the ones that omit it (the
    // switchblade's swing, the dog's snap, every Punches/*.ent) are the ones
    // that would otherwise be unusable at any distance.  Without this a dog
    // could never bite and a knife could never cut: milestone 8 only ever
    // exercised firearms, which all state their range.
    if (out->maxRange <= 0.0f) {
        const bool melee = out->rangeType == "Melee" || out->rangeType == "Punch" ||
                           out->rangeType.empty();
        if (melee) out->maxRange = 2.0f;
    }
    m.GetFloat("Damage Multiplier", &out->damageMult);
    m.GetFloat("KnockOver Multiplier", &out->knockOverMult);
    m.GetInt("Accuracy Bonus", &out->accuracyBonus);
    m.GetInt("Ammo Usage", &out->ammoUsage);
    m.GetInt("Critical Chance", &out->critBonus);
    m.GetBool("Can Call Shot", &out->canCallShot);
    m.GetBool("Has Projectile", &out->hasProjectile);
    m.GetBool("Destroy On Use", &out->destroyOnUse);
    if (out->ammoUsage < 1) out->ammoUsage = 1;
}
}  // namespace

bool ReadWeapon(const Esh& esh, WeaponDef* out) {
    if (!esh.GetInt("Minimum Damage", &out->minDamage)) {
        // Explosives spell it differently.
        esh.GetInt("Min Damage", &out->minDamage);
    }
    if (!esh.GetInt("MaximumDamage", &out->maxDamage)) {
        esh.GetInt("Max Damage", &out->maxDamage);
    }
    esh.GetString("Weapon Item Type", &out->itemType);
    esh.GetString("Display Name", &out->displayName);
    esh.GetString("Type of ammo", &out->ammoType);
    esh.GetString("Default Ammo Variant", &out->ammoVariant);
    esh.GetString("Sound Type", &out->soundType);
    esh.GetString("Sprite", &out->spritePath);      // MILESTONE 18: the icon
    if (!esh.GetString("Default Base Damage Type", &out->primaryDamage)) {
        out->primaryDamage = "normal";
    }
    esh.GetString("Default Special Damage Type", &out->secondaryDamage);
    esh.GetInt("Magazine capacity", &out->magazine);
    esh.GetInt("Critical Chance", &out->criticalChance);
    esh.GetFloat("Weight", &out->weight);

    for (int i = 0; i < 6; ++i) {
        Esh m;
        std::string err;
        char name[16];
        std::snprintf(name, sizeof(name), "Mode[%d]", i);
        if (!esh.GetNested(name, &m, &err)) continue;
        WeaponMode mode;
        ReadMode(m, &mode);
        out->modes.push_back(std::move(mode));
    }
    // Weapon::SetWeaponModeByIndex's default is the first enabled mode.
    out->selectedMode = 0;
    for (size_t i = 0; i < out->modes.size(); ++i) {
        if (out->modes[i].enabled) {
            out->selectedMode = static_cast<int32_t>(i);
            break;
        }
    }
    out->valid = !out->modes.empty();
    return out->valid;
}

void AttackAnimSuffixes(const WeaponDef& w, std::string* weaponPart, std::string* modePart) {
    *weaponPart = w.itemType;
    // Unarmed weapons are "Punch"/"Kick" item types in the data but the sprite
    // carries StandAttackUnarmedOne / ...Two.
    if (IEquals(w.itemType, "Punch") || IEquals(w.itemType, "Kick") ||
        IEquals(w.itemType, "Unarmed")) {
        *weaponPart = "Unarmed";
    }
    const WeaponMode* m = w.Mode();
    if (m == nullptr) {
        modePart->clear();
        return;
    }
    *modePart = m->animation;
    if (modePart->empty()) {
        *modePart = m->modeName;
    }
    if (!modePart->empty()) {
        (*modePart)[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>((*modePart)[0])));
    }
}

bool ReadSlotEntity(const Esh& owner, const std::string& slot, ft::EntityBlock* out) {
    const ft::EshProp* p = owner.Find(slot);
    if (p == nullptr || p->payload.size() < 12) return false;
    // `i32 innerLength` then the document.  The length is checked rather than
    // trusted: an empty slot writes a zero-length payload.
    const int32_t innerLength = Int32LE(p->payload.data());
    if (innerLength <= 0 || static_cast<size_t>(innerLength) + 4 > p->payload.size()) {
        return false;
    }
    ft::Reader r(p->payload.data() + 4, static_cast<size_t>(innerLength));
    std::string err;
    return out->Read(r, &err);
}

bool ReadArmour(const Esh& esh, ArmourDef* out) {
    esh.GetString("Display Name", &out->displayName);
    esh.GetString("Item Type", &out->itemType);
    esh.GetString("Sprite", &out->spritePath);      // MILESTONE 18: the icon
    Esh effect;
    std::string err;
    Attributes a;
    if (esh.GetNested("Effect", &effect, &err)) {
        ReadAttributes(effect, &a, false);
    }
    out->armorClass = a.armorClass;
    for (int i = 0; i < kDamageTypeCount; ++i) {
        out->thresh[i] = a.thresh[i];
        out->resist[i] = a.resist[i];
    }
    for (int i = 0; i < kSkillCount; ++i) out->skills[i] = a.skills[i];
    out->valid = true;
    return true;
}

}  // namespace game
