--[[ fotlog.lua -- differential-testing logger for the original, via FTSE 0.60a.

  Installed as <game>/core/FTSE/ftse.lua (see difftest/README.md).  Every hook
  is a pass-through: it logs and then returns exactly what the engine gave it,
  so the game plays as it always did.

  Records are one line each, in the format difftest/compare.py reads:

      TAG tick=<game ms> n=<record number> name=value name=value ...

  Values with spaces are not emitted; names are collapsed with underscores.
  The session file is named by the launcher in core/FTSE/logpath.txt; if that
  is missing, records go to fotlog.txt in the game directory.

  Tags: DIFF (the combat difficulty and level offset, once at OnStart),
        HIT (to-hit), CRIT (critical chance), CRITFX (critical effect roll),
        DMG (damage, with the per-shot rolls), ATTACK (an attack arc and the
        targets it considered), MOVE (coarse position, once per long tick),
        VAR (a mission or campaign variable write).

  MILESTONE 25.  Two additions, both to settle open questions:

    * every HIT and DMG record now names the weapon's SELECTED MODE
      (`mode`, `mode_id`, `mode_shots`, and on DMG also `spread`,
      `mode_dmgmult` and `mode_range`), through Weapon:GetCurrentMode() ->
      WeaponMode:GetName()/GetAmmoUsage()/GetSpreadType()/
      GetDamageMultiplier()/GetBaseMaxRange() and Weapon:GetSelectedModeID().
      Without the mode, a `DMG` record cannot say whether the burst mode's
      Damage Multiplier was in the roll -- session 2's MP5 rolls need it and
      session 3's UZI rolls forbid it, and neither can be settled by counting
      shots.  `mode_shots` is the mode's Ammo Usage, which IS the burst length.

    * a DIFF record at OnStart carrying `{game.difficulty}` and
      `{game.opponentLevelOffset}` out of `core/bos.cfg`.  FTSE exposes no
      config or world accessor for either, but the file is plain text beside
      the executable and Lua can read it -- which is enough, because
      `GetDifficultyModifierForAttackerTarget @0061b990` reads exactly that
      setting (`WorldGlobal->unknown4 + 0x24`) through a five-entry table.

  MILESTONE 26.  HIT, CRIT and DMG now carry the attacker's and target's own
  WORLD POSITIONS (`ax,ay,az` / `tx,ty,tz`, from the same GetPosition() the
  existing `gap_m` was already computed from) and, where the vtable answers
  it, their SELECTED STANCE (`astance`, `tstance`, Entity vtable id 236,
  `GetSelectedStance` -- FTSE has no named accessor).  `gap_m` alone was
  enough for the to-hit distance term but not for a genuine per-record
  replay, which is what settling 157.2's unexplained +2 needs.
--]]

local N = 0
local LOGFILE = nil

-- Where the session log goes, in order of preference:
--   1. whatever core/FTSE/logpath.txt names -- the Mac launcher writes an
--      absolute Wine path there;
--   2. logs/session-<stamp>.log relative to the game folder, which is what the
--      Windows kit uses: no absolute path, no Mac assumptions;
--   3. session-<stamp>.log in the game folder, if logs/ cannot be made.
local function logpath()
  local f = io.open("core/FTSE/logpath.txt", "r")
  if f then
    local p = f:read("*l")
    f:close()
    if p and p ~= "" and p ~= "-" then return p end
  end
  local stamp = os.date("%Y%m%d-%H%M%S")
  local rel = "logs/session-" .. stamp .. ".log"
  -- No os.execute here: under Wine it blocks (there is no shell in the prefix)
  -- and takes the whole script with it.  Both launchers create logs/ before
  -- starting the game; if it is missing, write beside the executable instead.
  local t = io.open(rel, "a")
  if t then t:close() return rel end
  return "session-" .. stamp .. ".log"
end

local function raw(line)
  if LOGFILE == nil then
    LOGFILE = logpath()
    -- FTSE.log always works, so say there which file was chosen and whether it
    -- could be opened; a session with no records is otherwise silent.
    if logger ~= nil then
      local probe = io.open(LOGFILE, "a")
      logger:log("fotlog: session log = " .. tostring(LOGFILE) ..
                 (probe and " (open ok)" or " (CANNOT OPEN)"))
      if probe then probe:close() end
    end
  end
  local f = io.open(LOGFILE, "a")
  if f then
    f:write(line, "\n")
    f:close()
  end
end

