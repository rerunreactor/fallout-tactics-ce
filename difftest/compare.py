#!/usr/bin/env python3
"""Compare a hooked session of the original against fotgame.

    compare.py logs/session-<stamp>.log [--mission ...] [--squad a,b,c]
                                        [--limit N] [--min-count N]

The session log is written by difftest/ftse/fotlog.lua while the player
plays the original under Wine or Windows (see README.md).  Records:

    DIFF   difficulty= opponent_level_offset= friendly_fire= source=
    HIT    attacker= target= weapon= mode= mode_id= mode_shots= chance=
           raw_chance= location= inelig= gap_m= ax= ay= az= tx= ty= tz=
           astance= tstance=
    DMG    attacker= target= weapon= mode= mode_id= mode_shots= spread=
           mode_dmgmult= mode_range= location= damage= shots= rolls= crit=
           gap_m= ax= ay= az= tx= ty= tz= astance= tstance=
    CRIT   kind= attacker= target= weapon= location= chance= ax= ay= az=
           tx= ty= tz= astance= tstance=
    CRITFX / ATTACK / ENT / MOVE / VAR

MILESTONE 26 added `ax,ay,az` / `tx,ty,tz` (the attacker's and target's own
GetPosition(), the same call `gap_m` was already computed from) and, where
the vtable answers it, `astance`/`tstance` to HIT, CRIT and DMG.  A record
logged before that milestone carries none of the six position fields, and
`record_gap()` below is what every table asks for a record's own range
through, so an old session degrades to its previous (less precise) behaviour
instead of breaking.

Three comparisons, each against the port's own code and never against a formula
reimplemented here:

  ENT   the original's stat block for a creature against `fotgame --entities
        --stats`, joined on the internal name (CORE_raider) and split by the
        prototype path, because one internal name covers several .ent
        prototypes with different armour.
  HIT   records grouped by attacker, weapon, target and the RECORD'S OWN
        range (MILESTONE 26: from its positions, rounded to a tenth of a
        metre -- no longer a coarse 3-to-20-metre band), each group replayed
        through `fotgame --hitcalc`, which puts that attacker at that range
        from that target holding that weapon and reports every term of the
        port's sum.  One fotgame run answers every group.
  DMG   the per-shot pre-resistance rolls against the port's own damage band
        for the same weapon.

Names: the original logs display names (Mick, Raider); its own ENT records give
the internal name for each (CORE_prefab1, CORE_raider), and that is what joins
the two sides.  Weapon display names ("MP5 H&K") are resolved to .ent paths
through the game's locale table and its entity archive.
"""
import argparse
import math
import os
import re
import statistics
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = os.path.abspath(os.path.join(HERE, "..", "game"))
# The `core` folder of your own Fallout Tactics install.
ASSETS = os.environ.get("FOT_ASSETS", "core")
FIELD = re.compile(r"(\w+)=(\S+)")

DEFAULT_MISSION = "campaigns/missions/core/mission01.mis"
# Mick, Farsight and Stitch: the campaign squad the player had, so that a HIT
# record naming any of them has a counterpart to replay against.
DEFAULT_SQUAD = ("entities/Special/prefab/prefab1.ent,"
                 "entities/recruits/hf03.ent,entities/recruits/hm09.ent")


# --------------------------------------------------------------- the log

def parse_log(path):
    header, records = [], []
    for line in open(path, errors="ignore"):
        line = line.rstrip("\n")
        if line.startswith("--") or line.startswith("#"):
            header.append(line)
            continue
        parts = line.split(None, 1)
        if len(parts) != 2 or parts[0] not in (
                "HIT", "DMG", "CRIT", "CRITFX", "ATTACK", "ENT", "MOVE", "VAR",
                "DIFF"):
            continue
        rec = dict(FIELD.findall(parts[1]))
        # after the fields, never before: an ENT record carries its own `tag=`
        # field (the mission tag, "Grunt"), which would otherwise overwrite the
        # record kind and hide every ENT record from the whole comparison
        rec["tag"] = parts[0]
        records.append(rec)
    return header, records


def num(rec, key):
    try:
        return float(rec[key])
    except (KeyError, ValueError, TypeError):
        return None


