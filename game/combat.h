// combat.h -- milestone 8: weapons, ammo, armour, to-hit, damage, criticals,
// action points and the turn queue.
//
// Everything here is ported from the shipped binary.  Two sources are used and
// both are named at every point of use:
//
//   * the Ghidra decompilation of the combat code.  Part of it is NEW in
//     this milestone: the
//     functions that carry the actual formulas (`CalculateHitChance`,
//     `ComputeWeaponAttackHitChance`, `ApplyRadiusDamageToTarget`,
//     `FinalizeCombatMessageFields`, `Tables::ActionTypes`,
//     `LookupDamageThresholdByName`, `LookupDamageResistanceByName`) were absent
//     from the dump this milestone started with and were exported from the
//     Ghidra project for it.  See NOTES.md section 19.
//   * the executable's own `.rdata`, read directly, for the numeric tables and
//     for every `_DAT_...` float Ghidra left unresolved.  Each table
//     below records the address it came from.
//
// Names in comments are `Function @ address`, the same convention the earlier
// milestones use.

#ifndef GAME_COMBAT_H
#define GAME_COMBAT_H

#include <cstdint>
#include <string>
#include <vector>

#include "esh.h"

namespace game {

using ft::Esh;

// ---------------------------------------------------------------------------
// The random number generator.
// ---------------------------------------------------------------------------
// Every combat roll in the game goes through one shared stream:
// `World_MaybeGetRandomizerObject(WorldGlobal)` returns a pointer to a single
// seed and each caller advances it in place with MSVC's `rand()` LCG --
// `RollCriticalHitChance @ 0060fdf0` is the
// clearest instance:
//     s = s * 0x343fd + 0x269ec3;  r = (s >> 16) & 0x7fff;
// `Random__GetRandomValueInFloatRange(rng, lo, hi)` is the float wrapper.
class Rng {
public:
    explicit Rng(uint32_t seed = 1) : m_state(seed) {}
    void Seed(uint32_t s) { m_state = s; }
    uint32_t State() const { return m_state; }

    // `(s >> 16) & 0x7fff`, MSVC rand().
    int32_t Rand15() {
        m_state = m_state * 0x343fdu + 0x269ec3u;
        return static_cast<int32_t>((m_state >> 16) & 0x7fff);
    }
    // The `% 100 + 1` percentile of RollCriticalHitChance.
    int32_t Percentile() { return Rand15() % 100 + 1; }
    // `Random__GetRandomValueInFloatRange`: uniform over [lo, hi].  The
    // normaliser is `_DAT_00808bb0 == 1/32767`, which the crit-miss path spells
    // out at `RollCriticalMissSeverity@0061d2a0`.
    float Range(float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(Rand15()) / 32767.0f);
    }
    // The `__ftol` the callers wrap every roll in: truncation toward zero.
    int32_t RangeI(float lo, float hi) { return static_cast<int32_t>(Range(lo, hi)); }

private:
    uint32_t m_state;
};

// ---------------------------------------------------------------------------
// Damage types.
// ---------------------------------------------------------------------------
// `Attributes::InitCopyAttributes @ 006080c0`
// enumerates exactly six threshold/resist pairs, and
// data/core/locale/game/misc.txt names the same six.  There is no separate
// laser/plasma/EMP: those are `energy`.
enum DamageType {
    kDmgNormal = 0,
    kDmgEnergy = 1,
    kDmgFire = 2,
    kDmgGas = 3,
    kDmgExplode = 4,
    kDmgElectrical = 5,
    kDamageTypeCount = 6,
};
DamageType DamageTypeFromName(const std::string& name);
const char* DamageTypeName(DamageType t);

