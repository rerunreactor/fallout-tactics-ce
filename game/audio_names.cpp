// audio_names.cpp -- see audio_names.h for what is ported and what is
// reconstructed.

#include "audio_names.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace ft {
namespace snd {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

bool Has(const std::string& hay, const char* needle) {
    return Lower(hay).find(Lower(needle)) != std::string::npos;
}

std::string Base(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = b.find_last_of('.');
    if (dot != std::string::npos) {
        b = b.substr(0, dot);
    }
    return b;
}

// A stable per-actor index.  The original picks the voice once, when the actor
// is built, and stores it; nothing in the dumps says how, so this is a hash of
// the display name -- stable across a mission and across a save round trip,
// which is the property that matters.
uint32_t Hash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

}  // namespace

const char* SurfaceName(int material) {
    switch (material) {
        case kSurfStone: return "Stone";
        case kSurfGravel: return "Gravel";
        case kSurfMetal: return "Metal";
        case kSurfWood: return "Wood";
        case kSurfWater: return "Water";
        case kSurfSnow: return "Snow";
        default: return "Stone";
    }
}

std::string ArmourTier(const std::string& itemType) {
    if (itemType.empty()) {
        return "";
    }
    if (Has(itemType, "power")) {
        return "Pow";
    }
    if (Has(itemType, "metal")) {
        return "Metal";
    }
    // Leather, Raider, Reaver, Ghoul, Mutant, Environmental: all the soft ones.
    return "Leath";
}

std::string ImpactTier(const std::string& itemType) {
    const std::string tier = ArmourTier(itemType);
    return tier.empty() ? "Flesh" : tier;
}

std::string WeightClass(const std::string& spritePath) {
    const std::string b = Base(spritePath);
    // The Heavy footstep set belongs to the big frames.  Power armour, the
    // super mutants and the huge/large raider and reaver frames are the only
    // shipped sprites that read as heavy.
    if (Has(b, "power") || Has(b, "mutant") || Has(b, "huge") || Has(b, "large") ||
        Has(b, "metal") || Has(b, "deathclaw") || Has(b, "behemoth")) {
        return "Heavy";
    }
    return "Normal";
}

std::string FootPosture(const std::string& posture) {
    // No Crouch footstep ships.
    return posture == "Prone" ? "Prone" : "Stand";
}

std::string Footstep(const std::string& posture, const std::string& spritePath,
                     const std::string& armourItemType, int material) {
    return "sound/game/" + FootPosture(posture) + WeightClass(spritePath) +
           ArmourTier(armourItemType) + SurfaceName(material);
}

std::string ActorVoiceSet(const std::string& spritePath,
                          const std::string& armourItemType) {
    const std::string b = Base(spritePath);
    // The critters and robots first: they have one set each, named after the
    // creature, with no tier or gender.
    struct Row {
        const char* sprite;
        const char* set;
    };
    static const Row kCreatures[] = {
        {"TwoHeadedBrahmin", "brahmin"}, {"BrahminTippable", "brahmin"},
        {"MutantDog", "dog"},            {"GiantRat", "rat"},
        {"RadScorpion", "scorpion"},     {"MutantLizard", "lizard"},
        {"DeathClawBaby", "babdeathclaw"}, {"DeathClawEgg", "deathclawegg"},
        {"DeathClaw", "deathclaw"},      {"Wasp", "wasp"},
        {"Wolf", "Wolf"},                {"BDC", "Bdc"},
        {"SDC", "Sdc"},                  {"MDC", "Bdc"},
        {"Humanoid", "RBThum"},          {"LoadLifter", "RBTload"},
        {"Pacification", "RBTpac"},      {"Hover", "RBThov"},
        {"Scurry", "RBTscur"},           {"Security", "RBTsec"},
        {"Behemoth", "RbtBeh"},          {"Turret", "Turret"},
    };
    for (const Row& r : kCreatures) {
        if (Has(b, r.sprite)) {
            return r.set;
        }
    }
    // Then the humanoid voices: <tier><gender>, tier from the armour the sprite
    // itself depicts (the character sprite IS the armour in this game --
    // `LeatherMale.spr`, `MetalFemale.spr`, `Power.spr`).
    if (Has(b, "ghoul")) {
        return "GhoulM";
    }
    if (Has(b, "mutant")) {
        return "MutantM";
    }
    std::string tier = "Normal";
    if (Has(b, "power")) {
        tier = "Heavy";
    } else if (Has(b, "metal")) {
        tier = "Heavy";
    } else if (Has(b, "leather")) {
        tier = "Light";
    } else if (!armourItemType.empty()) {
        const std::string a = ArmourTier(armourItemType);
        tier = a == "Pow" || a == "Metal" ? "Heavy" : (a == "Leath" ? "Light" : "Normal");
    }
    const std::string gender = Has(b, "female") ? "F" : "M";
    return tier + gender;
}