# MILESTONE 26.  The exact centre-to-centre range for one record, from its own
# `ax,ay,az` / `tx,ty,tz` (fotlog.lua's GetPosition() on the attacker and the
# target) where they exist, falling back to the record's own `gap_m` -- which
# was computed from that same GetPosition() call inside fotlog.lua, just not
# carried out to Python -- for a session logged before MILESTONE 26 added
# them.  This is what turns HIT (and the CRIT/HIT pairing) from a comparison
# averaged over a coarse distance band into one that replays each record at
# its own range.
def record_gap(rec):
    ax, ay, az = num(rec, "ax"), num(rec, "ay"), num(rec, "az")
    tx, ty, tz = num(rec, "tx"), num(rec, "ty"), num(rec, "tz")
    if None not in (ax, ay, az, tx, ty, tz):
        return math.sqrt((ax - tx) ** 2 + (ay - ty) ** 2 + (az - tz) ** 2)
    return num(rec, "gap_m")


# ------------------------------------------------------- the port's side

def fotgame(args):
    exe = os.path.join(GAME_DIR, "fotgame")
    if not os.path.exists(exe):
        sys.exit("fotgame is not built: run `make native` in %s" % GAME_DIR)
    r = subprocess.run([exe] + args, cwd=GAME_DIR, capture_output=True, text=True)
    return r.stdout + r.stderr


DIFFICULTY = "normal"


def base_args(mission, squad):
    return [mission, "--assets", ASSETS, "--campaign-start", "--squad", squad,
            "--difficulty", DIFFICULTY]


def ours_entities(mission, squad):
    """fotgame's ENT records, keyed by (internal name, prototype path)."""
    out = {}
    for line in fotgame(base_args(mission, squad) + ["--entities", "--stats"]).splitlines():
        if line.startswith("ENT "):
            r = dict(FIELD.findall(line))
            out[(r.get("internal"), r.get("race"), r.get("ent"))] = r
    return out


# ------------------------------------------------------ weapon name map

def weapon_paths():
    """Display name -> .ent path, from the game's own locale and its own loader.

    The logger writes a weapon's DISPLAY name with spaces collapsed to
    underscores ("Pump-action_Shotgun", "MP5_H&K").  `locale/game/items.txt`
    maps `name_<key>` to that display string, and the KEY is the `.ent`'s own
    `Display Name` property -- not its file stem.  Guessing the stem is what
    used to lose `Switchblade` (`switchBlade.ent` is keyed `switchblade`) and
    `Beretta 470 Silverhawk` (`berettaShotgun.ent` is keyed `berrettaShotgun`,
    a misspelling in the shipped locale).  `fotgame --weapon-dump` reads the
    property out of every `.ent` under `entities/` with the game's own loader,
    so the join is exact.
    """
    names = {}
    loc = os.path.join(ASSETS, "locale/game/items.txt")
    if os.path.exists(loc):
        names = dict(re.findall(r"name_(\w+)\s*=\s*\{([^}]*)\}",
                                open(loc, errors="ignore").read()))
    by_key = {}
    for line in fotgame(["--assets", ASSETS, "--weapon-dump"]).splitlines():
        if not line.startswith("WEAPON "):
            continue
        r = dict(FIELD.findall(line.split("|")[0]))
        key, path = r.get("display"), r.get("ent")
        if not key or not path or key == "-":
            continue
        by_key.setdefault(key.lower(), path)
    out = {}
    for key, disp in names.items():
        path = by_key.get(key.lower())
        if path:
            out[disp.strip().replace(" ", "_")] = path   # logger collapses spaces
    return out


# --------------------------------------------------------------- ENT