// The 18 skills, in the order the binary `<attribs>` blob stores them
// (indices 44..61) and the order `Skills::FindSkillIndexByName @ 0060f4c0`
// walks its name table.
enum SkillId {
    kSkillSmallGuns = 0, kSkillBigGuns, kSkillEnergyWeapons, kSkillUnarmed,
    kSkillMeleeWeapons, kSkillThrowing, kSkillFirstAid, kSkillDoctor, kSkillSneak,
    kSkillLockpick, kSkillSteal, kSkillTraps, kSkillScience, kSkillRepair,
    kSkillPilot, kSkillBarter, kSkillGambling, kSkillOutdoorsman,
    kSkillCount,
};
int SkillIndexFromName(const std::string& name);
const char* SkillName(int index);

// The 38 optional traits, in the order of the `.rdata` pointer table at
// **`0x008a42d0`** (read out of `BOS_HR.exe`: `+0` -> `fastMetabolism`, ...,
// `+0x94` -> `doNightPerson`), which is both the name order
// `OTraits::GetOTraitValueByName` walks and the byte order the binary
// `<attribs>` blob stores them in, immediately after the 18 skill tags.  The
// six race flags are members of this list, not a separate field.
enum OTraitId {
    kOtFastMetabolism = 0, kOtBruiser, kOtSmallFrame, kOtOneHander, kOtFinesse,
    kOtKamikaze, kOtHeavyHanded, kOtFastShot, kOtBloodyMess, kOtJinxed,
    kOtGoodNatured, kOtChemReliant, kOtChemResistant, kOtNightPerson, kOtSkilled,
    kOtGifted, kOtGlowingOne, kOtTechWizard, kOtFearTheReaper, kOtVatSkin,
    kOtHamFisted, kOtDomesticated, kOtRabid, kOtTightNuts, kOtTargetingComputer,
    kOtBetaSoftware, kOtEmpShielding, kOtHuman, kOtGhoul, kOtMutant,
    kOtRobotHumanoid, kOtDeathclaw, kOtDog, kOtDoAdrenalineRush, kOtDoDieHard,
    kOtDoHthEvade, kOtDoDrunkenMaster, kOtDoNightPerson,
    kOTraitCount,
};
const char* OTraitName(int index);

// ---------------------------------------------------------------------------
// Character sheet.
// ---------------------------------------------------------------------------
struct Attributes {
    // SPECIAL, `<attribs>` indices 0..6.
    int32_t strength = 5, perception = 5, endurance = 5, charisma = 5;
    int32_t intelligence = 5, agility = 5, luck = 5;

    // DerivedStats.  `armorclass` is index 23, the thresholds 26..30 and the
    // resists 32..36 (gas has no threshold slot in the shipped layout and is
    // carried at index 29).
    int32_t armorClass = 0;
    int32_t thresh[kDamageTypeCount] = {0, 0, 0, 0, 0, 0};
    int32_t resist[kDamageTypeCount] = {0, 0, 0, 0, 0, 0};
    int32_t fallover = 0;
    int32_t meleeDamage = 0;
    int32_t criticalChance = 0;
    int32_t maxHitPointsBonus = 0;    // the .ent stores a delta, not the total
    int32_t maxActionPointsBonus = 0;
    // The rest of the `Derived` block, in the order of the `.rdata` name table
    // at **`0x008a41a0`** (`maxHitPoints`, `maxCarryWeight`, `maxActionPoints`,
    // `radiationResist`, `poisonResist`, `armorClass`, `criticalChance`,
    // `fallover`, the six thresholds, the six resists, `camoflage`, `healRate`,
    // `meleeDamage`, `bonusDamage`, `skillPerLevel`, `levelsPerPerk`) -- which
    // is also the binary `<attribs>` layout, starting at index 18.
    int32_t radResist = 0;
    int32_t poisonResist = 0;
    int32_t maxCarryWeight = 0;
    int32_t healRate = 0;
    int32_t bonusDamage = 0;
    int32_t skillPerLevel = 0;
    int32_t levelsPerPerk = 0;
    // `Traits`: the name table at `0x008a6b70` is `experience skillPoints
    // tagsAvailable statsAvailable perksToTake rank reputation age bonusAC sex
    // race`, indices 7..17 of the binary blob.  `bonusAC` is the one
    // `SumFinalAttributes` reads.
    int32_t bonusAc = 0;
    int32_t raceIndex = 0;
    // MILESTONE 25.  `Traits/experience` is blob index 7 and `skillPoints` 8,
    // `perksToTake` 11 -- the three the level-up grant
    // (`Attributes::CheckLevelUpAndGiveStatGainIfSo @00607b30`) writes.
    // `levSincePerk` is the perk countdown's residual (`(this->chems).unknown`
    // in that dump), stored as a property of the `Attributes` block itself.
    int32_t experience = 0;
    int32_t skillPoints = 0;
    int32_t perksToTake = 0;
    int32_t levSincePerk = 0;