-- world:GetTime returns a TimeStamp TABLE (year/month/day/hour/minute/second/
-- msec), not a number: math.floor on it is what threw
-- "bad argument #1 to 'floor' (number expected, got table)" and stopped the
-- script.  Flatten it to milliseconds since the campaign's day zero.
local function num(x)
  if type(x) == "number" then return x end
  return 0
end

-- world:GetTime is only safe once a mission is running: calling it at script
-- load takes the PROCESS down (not a catchable Lua error), and calling it
-- during mission load returns a TimeStamp table, which is what threw
-- "bad argument #1 to 'floor' (number expected, got table)" in the player's
-- session.  So it is gated on a mission being up, and its table is flattened.
local time_ok = false

local function tick()
  if not time_ok then return 0 end
  local ok, t = pcall(function() return world:GetTime() end)
  if not ok or t == nil then return 0 end
  if type(t) == "number" then return math.floor(t) end
  if type(t) ~= "table" then return 0 end
  return math.floor(((num(t["day"]) * 24 + num(t["hour"])) * 60
                     + num(t["minute"])) * 60000
                    + num(t["second"]) * 1000 + num(t["msec"]))
end

-- a value safe to put after `name=`: no spaces, never empty
local function v(x)
  if x == nil then return "-" end
  local s = tostring(x)
  s = s:gsub("%s+", "_")
  if s == "" then return "-" end
  return s
end

local function nameof(e)
  if e == nil then return "-" end
  local ok, n = pcall(function() return e:GetName() end)
  if ok and n ~= nil and n ~= "" then return v(n) end
  local ok2, n2 = pcall(function() return e:GetInternalName() end)
  if ok2 and n2 ~= nil and n2 ~= "" then return v(n2) end
  return "entity"
end

local function pos(e)
  if e == nil then return nil end
  local ok, p = pcall(function() return e:GetPosition() end)
  if ok and p ~= nil then return p end
  return nil
end

-- the gap between two entities in metres, which is what the to-hit sum uses
local function gap(a, b)
  local pa, pb = pos(a), pos(b)
  if pa == nil or pb == nil then return nil end
  local dx = (pa["x"] or 0) - (pb["x"] or 0)
  local dy = (pa["y"] or 0) - (pb["y"] or 0)
  local dz = (pa["z"] or 0) - (pb["z"] or 0)
  return math.sqrt(dx * dx + dy * dy + dz * dz)
end

-- MILESTONE 26.  compare.py's per-record replay of HIT and CRIT (see 157.2's
-- unexplained +2) needs the two entities' own WORLD POSITIONS, not just the
-- scalar gap_m the records already carried: gap_m is enough for the to-hit
-- distance term, but a record-by-record replay wants the actual points so a
-- future geometry term (line of fire, elevation) is not thrown away at the
-- logging stage.  `posfields(e)` returns three formatted strings (or three
-- nils, if the entity or its position is unavailable), so a HIT/CRIT/DMG
-- record can splice `ax,ay,az` / `tx,ty,tz` in without a second GetPosition
-- call under a different pcall.
local function posfields(e)
  local p = pos(e)
  if p == nil then return nil, nil, nil end
  return string.format("%.3f", p["x"] or 0),
         string.format("%.3f", p["y"] or 0),
         string.format("%.3f", p["z"] or 0)
end

-- The attacker's / target's STANCE.  FTSE 0.60a exposes no named accessor for
-- it (there is no Actor:GetStance()), but the Entity Vtable list (manual
-- 05-Vtables.md) documents vtable id 236, `GetSelectedStance`, returning an
-- integer, callable on any entity through the generic `CallVtable` FTSE adds
-- to Entity -- "experimental" per that manual, and not every entity answers
-- it (a Weapon or a null target does not), so it is wrapped like everything
-- else that reaches into game state.
local VTABLE_GET_SELECTED_STANCE = 236

local function stance_of(e)
  if e == nil then return nil end
  local ok, s = pcall(function() return e:CallVtable(VTABLE_GET_SELECTED_STANCE) end)
  if ok and type(s) == "number" then return s end
  return nil
end