def ent_table(records, ours):
    groups = defaultdict(list)
    for r in records:
        if r["tag"] == "ENT":
            groups[(r.get("internal", "?"), r.get("race", "?"))].append(r)
    if not groups:
        return []

    # `fotgame --entities` prints the mission TAG as `internal` for a tagged
    # actor ("Keeper (K)") where the original always prints the prototype's own
    # `Display Name` ("CORE_raider") and carries the tag separately, so the join
    # has to accept either.  Without this every tagged actor -- the elder, the
    # captives, the guards -- fell out of the comparison.
    # The join is (internal name, race): one internal name covers several `.ent`
    # prototypes, and the original's own records carry the race, which is what
    # separates a RaiderLarge from a Raider -- the two that disagree about
    # whether raiderArmor is worn at all.
    ours_by_internal = defaultdict(list)
    for (internal, race, ent), r in ours.items():
        ours_by_internal[(internal, race)].append((ent, r))

    print("\n=== ENT: creature stat blocks, the original against ours ===")
    print("%-28s %5s  %-24s  %-42s %s"
          % ("internal / race", "n", "original", "ours (prototype)", "delta"))
    deltas = []
    for (internal, race), rs in sorted(groups.items()):
        hps = sorted(x for x in (num(r, "hp") for r in rs) if x is not None)
        acs = sorted(set(int(x) for x in (num(r, "ac") for r in rs) if x is not None))
        aps = sorted(set(x for x in (num(r, "maxap") for r in rs)
                         if x is not None and x < 1000))
        # maxhp was garbage in sessions logged before the accessor fix, so it is
        # only shown when it is plausible
        mx = sorted(set(int(x) for x in (num(r, "maxhp") for r in rs)
                        if x is not None and x < 1000))
        left = "hp %g-%g%s ac %s ap %s" % (
            hps[0] if hps else 0, hps[-1] if hps else 0,
            "/%s" % ",".join(str(m) for m in mx) if mx else "",
            ",".join(str(a) for a in acs) if acs else "-",
            ",".join("%g" % a for a in aps) if aps else "-")
        cands = ours_by_internal.get((internal, race), [])
        if not cands:
            print("%-28s %5d  %-24s  %-42s %s"
                  % ("%s/%s" % (internal, race), len(rs), left,
                     "(not in this mission)", ""))
            continue
        for ent, u in cands:
            proto = ent.rsplit("/", 1)[-1] if ent else "-"
            right = "%-24s hp %s/%s ac %s ap %s" % (proto[:24], u.get("hp"),
                                                    u.get("maxhp"), u.get("ac"),
                                                    u.get("maxap"))
            d = []
            try:
                uac = int(u.get("ac", -1))
                if acs and uac not in acs:
                    d.append("ac")
                    deltas.append(("armour class",
                                   "%s/%s (%s): original %s, ours %d"
                                   % (internal, race, proto,
                                      ",".join(str(a) for a in acs), uac)))
                uhp = float(u.get("hp", -1))
                if hps and not (hps[0] <= uhp <= hps[-1]):
                    d.append("hp")
                    deltas.append(("hit points",
                                   "%s/%s (%s): original %g-%g, ours %g"
                                   % (internal, race, proto, hps[0], hps[-1], uhp)))
            except (TypeError, ValueError):
                pass
            print("%-28s %5s  %-24s  %-42s %s"
                  % ("%s/%s" % (internal, race), len(rs) if left else "", left,
                     right, ",".join(d) if d else "-"))
            left = ""      # the original's column is printed once per group
    return deltas


# --------------------------------------------------------------- HIT

def internal_of(records):
    """Display name -> internal name, from the session's own ENT records."""
    out = {}
    for r in records:
        if r["tag"] == "ENT" and r.get("name") and r.get("internal"):
            out.setdefault(r["name"], r["internal"])
    return out


# `chanceInfo.ineligible_flags`.  A HIT record is only a statement about the
# to-hit FORMULA when the engine considered the shot eligible: a non-zero
# `inelig` means the number on screen was produced for a shot the engine had
# already disqualified (out of range, no line of fire, not a legal target, the
# item in hand not a weapon at all), and comparing it to a computed chance
# compares two different questions.  Session 3 is 160,213 HIT records of which
# only 22,114 are eligible -- and 86,921 of the rest are one held First Aid Kit.
def eligible(rec):
    v = num(rec, "inelig")
    return v is not None and int(v) == 0