    int32_t skills[kSkillCount] = {0};
    bool tagged[kSkillCount] = {false};
    bool otrait[kOTraitCount] = {false};

    // The two perks the to-hit and damage paths actually read.
    int32_t sharpshooter = 0;         // ComputeWeaponAttackHitChance, the range term
    int32_t bonusRangedDamage = 0;    // Weapon::GetMinDmg @ 005d9930
    int32_t betterCriticals = 0;      // Trap::ComputeMineDamageToLeg @ 005d1df0
    int32_t bonusMove = 0;            // Actor::Move @ 0055ba20, the AP divisor
    int32_t steadyArm = 0;            // Weapon::GetAPCost @ 005da3d0
    // MILESTONE 18: `perks.silentrunning`, the one perk the RUN rule reads
    // (every order site tests it before letting a sneaking actor run).  No
    // shipped Brahmin Wood actor has it; it is read, not invented.
    int32_t silentRunning = 0;
    // The four `SumFinalAttributes` reads, plus the one the heal arms read.
    int32_t adrenalineRush = 0;       // + strength, with doAdrenalineRush
    int32_t hthEvade = 0;             // + (hthEvade * unarmed) / 8 to AC
    int32_t dieHard = 0;              // + 10 * dieHard to every resist
    int32_t healer = 0;               // + 4 * healer hit points, both heal arms

    // Derived, computed by Derive().
    int32_t maxHitPoints = 0;
    int32_t maxActionPoints = 0;

    // MILESTONE 21.  The whole derivation, both halves:
    // `Attributes::InitCopyAttributes @ 006080c0` lays
    // the flat base and the race and optional-trait terms over the STORED sheet,
    // and `Attributes::SumFinalAttributes @ 006088d0` (:28861) adds the
    // SPECIAL-derived terms and finishes with
    // `Skills::AddStatBonusesToSkills @ 0060f320` (:31037).  Everything in the
    // `Derived` and `Skills` blocks of a `.ent` is a DELTA that this sums on top
    // of; only `Current Attributes` (the live hit points and action points) is
    // read back as a stored value.  Called once, on the sheet as read.
    void Derive();
};

// MILESTONE 25.  The experience ladder and the levelled-actor grant.
//
// `GetLevelForXPAmount @00607950` and `GetNeededXPForLevel @00607a90` walk one
// 15-entry `.rdata` table at `XPLevelValues @0x008a3e74`.
int LevelForExperience(int32_t xp);
int32_t ExperienceForLevel(int level);