bool VoiceSetIsNumbered(const std::string& set) {
    static const char* kNumbered[] = {"NormalM", "NormalF", "LightM", "LightF",
                                      "HeavyM",  "HeavyF",  "GhoulM", "MutantM"};
    for (const char* s : kNumbered) {
        if (set == s) {
            return true;
        }
    }
    return false;
}

std::string ActorVoice(const std::string& spritePath, const std::string& displayName,
                       const std::string& armourItemType, int variantCount) {
    const std::string set = ActorVoiceSet(spritePath, armourItemType);
    if (!VoiceSetIsNumbered(set) || variantCount <= 1) {
        return VoiceSetIsNumbered(set) ? set + "1" : set;
    }
    return set + std::to_string((int)(Hash(displayName + set) % (uint32_t)variantCount) + 1);
}

std::string AttackStem(const std::string& weaponSoundType,
                       const std::string& modeName) {
    if (weaponSoundType.empty()) {
        return "";
    }
    // `Actor::PlayActorAttackSound` concatenates the sound type and the mode's
    // attack-type display string.  The shipped names use "single" and "burst"
    // (with varied capitalisation, which the case-insensitive resolve absorbs);
    // every other mode name -- swing, thrust, slash, throw, punch, kick -- has
    // no per-weapon file and is left to the impact sound.
    const std::string m = Lower(modeName);
    if (m.find("burst") != std::string::npos || m.find("spray") != std::string::npos) {
        return "sound/game/" + weaponSoundType + "burst";
    }
    if (m.find("single") != std::string::npos || m.find("aimed") != std::string::npos ||
        m.find("snap") != std::string::npos || m.find("shot") != std::string::npos) {
        return "sound/game/" + weaponSoundType + "single";
    }
    return "sound/game/" + weaponSoundType + "single";
}

std::string ReloadStem(const std::string& weaponSoundType) {
    return weaponSoundType.empty() ? "" : "sound/game/" + weaponSoundType + "reload";
}

std::string OutOfAmmoStem(const std::string& weaponSoundType) {
    return weaponSoundType.empty() ? "sound/game/outofammo"
                                   : "sound/game/" + weaponSoundType + "outofammo";
}