-- The weapon's SELECTED mode.  A `Weapon` answers GetCurrentMode() with a
-- WeaponMode object and GetSelectedModeID() with its index; anything else in
-- hand (a First Aid Kit through the same to-hit hook, a fist) answers neither,
-- and every call is wrapped so that costs nothing.
local function modeof(w)
  if w == nil then return nil end
  local ok, m = pcall(function() return w:GetCurrentMode() end)
  if not ok or m == nil then return nil end
  local function get(fn)
    local ok2, r = pcall(function() return m[fn](m) end)
    if ok2 then return r end
    return nil
  end
  local id = nil
  local oki, i = pcall(function() return w:GetSelectedModeID() end)
  if oki then id = i end
  return {
    name  = get("GetName"),
    id    = id,
    shots = get("GetAmmoUsage"),        -- the mode's Ammo Usage IS the burst length
    spread = get("GetSpreadType"),
    dmgmult = get("GetDamageMultiplier"),
    range = get("GetBaseMaxRange"),
  }
end

-- A hook that throws takes the whole script down with it -- FTSE unloads on a
-- Lua error -- so nothing here is allowed to escape.  record() never throws,
-- and each hook body runs under safe().
local function safe(f, fallback)
  local ok, r = pcall(f)
  if ok then return r end
  raw("-- lua error (handled): " .. tostring(r))
  return fallback
end