// The `.mis` instance property `Level` is `Actor.ct_.npclevel`
// (`Actor::ClassType::Serialize@00570cb0` writes it as `s_Level_00895f1c` with default 0),
// and `Actor::HandleXPForNewPlayerCharacterMaybe @00577830` -- the
// `HandleXPForNewPlayerCharacterMaybe` vtable slot `Actor::InitEntityAfterSpawn
// @0056b9c0` calls once per actor at spawn -- turns it into levels:
//
//     needed = GetNeededXPForLevel(npclevel)
//     if (experience < needed)
//         CheckLevelUpAndGiveStatGainIfSo(base_stats, needed - experience, 0)
//         hp = maxhitpoints;  Actor::Levelup(oldMax);  npclevel = 0
//
// and `Attributes::CheckLevelUpAndGiveStatGainIfSo @00607b30` is the grant
// itself: PER LEVEL, `maxhitpoints += endurance / 2 + 3` (doubled by Brutish
// Hulk, plus `4 * lifegiver`), `skillpoints += skillperlevel`, and a perk slot
// every `levelsperperk` levels.  `RecalculateStatsFromXPLevel @00570920` states
// the same rule in closed form, `(endurance / 2 + 3) * (level - 1)`.
//
// Applied to the sheet AS READ, before `Derive()`; the endurance, skillPerLevel
// and levelsPerPerk it reads are the DERIVED ones (the dump reads them out of
// `this[1]` / `this[3]`, the summed blocks), so a derived copy is made here.
// Returns the number of levels granted.
int ApplyStoredLevel(Attributes* a, int npcLevel);

// `Actor::AutoAllocateLevelUpPoints @005775c0`, which `Actor::Levelup @00577280`
// runs for any actor with `npclevel > 0`, through
// `Attributes::SpendSkillPointsOnSkill @0060fb00`.  Only the FIRST of its three
// arms is deterministic -- every tagged skill is given `(skillPoints * 5) / 18`
// points -- and that is all this ports; the other two spend the remainder on
// `rand() % 18` skills off the shared stream, which is unreproducible (the
// function calls `time(NULL)` on the way in).  Run on the DERIVED sheet, where
// the skill percentages are already the effective ones the original's own
// summed copy tiers against.
void AutoAllocateLevelUpPoints(Attributes* a);
int SpendSkillPointsOnSkill(Attributes* a, int skill, int points);

// Reads an `Attributes` / `Requirements` / `Effect` block in either of the two
// encodings the shipped data uses: the named form (child `<esh>` blocks named
// Stats / Traits / DerivedStats / Skills / OTraits / Perks) and the binary form
// (`Binary (bool)=True` plus an `esbin` payload holding `<attribs>` v3 -- 197
// little-endian int32 then a run of bytes for the boolean traits).
// `additive` picks whether the values are absolute (an actor sheet) or deltas
// (an armour's Effect block).
bool ReadAttributes(const Esh& block, Attributes* out, bool additive);

// ---------------------------------------------------------------------------
// Weapons.
// ---------------------------------------------------------------------------
// The field names are Ghidra's own for the `WeaponMode` struct, which
// `Actor::Actor_HandleFireWeaponCommand @ 00546560`
// spells out one member at a time as it copies the active mode.
struct WeaponMode {
    bool enabled = false;
    std::string modeName;        // "single", "burst", "swing", ...
    std::string weaponSkill;     // absent in the data => "smallGuns"
    std::string animation;       // the sprite sequence suffix, e.g. "Burst"
    std::string rangeType;       // Melee | Short | Long | Scoped | OnlyLong | Thrown
    std::string spreadType;      // Straight | Burst | Cone | Radial | Spray
    std::string actionCostType;  // a key into the ActionTypes table below
    std::string impactFx, soundFx, lightFx;
    float maxRange = 0.0f;
    float damageMult = 1.0f;
    float knockOverMult = 0.0f;
    int32_t accuracyBonus = 0;
    int32_t ammoUsage = 1;
    int32_t critBonus = 0;
    bool canCallShot = false;
    bool hasProjectile = false;
    bool destroyOnUse = false;
};

struct WeaponDef {
    bool valid = false;
    std::string path, displayName, itemType;
    // MILESTONE 18: the item's own `Sprite` property, which is what
    // `WIIBase::Draw @00490b50` blits the `guibig` / `guismall` animation out
    // of when it draws the slot's icon.
    std::string spritePath;
    std::string ammoType, ammoVariant;
    std::string soundType;
    std::string primaryDamage = "normal";     // "Default Base Damage Type"
    std::string secondaryDamage;              // "Default Special Damage Type"
    int32_t minDamage = 1, maxDamage = 1;
    int32_t magazine = 0;
    int32_t criticalChance = 0;
    float weight = 0.0f;
    std::vector<WeaponMode> modes;
    int32_t selectedMode = 0;