std::string ImpactStem(const std::string& targetArmourItemType,
                       const std::string& impactSoundFx,
                       const std::string& damageType, bool melee, bool unarmed,
                       bool burst) {
    std::string fx = Lower(impactSoundFx);
    // "esbin" is the placeholder value the shipped `.ent` files carry where no
    // impact sound was authored; it has no family of its own, so the weapon's
    // damage type and its melee/unarmed flags decide instead.
    if (fx.empty() || fx == "esbin" || fx == "none") {
        if (unarmed) {
            fx = "punch";
        } else if (melee) {
            fx = "stab";
        } else {
            fx = Lower(damageType);
        }
    }
    const char* kind = "Bullet";
    if (fx.find("laser") != std::string::npos) {
        kind = "Laser";
    } else if (fx.find("plasma") != std::string::npos ||
               fx.find("fire") != std::string::npos) {
        kind = "Plasma";
    } else if (fx.find("prod") != std::string::npos ||
               fx.find("electric") != std::string::npos ||
               fx.find("shock") != std::string::npos) {
        kind = "Prod";
    } else if (fx.find("punch") != std::string::npos ||
               fx.find("kick") != std::string::npos ||
               fx.find("fist") != std::string::npos) {
        kind = "Punch";
    } else if (fx.find("ripper") != std::string::npos) {
        kind = "Ripper";
    } else if (fx.find("spasm") != std::string::npos) {
        kind = "Spasm";
    } else if (fx.find("stab") != std::string::npos ||
               fx.find("knife") != std::string::npos ||
               fx.find("spear") != std::string::npos ||
               fx.find("blade") != std::string::npos) {
        kind = "Stab";
    }
    std::string s = "sound/game/" + ImpactTier(targetArmourItemType) + "_" + kind;
    if (burst) {
        s += "_Burst";
    }
    return s;
}

std::string DeathStem(const std::string& voiceSet, const std::string& deathType) {
    if (voiceSet.empty()) {
        return "";
    }
    // `LiveEntity::deathType` is the suffix of the Death<type> sequence the
    // damage type selected -- "", "CutInHalf", "Explode", "Fire", "Melt",
    // "Riddled", "Electrify".  The sound families use exactly those words.
    if (deathType.empty()) {
        return "sound/game/" + voiceSet + "Death";
    }
    return "sound/game/" + voiceSet + deathType;
}

std::string HitStem(const std::string& voiceSet) {
    return voiceSet.empty() ? "" : "sound/game/" + voiceSet + "Hit";
}

std::string FidgetStem(const std::string& voiceSet) {
    return voiceSet.empty() ? "" : "sound/game/" + voiceSet + "Fidget";
}

std::string BodyFallStem(const std::string& spritePath,
                         const std::string& armourItemType) {
    std::string tier = ArmourTier(armourItemType);
    if (tier.empty()) {
        tier = "Cloth";
    } else if (tier == "Pow") {
        tier = "POW";
    }
    return "sound/game/bfall_" + WeightClass(spritePath) + "_" + tier;
}

std::string DoorStem(const std::string& spritePath, bool opening) {
    const std::string b = Base(spritePath);
    const char* family = "Metal";
    struct Row {
        const char* needle;
        const char* family;
    };
    static const Row kRows[] = {
        {"BOS InteriorDoor1", "BOSInterior1"}, {"BOS InteriorDoor2", "BOSInterior2"},
        {"BOS InteriorDoor3", "BOSInterior3"}, {"WoodDoor1", "Wood1"},
        {"WoodDoor2", "Wood2"},                {"WoodDoor3", "Wood3"},
        {"PWT WoodDoor", "Wood1"},             {"Flyscreen", "Flyscreen"},
        {"WireFence", "WireFence"},            {"Junk", "Junk"},
        {"Mis4Gate", "Mis4Gate"},              {"JailCell", "JailCell"},
        {"RaiderMainGate", "RaiderMainGate"},  {"RobotInterior", "RobotInterior"},
        {"RobotEnergy", "RobotEnergy"},        {"RobotFortified", "RobotFortified"},
        {"RobotHuge", "RobotHuge"},            {"VaultEntrance", "VaultEntrance"},
        {"VaultInterior", "VaultInterior"},    {"TrapDoor", "TrapDoor"},
        {"Fence", "Fence"},                    {"Locker", "Locker"},
        {"Chest", "Chest"},                    {"Safe", "Safe"},
        {"Wood", "Wood1"},
    };
    for (const Row& r : kRows) {
        if (Has(b, r.needle)) {
            family = r.family;
            break;
        }
    }
    return std::string("sound/game/") + family + (opening ? "Open" : "Close");
}

}  // namespace snd
}  // namespace ft