def hit_table(records, mission, squad, wmap, names, limit, min_count, tag="HIT",
              want_weapon=True):
    groups = defaultdict(list)
    skipped_null = skipped_inelig = 0
    for r in records:
        if r["tag"] != "HIT":
            continue
        if (r.get("weapon") in wmap) != want_weapon:
            continue
        gap, chance = record_gap(r), num(r, "chance")
        if gap is None or chance is None:
            continue
        if r.get("target") == "entity":
            # a null target: FTSE hands the hook the null entity, whose position
            # is the map origin, so gap_m is just the attacker's distance from
            # (0,0) and the chance is not about any creature
            skipped_null += 1
            continue
        if not eligible(r):
            skipped_inelig += 1
            continue
        # MILESTONE 26: keyed on the RECORD'S OWN range, rounded to a tenth of
        # a metre -- not a coarse 3-to-20-metre band -- exactly how dmg_table
        # already keys its groups.  A HIT record has one every frame a shot is
        # hovered, so this still groups (many identical hovers land on the
        # same key); it no longer averages together shots that were never at
        # the same range.
        groups[(r.get("attacker"), r.get("weapon"), r.get("target"),
                round(gap, 1))].append(r)

    rows = sorted(((k, v) for k, v in groups.items() if len(v) >= min_count),
                  key=lambda kv: -len(kv[1]))[:limit]
    if not rows:
        print("\nno %s groups big enough to replay" % tag)
        return []

    specs, meta = [], []
    for (att, wep, tgt, rgap), rs in rows:
        gap = statistics.median(sorted(record_gap(r) for r in rs))
        specs.append("%s,%s,%s,%.2f" % (names.get(att, att), names.get(tgt, tgt),
                                        wmap.get(wep, "-"), gap))
        meta.append(((att, wep, tgt, rgap), rs, gap))

    args = base_args(mission, squad)
    for s in specs:
        args += ["--hitcalc", s]
    out = {}
    for line in fotgame(args).splitlines():
        if line.startswith("HITCALC "):
            r = dict(FIELD.findall(line))
            out[r.get("spec")] = r

    print("\n=== %s: the original's chance against ours ===" % tag)
    if skipped_null:
        print("(%d records against a null target skipped -- see README.md)"
              % skipped_null)
    if skipped_inelig:
        print("(%d records with ineligible_flags set skipped -- not formula "
              "statements)" % skipped_inelig)
    print("%-9s %-16s %-9s %-8s %5s %6s %8s %6s %7s  %s"
          % ("attacker", "weapon", "target", "range m", "n", "gap", "original",
             "ours", "delta", "our terms"))
    deltas = []
    for spec, ((att, wep, tgt, rgap), rs, gap) in zip(specs, meta):
        o = [num(r, "chance") for r in rs]
        omean = statistics.mean(o)
        u = out.get(spec)
        label = ("%-9s %-16s %-9s %-8.2f %5d %6.2f %8.1f"
                 % (att[:9], wep[:16], tgt[:9], rgap, len(rs), gap, omean))
        if not u:
            print(label + " %6s %7s  no reply" % ("-", "-"))
            continue
        uc = float(u.get("chance", 0))
        terms = ("skill %s rc %s rangePen %s close %s ac %s dodge %s cover %s "
                 "diff %s dist %sm"
                 % (u.get("skill"), u.get("rc"), u.get("rangePen"), u.get("close"),
                    u.get("ac"), u.get("dodge"), u.get("cover"), u.get("difficulty",
                                                                      u.get("diff")),
                    u.get("dist")))
        if u.get("refused"):
            terms += " [refused]"
        placed = True
        try:
            # `--hitcalc` places the target so that the port's own distance
            # term equals the logged `gap_m`, which is what §136.1's `* 1.5`
            # was fitted against; `centre` is printed beside it and is always
            # about 1.5 m more, the two bounding boxes' half-extents.  The
            # check is on the term the formula uses.
            placed = abs(float(u.get("dist", 0)) - gap) <= 0.75
        except (TypeError, ValueError):
            placed = False
        if not placed:
            terms += (" [replay landed at %sm, not %.2f -- not counted]"
                      % (u.get("dist"), gap))
        print(label + " %6.0f %+7.0f  %s" % (uc, uc - omean, terms))
        if placed and not u.get("refused"):
            deltas.append((abs(uc - omean), att, wep, tgt, rgap, omean, uc, u, gap))
    return deltas


# --------------------------------------------------------------- DMG