    const WeaponMode* Mode() const {
        if (selectedMode < 0 || selectedMode >= static_cast<int32_t>(modes.size())) {
            return nullptr;
        }
        return &modes[static_cast<size_t>(selectedMode)];
    }
    bool IsMelee() const;      // Weapon::IsMeleeAttack @ 005d9260
    bool IsUnarmed() const;    // Weapon::IsUnarmedAttack @ 005d9210
    int32_t ModeIndexByName(const std::string& name) const;
};

struct ArmourDef {
    bool valid = false;
    std::string path, displayName, itemType;
    std::string spritePath;          // MILESTONE 18, as WeaponDef
    int32_t armorClass = 0;
    int32_t thresh[kDamageTypeCount] = {0, 0, 0, 0, 0, 0};
    int32_t resist[kDamageTypeCount] = {0, 0, 0, 0, 0, 0};
    // MILESTONE 25: an armour's `Effect` block is a whole `<attribs>` sheet and
    // it can carry SKILL deltas as well as derived rows -- `bosMetal.ent` states
    // `Effect/Skills/sneak = -25`, and the session logs the BOS Lieutenant who
    // wears it at sneak **-7**, which is his 18 plus that -25.  Nothing else in
    // Brahmin Wood carries one, but reading it costs nothing.
    int32_t skills[kSkillCount] = {0};
};

// Both read one `<entity>` block as it is stored inside another entity's
// `Hand1` / `Hand2` / `Armour` property (an esh type-4 payload).
bool ReadWeapon(const Esh& esh, WeaponDef* out);
bool ReadArmour(const Esh& esh, ArmourDef* out);

// An equipment slot's payload is `i32 innerLength` followed by a complete
// `<entity>` document -- the same framing esh.cpp's `GetNested` unwraps for
// type-11 records, but carried in a type-4 record.  `Esh::GetNested` refuses it
// because of the type check, so equipment goes through this instead.
bool ReadSlotEntity(const Esh& owner, const std::string& slot, ft::EntityBlock* out);

// The two suffixes `TryApplySequenceNameVariants @005e28a0` appends to "<Posture>Attack".  It tries (a, b), (a, ""), ("", b),
// ("", "") in that order and takes the first sequence that exists, which is why
// a weapon with no art falls back to the bare posture animation.
//   a = the weapon's `Weapon Item Type`  -- "SMG", "Pistol", "Club", ...
//   b = the mode's `Animation Name`, or its `Display Name` capitalised
// Checked against RaiderMale.spr, which carries exactly StandAttackSMGBurst,
// StandAttackPistolSingle, CrouchAttackClubSwing, StandAttackKnifeSlash and so
// on -- see NOTES.md section 21.
void AttackAnimSuffixes(const WeaponDef& w, std::string* weaponPart, std::string* modePart);

// ---------------------------------------------------------------------------
// The two tables the game keeps in `.rdata`, transcribed.
// ---------------------------------------------------------------------------
// `Tables::ActionTypes @ 0062f090` is a linear search
// over 41 {name, cost} records of 16 bytes at 0x008a8a38; the cost is the
// second dword.  It then adds `ActionPointCostAdjust(index) @ 0062f1a0`, which
// is a per-difficulty tweak and is zero at the default setting.
int32_t ActionTypeCost(const std::string& name);