local function record(tag, ...)
  N = N + 1
  local n = select("#", ...)
  local parts = { tag, "tick=" .. tick(), "n=" .. N }
  for i = 1, n, 2 do
    parts[#parts + 1] = tostring(select(i, ...)) .. "=" .. v(select(i + 1, ...))
  end
  raw(table.concat(parts, " "))
end

raw("-- fotlog.lua via FTSE 0.60a, Lua " .. _VERSION)
raw("-- game: Fallout Tactics BOS.exe 1.27; hooks are pass-through observers")
raw("-- each record: TAG tick=<game ms> n=<record> then name=value")

-- Exercise record() -> tick() -> world:GetTime() once at load and once at
-- OnStart, so a type error in that path shows up in the first two lines of the
-- session log instead of sixteen seconds into the player's game.
-- `core/bos.cfg` is the game's own settings file, one `{key} = {value}` per
-- line, and it is where the combat difficulty lives.  FTSE has no accessor for
-- it (nothing in `world`, `hookexecutor` or `improver` reaches the config
-- block), but the file is plain text in the game directory and io.open reads
-- it the same way the logger finds its own log path.  `game.difficulty` is the
-- index `GetDifficultyModifierForAttackerTarget @0061b990` switches on
-- (0 -> 0.7, 1 -> 1.0, 2 -> 1.3, 3 -> 2.0, 4 -> 5.0, and the reciprocal is
-- taken when the ATTACKER is the player), and `game.opponentLevelOffset` is
-- the campaign-wide level bump
-- `Actor::HandleXPForNewPlayerCharacterMaybe @00577830` adds to an actor's
-- stored `Level` before granting the level-ups.
local function cfgvalue(key)
  for _, path in ipairs({ "core/bos.cfg", "bos.cfg", "../core/bos.cfg" }) do
    local f = io.open(path, "r")
    if f then
      for line in f:lines() do
        local v = line:match("^%s*{" .. key:gsub("%.", "%%.") .. "}%s*=%s*{(.-)}")
        if v then f:close() return v, path end
      end
      f:close()
    end
  end
  return nil, nil
end

function OnStart()
  raw("-- OnStart: FTSE engine up, no entities yet")
  safe(function()
    local diff, path = cfgvalue("game.difficulty")
    local off = cfgvalue("game.opponentLevelOffset")
    local ff = cfgvalue("game.friendlyFire")
    record("DIFF",
      "difficulty", diff,
      "opponent_level_offset", off,
      "friendly_fire", ff,
      "source", path
    )
  end)
end

function OnLocaleLoad()
  raw("-- OnLocaleLoad: string table built")
end

-- Forward declaration: OnDamageCalc below is written before the entity
-- snapshot block, and a Lua local has to exist when the closure that uses it is
-- created, not just when it runs.
local ent_record

-- ---------------------------------------------------------------- combat

-- Fires for the percentage under the cursor, for the sentry-mode eligibility
-- test and for the real attack roll, so a hover alone produces records.
function OnChanceToHitCalc(attacker, target, weapon, chanceInfo)
  safe(function()
  local d = gap(attacker, target)
  local m = modeof(weapon)
  local ax, ay, az = posfields(attacker)
  local tx, ty, tz = posfields(target)
  record("HIT",
    "attacker", nameof(attacker),
    "target", nameof(target),
    "weapon", nameof(weapon),
    "mode", m and m.name or nil,
    "mode_id", m and m.id or nil,
    "mode_shots", m and m.shots or nil,
    "chance", chanceInfo["hit_chance"],
    "raw_chance", chanceInfo["raw_chance"],
    "location", chanceInfo["location"],
    "inelig", chanceInfo["ineligible_flags"],
    "gap_m", d and string.format("%.3f", d) or nil,
    -- MILESTONE 26: the world positions per-record replay needs, plus the
    -- selected stance where the vtable answers it (see 157.2).
    "ax", ax, "ay", ay, "az", az,
    "tx", tx, "ty", ty, "tz", tz,
    "astance", stance_of(attacker), "tstance", stance_of(target)
  )
  end)
  return chanceInfo               -- the engine's own value, unmodified
end

local function crit(kind, attacker, target, weapon, loc, chance)
  safe(function()
  local ax, ay, az = posfields(attacker)
  local tx, ty, tz = posfields(target)
  record("CRIT",
    "kind", kind,
    "attacker", nameof(attacker),
    "target", nameof(target),
    "weapon", nameof(weapon),
    "location", loc,
    "chance", chance,
    -- MILESTONE 26: same positions/stance as HIT, so compare.py can replay a
    -- critical-chance record against the SAME geometry as its paired HIT
    -- record rather than an averaged one.
    "ax", ax, "ay", ay, "az", az,
    "tx", tx, "ty", ty, "tz", tz,
    "astance", stance_of(attacker), "tstance", stance_of(target)
  )
  end)
  return chance
end

function OnChanceToCritical1(a, t, w, loc, c) return crit("ranged", a, t, w, loc, c) end
function OnChanceToCritical2(a, t, w, loc, c) return crit("melee", a, t, w, loc, c) end

local function critfx(kind, attacker, target, weapon, loc, roll)
  safe(function()
  record("CRITFX",
    "kind", kind,
    "attacker", nameof(attacker),
    "target", nameof(target),
    "weapon", nameof(weapon),
    "location", loc,
    "roll", roll
  )
  end)
  return roll
end

function OnCriticalEffect1(a, t, w, loc, r) return critfx("ranged", a, t, w, loc, r) end
function OnCriticalEffect2(a, t, w, loc, r) return critfx("melee", a, t, w, loc, r) end

-- The per-shot pre-resistance rolls are the whole point of this one: they are
-- the damage model's input, before armour.
function OnDamageCalc(attacker, target, weapon, loc, damage, critTable, rollArray)
  safe(function()
  local rolls = {}
  if type(rollArray) == "table" then
    for i = 1, #rollArray do rolls[#rolls + 1] = tostring(rollArray[i]) end
  end
  local crits = {}
  if type(critTable) == "table" then
    for k, val in pairs(critTable) do
      if val == true or val == 1 then crits[#crits + 1] = tostring(k) end
    end
  end
  local m = modeof(weapon)
  local ax, ay, az = posfields(attacker)
  local tx, ty, tz = posfields(target)
  record("DMG",
    "attacker", nameof(attacker),
    "target", nameof(target),
    "weapon", nameof(weapon),
    -- MILESTONE 25: the mode is what settles whether the burst multiplier is
    -- in the roll, and `spread`/`mode_range` are what the Cone damage
    -- multiplier `2.5 - 2 * dist / range` is checked against.
    "mode", m and m.name or nil,
    "mode_id", m and m.id or nil,
    "mode_shots", m and m.shots or nil,
    "spread", m and m.spread or nil,
    "mode_dmgmult", m and m.dmgmult or nil,
    "mode_range", m and m.range or nil,
    "location", loc,
    "damage", damage,
    "shots", #rolls,
    "rolls", #rolls > 0 and table.concat(rolls, "|") or nil,
    "crit", #crits > 0 and table.concat(crits, "|") or "none",
    "gap_m", (function() local d = gap(attacker, target)
                return d and string.format("%.3f", d) or nil end)(),
    -- MILESTONE 26: positions/stance, same shape as HIT and CRIT.
    "ax", ax, "ay", ay, "az", az,
    "tx", tx, "ty", ty, "tz", tz,
    "astance", stance_of(attacker), "tstance", stance_of(target)
  )
  end)
  ent_record(target, "after_damage")
  return damage
end

-- The attack-arc hooks are replacements, not observers: whatever they return is
-- the shot distribution the engine uses.  These log the candidate list and then
-- hand back exactly what the engine proposed, so nothing changes.
local function arc(kind, attacker, shots, targets, weapon)
  safe(function()
  local out = {}
  local desc = {}
  if type(targets) == "table" then
    for i = 1, #targets do
      local t = targets[i]
      desc[#desc + 1] = string.format("%s:%s:%s:%s:%s",
        nameof(t["actor"]), tostring(t["hit_chance"]), tostring(t["distance"]),
        tostring(t["angle"]), tostring(t["intended"]))
      out[#out + 1] = { id = i, hits = 0, mult = 1, deltax = 0, deltaz = 0 }
    end
  end
  record("ATTACK",
    "kind", kind,
    "attacker", nameof(attacker),
    "weapon", nameof(weapon),
    "shots", shots,
    "candidates", #desc > 0 and table.concat(desc, ",") or "none"
  )
  end)
  return nil     -- nil = let the engine resolve the attack the way it always does
end

function OnStraightAttack(a, s, t, w) return arc("straight", a, s, t, w) end
function OnBurstAttack(a, s, t, w) return arc("burst", a, s, t, w) end
function OnSprayAttack(a, s, t, w) return arc("spray", a, s, t, w) end
function OnConeAttack(a, s, t, w) return arc("cone", a, s, t, w) end
function OnAreaAttack(a, s, t, w) return arc("area", a, s, t, w) end
function OnRadialAttack(a, s, t, w) return arc("radial", a, s, t, w) end
function OnProjectileAttack(a, s, t, w) return arc("projectile", a, s, t, w) end

-- --------------------------------------------------------- entity snapshot
--
-- ENT answers "what did the engine actually give this creature", which is the
-- question a stat the port derives itself cannot settle on its own: a brahmin's
-- current versus maximum hit points at spawn (the port reads 50/68, from a
-- maxHitPoints delta), a raider's armour class, a deathclaw's skills.
--
-- FTSE's Actor:GetAttribute takes the attribute name from the GAME'S OWN name
-- tables (FTSE/AttributesTable.cpp reads them out of .rdata at 0x8a6b70,
-- 0x8a6b8c, 0x8a41a0 and 0x8a6b18), and an unknown name silently resolves to
-- offset 0 -- which reads the entity's vtable pointer and is why the first
-- session logged 8539060 (0x824cb4) for four fields.  The spellings that exist:
--
--   stats    strength perception endurance charisma intelligence agility luck
--   traits   experience skillPoints tagsAvailable statsAvailable perksToTake
--            rank reputation age bonusAC sex race
--   derived  maxHitPoints maxCarryWeight maxActionPoints radiationResist
--            poisonResist armorClass criticalChance fallover *Thresh *Resist
--            camoflage healRate meleeDamage bonusDamage skillPerLevel
--            levelsPerPerk
--   skills   smallGuns bigGuns ... outdoorsman  (and tag_<skill>)
--
-- So it is maxHitPoints and maxActionPoints, never hitPoints/actionPoints.
-- The four tables are four copies of the same block; CURRENT is the effective
-- value (armour and bonuses folded in), PERM the character's own.
local T_PERM = ACTOR_TABLE_PERM or 0
local T_DERIVED = ACTOR_TABLE_DERIVED or 1
local T_TEMP = ACTOR_TABLE_TEMPORARY or 2
local T_CURRENT = ACTOR_TABLE_CURRENT or 3

local SPECIAL = { "strength", "perception", "endurance", "charisma",
                  "intelligence", "agility", "luck" }
local SKILLS = { "smallGuns", "bigGuns", "energyWeapons", "unarmed",
                 "meleeWeapons", "throwing", "firstAid", "doctor", "sneak",
                 "lockpick", "steal", "traps", "science", "repair", "pilot",
                 "barter", "gambling", "outdoorsman" }

local function attr(e, name, tab)
  local ok, val = pcall(function() return e:GetAttribute(name, tab) end)
  if ok and type(val) == "number" then return val end
  if ok and type(val) == "boolean" then return val and 1 or 0 end
  return nil
end

local function call0(e, fname)
  local ok, val = pcall(function() return e[fname](e) end)
  if ok and type(val) == "number" then return val end
  return nil
end

local function str0(e, fname)
  local ok, val = pcall(function() return e[fname](e) end)
  if ok and val ~= nil and val ~= "" then return val end
  return nil
end

-- what is in the two hands and the armour slot: this is what tells one raider
-- prototype from another, and it is where the armour class comes from
local SLOT_LEFT = EQUIP_SLOT_LEFT or 0
local SLOT_RIGHT = EQUIP_SLOT_RIGHT or 1
local SLOT_ARMOR = EQUIP_SLOT_ARMOR or 2

local function equipped(e, slot)
  local ok, item = pcall(function() return e:GetEquippedItem(slot) end)
  if not ok or item == nil then return nil end
  local n = str0(item, "GetInternalName") or str0(item, "GetName")
  if n == nil or n == "Entity" then return nil end
  return n
end

local seen_ent = {}

local function ent_key(e)
  return nameof(e) .. "/" .. tostring(e)
end

ent_record = function(e, why)
  if e == nil then return end
  safe(function()
    local sp, sk = {}, {}
    for i = 1, #SPECIAL do
      sp[#sp + 1] = tostring(attr(e, SPECIAL[i], T_CURRENT) or "-")
    end
    for i = 1, #SKILLS do
      sk[#sk + 1] = tostring(attr(e, SKILLS[i], T_CURRENT) or "-")
    end
    local p = pos(e)
    record("ENT",
      "why", why,
      "name", nameof(e),
      "internal", str0(e, "GetInternalName"),
      "class", str0(e, "GetEntityType"),
      "race", str0(e, "GetEntitySubType"),
      "id", call0(e, "GetBaseID"),
      "tag", str0(e, "GetTag"),
      -- hit points: live, effective maximum, and the character's own maximum
      "hp", call0(e, "GetHP"),
      "maxhp", attr(e, "maxHitPoints", T_CURRENT),
      "maxhp_perm", attr(e, "maxHitPoints", T_PERM),
      "maxhp_derived", attr(e, "maxHitPoints", T_DERIVED),
      -- armour class: effective, the character's own, and the trait bonus
      "ac", attr(e, "armorClass", T_CURRENT),
      "ac_perm", attr(e, "armorClass", T_PERM),
      "ac_derived", attr(e, "armorClass", T_DERIVED),
      "bonus_ac", attr(e, "bonusAC", T_CURRENT),
      -- action points
      "ap", call0(e, "GetAP"),
      "maxap", attr(e, "maxActionPoints", T_CURRENT),
      "maxap_perm", attr(e, "maxActionPoints", T_PERM),
      -- what it is holding and wearing, which is what separates prototypes
      "armour", equipped(e, SLOT_ARMOR),
      "hand_r", equipped(e, SLOT_RIGHT),
      "hand_l", equipped(e, SLOT_LEFT),
      "melee_dmg", attr(e, "meleeDamage", T_CURRENT),
      "crit_chance", attr(e, "criticalChance", T_CURRENT),
      "heal_rate", attr(e, "healRate", T_CURRENT),
      "special", table.concat(sp, ","),
      "skills", table.concat(sk, ","),
      "x", p and string.format("%.3f", p["x"] or 0) or nil,
      "z", p and string.format("%.3f", p["z"] or 0) or nil
    )
  end)
end

-- ------------------------------------------------- movement and variables

-- There is no movement hook in FTSE; OnLongTick is once per entity per ten
-- game seconds, which is enough for a coarse track.  Only moving entities are
-- logged, so a still map costs nothing.
local last = {}

function OnLongTick(ent)
  time_ok = true          -- entities are ticking, so the world clock is up
  safe(function()
    local key = ent_key(ent)
    local hp = call0(ent, "GetHP")
    if seen_ent[key] == nil then
      seen_ent[key] = hp or -1
      ent_record(ent, "spawn")
    elseif hp ~= nil and hp ~= seen_ent[key] then
      seen_ent[key] = hp
      ent_record(ent, (hp <= 0) and "death" or "hp_change")
    end
  end)
  safe(function()
  local p = pos(ent)
  if p == nil then return end
  local key = nameof(ent) .. "/" .. tostring(ent)
  local prev = last[key]
  local x, y, z = p["x"] or 0, p["y"] or 0, p["z"] or 0
  if prev == nil or math.abs(prev[1] - x) > 0.05 or math.abs(prev[3] - z) > 0.05 then
    record("MOVE",
      "actor", nameof(ent),
      "x", string.format("%.3f", x),
      "y", string.format("%.3f", y),
      "z", string.format("%.3f", z),
      "from_x", prev and string.format("%.3f", prev[1]) or nil,
      "from_z", prev and string.format("%.3f", prev[3]) or nil
    )
  end
  last[key] = { x, y, z }
  end)
end

function OnVariableChanged(key, value, isCampaign)
  safe(function()
    -- variables are written by the trigger engine, i.e. a mission is loading
    record("VAR", "key", key, "value", value, "campaign", tostring(isCampaign))
  end)
end