def dmg_table(records, mission, squad, wmap, names):
    """Every DMG record's own rolls against the band the port would roll from.

    The band is asked for AT THE RECORD'S OWN RANGE, because a `Cone` spread
    weapon -- the shotguns -- has a range-dependent band, so one band per
    (attacker, weapon, target) is not enough: the same Pump-action Shotgun rolls
    38 at 0.9 m and 25 at 7.0 m, both legitimately.
    """
    dmgs = [r for r in records if r["tag"] == "DMG"]
    if not dmgs:
        print("\nno DMG records")
        return []
    # One --hitcalc per (attacker, weapon, target, range band): the band is what
    # the cone term needs, and grouping keeps one fotgame run enough.
    specs, keys = [], []
    seen = {}
    for r in dmgs:
        gap = record_gap(r)
        # MILESTONE 25: keyed on the record's OWN range, rounded to a tenth of
        # a metre, not on a four-metre band.  A `Cone` weapon's band is a
        # continuous function of the range (`2.5 - 2 * d / maxRange`), so
        # replaying a 6-10 m group at one midpoint compared rolls taken at
        # 6.0 m against a band computed for 8 m.  There are only tens of `DMG`
        # records in a session, so one --hitcalc each costs nothing.
        key = (r.get("attacker"), r.get("weapon"), r.get("target"),
               round(gap, 1) if gap is not None else 5.0, r.get("mode") or "-")
        if key in seen:
            continue
        mid = gap if gap is not None else 5.0
        # MILESTONE 25: the record NAMES the mode now (`mode=`, from
        # Weapon:GetCurrentMode():GetName()), so the band is asked for at the
        # mode the shot was actually fired in.  Sessions before that carry no
        # `mode=` field, and those fall back to the weapon's selected mode --
        # which is what left session 2's MP5 rolls (down to 8, below the
        # single-shot band's 9) and session 3's UZI rolls (up to 13, above the
        # burst band's 12) contradicting each other about the burst Damage
        # Multiplier.  `mode_shots` is the mode's Ammo Usage, the burst length.
        mode = r.get("mode") or "-"
        seen[key] = "%s,%s,%s,%.2f,%s" % (names.get(key[0], key[0]),
                                          names.get(key[2], key[2]),
                                          wmap.get(key[1], "-"), mid, mode)
    args = base_args(mission, squad)
    for spec in seen.values():
        args += ["--hitcalc", spec]
    bands = {}
    for line in fotgame(args).splitlines():
        if line.startswith("HITCALC "):
            r = dict(FIELD.findall(line))
            bands[r.get("spec")] = r

    groups = defaultdict(list)
    for r in dmgs:
        gap = record_gap(r)
        groups[(r.get("attacker"), r.get("weapon"), r.get("target"),
                round(gap, 1) if gap is not None else 5.0,
                r.get("mode") or "-")].append(r)

    print("\n=== DMG: per-shot rolls against the port's damage band ===")
    print("%-9s %-16s %-9s %-8s %-7s %4s %-22s %-14s %s"
          % ("attacker", "weapon", "target", "gap m", "mode", "n",
             "original rolls", "ours band", "final damage (orig)"))
    out = []
    for key in sorted(groups, key=lambda k: (k[0] or "", k[1] or "", k[2] or "")):
        rs = groups[key]
        spec = seen[key]
        rolls = []
        for r in rs:
            for tok in (r.get("rolls") or "").split("|"):
                try:
                    rolls.append(float(tok))
                except ValueError:
                    pass
        finals = [x for x in (num(r, "damage") for r in rs) if x is not None]
        b = bands.get(spec)
        band = ("%s-%s" % (b.get("dmg_min"), b.get("dmg_max"))) if b else "-"
        if b and b.get("ammomult") not in (None, "1"):
            band += "x" + b["ammomult"]
        print("%-9s %-16s %-9s %-8s %-7s %4d %-22s %-14s %s"
              % (key[0][:9], key[1][:16], key[2][:9], "%.1f" % key[3],
                 (key[4] or "-")[:7], len(rs),
                 ("%g-%g mean %.1f" % (min(rolls), max(rolls),
                                       statistics.mean(rolls))) if rolls else "-",
                 band,
                 ("%g-%g mean %.1f" % (min(finals), max(finals),
                                       statistics.mean(finals))) if finals else "-"))
        if rolls and b:
            try:
                lo, hi = float(b["dmg_min"]), float(b["dmg_max"])
                if min(rolls) < lo or max(rolls) > hi:
                    out.append(("weapon damage band",
                                "%s (%s) at %.1fm: original rolls %g-%g, "
                                "ours %g-%g"
                                % (key[1], key[4], key[3],
                                   min(rolls), max(rolls), lo, hi)))
            except (KeyError, ValueError):
                pass
    return out


# --------------------------------------------------------------- CRIT