// `LookupDamageThresholdByName @ 0062f9f0` and `LookupDamageResistanceByName
// @ 0062fad0` are the same linear search over 12
// {name, damageMult, drMod} records of 12 bytes at 0x008a8e60, returning the
// second and third field.  Both return 1.0 for an unknown name.  So an ammo
// variant carries no data of its own -- the .ent files store only the variant
// NAME, and these two floats are the whole of its effect.
struct AmmoVariant {
    float damageMult = 1.0f;   // multiplies the damage roll
    float drMod = 1.0f;        // scales the target's DT and DR
};
AmmoVariant AmmoVariantByName(const std::string& name);

// `FUN_0062f890 @ 0062f890` -- `table[GetBodyPartIndexByName(name) * 3]` at
// 0x008a8da4.  The called-shot to-hit penalty; an uncalled shot passes the
// empty string, which resolves to index 0 and a penalty of 0.
int32_t CalledShotToHitPenalty(const std::string& bodyPart);
int32_t CalledShotCritBonus(const std::string& bodyPart);

// ---------------------------------------------------------------------------
// Constants, all read out of the executable's .rdata.
// ---------------------------------------------------------------------------
namespace k {
constexpr float kRangePenaltyPerUnit = 4.0f;    // _DAT_007fd788
// MILESTONE 22.  Every distance term in `ComputeWeaponAttackHitChance` is
// computed on `dist * 1.5`, not on the metre gap: `ComputeWeaponAttackHitChance@00615e80` is
// `local_5c = (float)(fVar36 * (float10)_DAT_00809580)` and the eight bytes at
// 0x00809580 are the DOUBLE 1.5 (as floats they read 0.0 then 1.5f, which is
// how the constant was missed).  `local_5c` is then what the range penalty,
// the close bonus, the melee dodge and the fallen-target gate all compare.
// So the free range is `rangeClass * (PE + 2*Sharpshooter) / 1.5` metres and
// the penalty is 6 points per metre beyond it, not 4 -- which is the whole of
// the "short-range weapons fall off almost immediately" delta.
constexpr float kRangeUnitsPerMetre = 1.5f;     // _DAT_00809580, a double
constexpr float kCloseBonus = 30.0f;            // _DAT_0080afc0
constexpr float kCloseBonusRange = 6.0f;        // _DAT_0080a734
constexpr float kCloseBonusSlope = 1.0f / 6.0f; // _DAT_00824c78 == 0.166667
constexpr float kDifficultyScale = 40.0f;       // _DAT_0080afd4
// MILESTONE 25: the `Cone` spread's range-dependent damage multiplier, out of
// the binary.  `ApplyAreaDamageAttackWithFactionCheck @00615100` -- the arm
// `FinalizeCombatMessageFields @00611730` dispatches to when the mode's
// `Spread (Arc)` is `Cone` (the string at `0x0089f058`) -- computes
//     mult = 2.5 - 2 * (dist / Weapon::GetRange(weapon, attacker))
// at `0x00615605` (`fadd st(0),st(0); fsubr [0x0080c174]`, and `0x0080c174` is
// the float 2.5), where `dist` is the raw CENTRE-to-centre distance between
// attacker and target, and passes it as the second argument to
// `ApplyRadiusDamageToTarget @00613230` (`0x006156ff`).  Every other spread
// passes the literal 1.0 there (`0x613b1a`, `0x614a0b`, `0x61504d`), so the
// term is the cone's alone.  `CombatAI_EvaluateAttackApproachAndDamage
// @00612170` then clamps it to [0.2, 2.5] (`0x00612265`, the constants
// `_DAT_0080072c == 0.2` and `_DAT_0080c174 == 2.5`) and applies it as
//     minDmg = trunc(GetMinDmg(weapon, attacker) * mult)
//     maxDmg = trunc(GetMaxDmg(weapon, attacker) * mult)
// `Weapon::GetRange @005dbb00` is the selected mode's `Maximum Range` (for a
// Thrown mode, `min(GetThrowRange(attacker), maxRange)`).  Both shipped
// shotguns state 14, which is exactly why the 28 pooled rolls fitted
// `2.5 - gap_m / 7`: 2/14 is 1/7.
constexpr float kConeDamageMax = 2.5f;          // _DAT_0080c174
constexpr float kConeDamageMin = 0.2f;          // _DAT_0080072c
constexpr float kConeRangeTerms = 2.0f;         // fadd st(0),st(0)
constexpr float kMeleeDodgeRange = 3.0f;        // _DAT_0080a6ec
constexpr float kMeleeDodgeSlope = 1.0f / 3.0f; // _DAT_00809564 == 0.333333
constexpr float kMeleeDodgeScale = 0.33f;       // _DAT_0080a6dc
constexpr float kBigTargetVolume = 81.0f;       // _DAT_00824d00
constexpr int32_t kBigTargetBonus = 15;         // the literal 0xf
constexpr int32_t kChanceCap = 95;              // the literal 0x5f
constexpr int32_t kArmourClassCap = 80;         // the literal 0x50
constexpr float kMetresPerUnit = 0.25f;         // _DAT_007fd598
constexpr float kHalf = 0.5f;                   // _DAT_008bd194
constexpr float kEyeHeightFraction = 0.9f;      // _DAT_00804550, times kHalf
constexpr float kPercentToFraction = 0.01f;     // _DAT_00800730
constexpr float kBetterCriticals = 1.2f;        // _DAT_0080c160 (a double)
constexpr float kBonusRangedDamagePerRank = 0.15f;  // _DAT_008031c8
constexpr int32_t kBurstChanceCap = 70;         // the literal 0x46
constexpr float kMegaDamageBase = 4.0f;         // _DAT_0080c0c0 (a double)
constexpr float kMegaDamageRange = 100.0f;      // _DAT_0080c100 (a double)
// Actor::Move @ 0055ba20, the turn-based movement divisors.
constexpr float kHexPerApClimb = 1.0f;          // _DAT_00824ce8
constexpr float kHexPerApRun = 1.5f;            // _DAT_00824cd8
constexpr float kHexPerApStand = 1.5f;          // _DAT_00824cdc
constexpr float kHexPerApCrouch = 0.85f;        // _DAT_00824ce0
constexpr float kHexPerApProne = 0.65f;         // _DAT_00824ce4
constexpr float kBonusMovePerRank = 0.2f;       // _DAT_0080c188 (a double)
// World::StartNextCombatTurn @ 00507610: one whole round advances the world
// clock by World+0x2c5 seconds, split evenly between the queue's entries.
constexpr float kRoundSeconds = 7.0f;           // World+0x2c5, set by World::World
}  // namespace k

