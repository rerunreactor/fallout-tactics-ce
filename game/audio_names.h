// audio_names.h -- milestone 13: which file a sound event names.
//
// There is no sound table.  `grep -ril sound` over `tables/` finds nothing and
// no `.bos` carries one; the mapping is a FILENAME CONVENTION under
// `sound/game/`, assembled by string concatenation at the call site.  The one
// assembler that survives in the dumps is `Actor::PlayActorAttackSound
// @00562910` (`bos/game/entity/actor/actor-sound.cpp`, `Actor::PlayActorAttackSound@00562910`
// l.12124), which builds
//
//     "sound/game/" + <the entity's GetSoundType> + <the mode's attack type>
//                   + ".wav"
//
// literally, from `s_sound_game__0089fc10`, `GetSoundType`,
// `GetWeaponAttackTypeDisplayString` and `DAT_0089fc08` (".wav"), and picks
// among numbered variants with the MSVC LCG.
//
// Every other family below is read off the shipped 2040 names in
// `sound/Game/`, which fall into exactly the families this header declares.
// `Actor::SoundRelated1` -- the vtable slot the frame events -44/-41/-40 and
// the state changes in `Actor::DoEngineThink` call, and the function that would
// name the footstep, hit, death and fidget files -- is NOT in the 50 supplied
// functions, so the argument-to-family mapping here is a RECONSTRUCTION.  It is
// pinned by the data: every family's name set is complete and disjoint, and
// every combination this header can produce exists on disk.

#ifndef FOTGAME_AUDIO_NAMES_H
#define FOTGAME_AUDIO_NAMES_H

#include <string>

namespace ft {
namespace snd {

// `TileHeader.material` (+0x26 of a `<tile>` record, mapview/til.cpp l.43).
// The six values 0..5 cover 29,957 of the shipped tiles and map one-for-one
// onto the six footstep surface suffixes.  Established from the data:
// 9045/9196 tiles whose path contains "stone" are 0, 4821/4829 "gravel" are 1,
// 10556/10687 "metal" are 2, 3771/3967 "wood" are 3, 222/256 "water" are 4 and
// 808/808 "snow" are 5.
enum Surface {
    kSurfStone = 0,
    kSurfGravel = 1,
    kSurfMetal = 2,
    kSurfWood = 3,
    kSurfWater = 4,
    kSurfSnow = 5,
};
const char* SurfaceName(int material);

// The armour tier that appears in a footstep, a body-fall and an impact name.
// "" (none), "Leath", "Metal", "Pow" -- from the armour's `Item Type`
// ("Leather", "Metal", "Power", "Environmental", ...).
std::string ArmourTier(const std::string& armourItemType);
// The impact family's spelling of the same tier: Flesh / Leath / Metal / Pow.
std::string ImpactTier(const std::string& armourItemType);

// Normal or Heavy: the walker's build.  Only two footstep weight classes ship,
// and only the big frames (Power armour, super mutants, the huge raider and
// reaver frames) have the Heavy set.
std::string WeightClass(const std::string& spritePath);

// "Stand" or "Prone".  No Crouch footstep ships, so a crouching actor walks on
// the Stand set -- the only choice the data leaves.
std::string FootPosture(const std::string& posture);

// `sound/game/<Stand|Prone><Normal|Heavy><tier><Surface>` -- the stem, without
// the 1..4 variant number.  Ladder is a surface too (Stand only).
std::string Footstep(const std::string& posture, const std::string& spritePath,
                     const std::string& armourItemType, int material);

// The actor's own voice set: `NormalM1`..`NormalM8`, `NormalF1`..`F4`,
// `LightM1`..`M5`, `LightF1`/`F2`, `HeavyM1`..`M6`, `HeavyF1`, `GhoulM1`/`M2`,
// `MutantM1`/`M2`, and the critter and robot sets (`brahmin`, `dog`, `rat`,
// `wasp`, `scorpion`, `lizard`, `deathclaw`, `babdeathclaw`, `Wolf`, `BDC`,
// `SDC`, `RBThum`, ...).  `Actor::GetSoundType` is a vtable slot with no body
// in the dumps; this derives the set from the sprite (which is what carries
// the armour tier and the race) and picks the numbered variant from a hash of
// the actor's display name, so one actor keeps one voice for a whole mission.
// Returns the set name WITH its variant index, e.g. "NormalM3".
std::string ActorVoice(const std::string& spritePath, const std::string& displayName,
                       const std::string& armourItemType, int variantCount);
// The set name without the index -- "NormalM", "brahmin", "RBThum".
std::string ActorVoiceSet(const std::string& spritePath,
                          const std::string& armourItemType);
// True when the set is numbered (the humanoid voices) rather than a critter's
// single set.
bool VoiceSetIsNumbered(const std::string& set);

// The suffix `Actor::PlayActorAttackSound` appends: "single", "burst",
// "Reload", "OutOfAmmo".  A melee/unarmed weapon has no `Sound Type` sound of
// its own and falls back to the flesh impact.
std::string AttackStem(const std::string& weaponSoundType,
                       const std::string& modeName);
std::string ReloadStem(const std::string& weaponSoundType);
std::string OutOfAmmoStem(const std::string& weaponSoundType);

// `sound/game/<Flesh|Leath|Metal|Pow>_<Bullet|Laser|Plasma|Prod|Punch|Ripper|
// Spasm|Stab>[_Burst]` -- the weapon mode's `Impact Sound FX` value ("bullet",
// "laser", "esbin", ...) picks the second half.
// `damageType` is the weapon's `Default Base Damage Type` and is the fallback
// when the mode's `Impact Sound FX` is empty or is the shipped placeholder
// "esbin" (which most melee and several ranged modes carry).
std::string ImpactStem(const std::string& targetArmourItemType,
                       const std::string& impactSoundFx,
                       const std::string& damageType, bool melee, bool unarmed,
                       bool burst);

// `sound/game/<set><Death|Hit|Fidget|GetUp|Walk|Run|Magic|Pickup>` and the
// death variants `CutInHalf`, `Explode`, `Fire`, `Melt`, `Riddled`,
// `Electrify`, which are the `Death<type>` sequence suffixes milestone 8
// already stores in `LiveEntity::deathType`.
std::string DeathStem(const std::string& voiceSet, const std::string& deathType);
std::string HitStem(const std::string& voiceSet);
std::string FidgetStem(const std::string& voiceSet);

// `sound/game/bfall_<Normal|Heavy>_<Cloth|Leath|Metal|POW>`.
std::string BodyFallStem(const std::string& spritePath,
                         const std::string& armourItemType);

// A door's own sound family, from its sprite path: `sprites/doors/Wooden
// DOORS/Wood1Door.spr` -> "Wood1", which gives `Wood1Open` / `Wood1Close`.
// Falls back to "Metal".
std::string DoorStem(const std::string& spritePath, bool opening);

}  // namespace snd
}  // namespace ft

#endif  // FOTGAME_AUDIO_NAMES_H