def crit_table(records, ours):
    """The original's critical CHANCE against `RollCriticalHitChance @0060fdf0`.

    The port's rule is `trunc(hitChance / 10) + weapon.Critical Chance +
    mode.Critical Chance + derived.criticalChance`, and every term of it is in
    the session itself: the `HIT` record the engine computed immediately before
    the `CRIT` one carries the chance, and the attacker's own `ENT` block
    carries `crit_chance`.  So this needs no replay -- it is the port's formula
    against the original's numbers, record by record.
    """
    crits = [r for r in records if r["tag"] == "CRIT"]
    if not crits:
        return []
    # last HIT and last ENT seen for each name, walked in record order
    order = []
    for r in records:
        n = num(r, "n")
        order.append((n if n is not None else 0.0, r))
    order.sort(key=lambda kv: kv[0])
    last_hit, last_crit_stat, rows = {}, {}, []
    for _, r in order:
        if r["tag"] == "ENT" and r.get("name") and r.get("crit_chance"):
            last_crit_stat.setdefault(r["name"], set()).add(int(num(r, "crit_chance")))
        elif r["tag"] == "HIT":
            last_hit[(r.get("attacker"), r.get("target"), r.get("weapon"))] = r
        elif r["tag"] == "CRIT":
            # the engine computes the to-hit chance and then the critical
            # chance for the SAME shot, so the CRIT record's own input is the
            # HIT record immediately before it in the stream -- not the last
            # one that happened to name the same three entities, which is a
            # cursor hover from minutes earlier
            # the shot's OWN to-hit record: same attacker, target and weapon,
            # and within a few records -- the engine computes the chance, then
            # the critical chance, then the damage, so the pair is adjacent.
            # A cursor hover from minutes earlier names the same three and is
            # not the same shot, which is what the record-number window rejects.
            h = last_hit.get((r.get("attacker"), r.get("target"), r.get("weapon")))
            c = num(r, "chance")
            if h is not None and (num(r, "n") or 0) - (num(h, "n") or 0) > 4:
                h = None
            # MILESTONE 26: CRIT now carries its own positions, so the pairing
            # can be checked directly instead of trusting the record-number
            # window alone -- a hover produces a HIT record every frame, so a
            # HIT logged from a DIFFERENT range still fell inside the window
            # of four.  Only records with positions (both this milestone's)
            # are checked this way; an older session with no ax/tx on either
            # side falls back to the window alone, as before.
            if h is not None:
                rg, hg = record_gap(r), record_gap(h)
                if rg is not None and hg is not None and abs(rg - hg) > 0.05:
                    h = None
            if h is None or c is None:
                rows.append((r, None, None))
                continue
            hc = num(h, "chance")
            rows.append((r, hc, int(c) - int(hc // 10)))

    print("\n=== CRIT: RollCriticalHitChance against the original ===")
    print("%-9s %-16s %-9s %8s %6s %8s %6s %6s"
          % ("attacker", "weapon", "target", "hit%", "ENT crit", "original",
             "implied", "verdict"))
    bad = 0
    for r, hc, implied in rows:
        att = r.get("attacker", "?")
        stats = sorted(last_crit_stat.get(att, set()))
        if implied is None:
            print("%-9s %-16s %-9s %8s %6s %8s %6s %6s"
                  % (att[:9], r.get("weapon", "?")[:16], r.get("target", "?")[:9],
                     "-", "-", r.get("chance"), "-", "no HIT pair"))
            continue
        ok = implied in stats
        if not ok:
            bad += 1
        print("%-9s %-16s %-9s %8.0f %6s %8s %6d %6s"
              % (att[:9], r.get("weapon", "?")[:16], r.get("target", "?")[:9], hc,
                 ",".join(str(x) for x in stats) or "-", r.get("chance"), implied,
                 "ok" if ok else "MISMATCH"))
    print("%d of %d critical-chance records are exactly "
          "trunc(hitChance / 10) + the attacker's own crit_chance"
          % (len(rows) - bad, len(rows)))
    print("(the HIT record carries the chance for the SHOT the cursor is on; a "
          "burst\n resolves each of its shots at its own reduced chance, which "
          "only the ATTACK\n record's candidate list carries, so a burst weapon's "
          "CRIT records cannot be\n checked this way -- the single-shot ones can, "
          "and they are the ones that agree)")
    fx = [r for r in records if r["tag"] == "CRITFX"]
    for r in fx:
        print("CRITFX %s %s -> %s: uncapped roll %s (the effect table's own input)"
              % (r.get("kind"), r.get("attacker"), r.get("target"), r.get("roll")))
    return ([("critical chance", "%d of %d records disagree" % (bad, len(rows)))]
            if bad else [])


# --------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--mission", default=DEFAULT_MISSION)
    ap.add_argument("--squad", default=DEFAULT_SQUAD)
    ap.add_argument("--limit", type=int, default=24)
    ap.add_argument("--min-count", type=int, default=3)
    ap.add_argument("--difficulty", default="auto",
                    help="0..4 (the bos.cfg {game.difficulty} index) | easy | "
                         "normal | hard | <multiplier>: the "
                         "GetDifficultyModifierForAttackerTarget term the port "
                         "applies to the player's own shots.  The default, "
                         "`auto`, takes it from the session's own DIFF record "
                         "and falls back to normal when there is none.")
    a = ap.parse_args()
    global DIFFICULTY
    DIFFICULTY = a.difficulty

    header, records = parse_log(a.log)
    # MILESTONE 25.  The session records its own difficulty now: fotlog.lua's
    # OnStart reads `{game.difficulty}` out of `core/bos.cfg` and writes a DIFF
    # record, so the `GetDifficultyModifierForAttackerTarget @0061b990` term no
    # longer has to be guessed at from the residual.  An explicit --difficulty
    # on the command line still wins, for experiments.
    diffrec = next((r for r in records if r["tag"] == "DIFF"), None)
    if diffrec is not None:
        idx = diffrec.get("difficulty")
        if a.difficulty == "auto" and idx in ("0", "1", "2", "3", "4"):
            DIFFICULTY = idx
        print("difficulty: {game.difficulty} = %s from %s "
              "(opponentLevelOffset %s), using --difficulty %s"
              % (idx, diffrec.get("source"),
                 diffrec.get("opponent_level_offset"), DIFFICULTY))
    if DIFFICULTY == "auto":
        DIFFICULTY = "normal"
    kinds = defaultdict(int)
    for r in records:
        kinds[r["tag"]] += 1
    print("\n".join(header[:3]))
    print("\nrecords: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
    if not records:
        sys.exit("nothing to compare")

    wmap = weapon_paths()
    names = internal_of(records)
    ours = ours_entities(a.mission, a.squad)

    ent_deltas = ent_table(records, ours)
    hit_deltas = hit_table(records, a.mission, a.squad, wmap, names, a.limit,
                           a.min_count)
    # The same hook fires for a SKILL preview -- a held First Aid Kit is not a
    # weapon, and `Actor::PerformSkillUseCheck` is a different sum -- so those
    # records are tabulated apart instead of being averaged into the to-hit
    # deltas, where 86,921 of them at 3% swamped everything else.
    hit_table(records, a.mission, a.squad, wmap, names, a.limit, a.min_count,
              tag="SKILL previews (the item in hand is not a weapon)",
              want_weapon=False)
    dmg_deltas = dmg_table(records, a.mission, a.squad, wmap, names)
    crit_deltas = crit_table(records, ours)

    print("\n=== deltas, worst first ===")
    for absd, att, wep, tgt, rgap, omean, uc, u, gap in sorted(
            hit_deltas, key=lambda d: -d[0])[:8]:
        why = []
        if u.get("refused"):
            why.append("ours refuses the shot")
        try:
            if abs(float(u.get("dist", 0)) - gap) > 0.75:
                why.append("replay landed at %sm, not %.1fm" % (u.get("dist"), gap))
        except (TypeError, ValueError):
            pass
        why.append("ac %s" % u.get("ac"))
        if int(u.get("rangePen", 0)):
            why.append("rangePen %s" % u.get("rangePen"))
        if int(u.get("close", 0)):
            why.append("close %s" % u.get("close"))
        print("%+6.0f  %s / %s / %s at %.1fm: original %.0f, ours %.0f  [%s]"
              % (uc - omean, att, wep, tgt, rgap, omean, uc,
                 ", ".join(why)))
    seen = set()
    for what, detail in ent_deltas + dmg_deltas + crit_deltas:
        if (what, detail) not in seen:
            seen.add((what, detail))
            print("   %-18s %s" % (what, detail))


main()