// ---------------------------------------------------------------------------
// Critical hits.
// ---------------------------------------------------------------------------
// The flag bits `CombatResultMessage::BuildCriticalEffectMessageKey @ 00622f90`
// names.
enum CritFlag {
    kCritBypassArmour = 0x01,
    kCritDoubleDamage = 0x02,
    kCritKnockOver = 0x04,
    kCritDropWeapon = 0x30,
    kCritKnockout = 0x40,
    kCritWeaponDamage = 0x80,
    kCritMegaDamage = 0x100,
};

// `RollCriticalEffectForBodyLocation @ 0061c550`: a cumulative table of
// byte[10][9] at 0x008a6c54, walked until the roll falls inside a bucket.  The
// table bytes are transcribed in combat.cpp.
int32_t RollCriticalEffect(Rng* rng, int32_t bodyLocation, int32_t roll);

// `RollCriticalMissSeverity @ 0061d2a0` -- the damage multiplier that comes out
// of the critical flags.  1 normally, 2 on DoubleDamage, and on MegaDamage a
// roll in [kMegaDamageBase, kMegaDamageBase + kMegaDamageRange] times that,
// forced to 4 for a player character.
int32_t CriticalDamageMultiplier(Rng* rng, int32_t critFlags, bool targetIsPlayer);

}  // namespace game

#endif  // GAME_COMBAT_H
