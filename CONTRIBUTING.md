# Contributing

Thanks for helping rebuild Fallout Tactics. This page covers what you need, how engine work is done here, and the legal side.

## What you need

- **Your own copy of Fallout Tactics: Brotherhood of Steel 1.27**, from Steam or GOG. Steam ships both `BOS.exe` and the hi-res `BOS_HR.exe`; GOG ships `BOS.exe`. This repository never holds any part of the game.
- **A C++17 compiler and CMake.** See the [README](README.md) for build commands.
- **For reverse-engineering work:** Ghidra 12, plus the [kit](kit/README.md), which loads our function names and types into your own copy of the executable.

## Ways to help

- **Engine work:** port or fix behavior against the original. See the [porting guide](docs/porting-guide.md) and the open items in [docs/status.md](docs/status.md).
- **Checking against the original:** record sessions of the original game with the [difftest kit](difftest/README.md) and report the differences.
- **The kit:** better names, types and file-format notes.
- **Builds:** get the build working well on your platform, and keep CI green.
- **Bug reports:** include a save if you can.

## How engine work is done

- **Match the original's behavior, not just its look.** Cite the original function in a comment as `Name@address`, for example `ComputeWeaponAttackHitChance@00615e80`. The kit says which names are verified for which executable.
- **Never paste decompiler output.** Read it, work out what it does, and write it as clean C++17 like the rest of the engine. Mentioning a field offset or a constant is fine.
- **Say when the original doesn't settle something.** Label the code RECONSTRUCTION or PLACEHOLDER, as the existing code does, and say what it's based on.
- **Keep pull requests focused:** one behavior per pull request where you can.

## Citations you can't follow

Much of the engine was written from private development notes, which aren't published. Comments that cite them look like this:

- `NOTES.md §72`, `mapview/NOTES.md 25.13`, `VIDEO_GAPS M4`: sections of those notes.
- `f0348`: a frame of a reference recording of the original game, used for comparison.
- `l.12345`: a line in the maintainers' Ghidra export of the functions the port was written from.

The conclusions are in the comments themselves. If you rework one of those comments, replace the citation with evidence anyone can check: a function name and address, a file-format detail, or a measurement you describe.

## Testing

- With `FOT_DATA_DIR` pointing at your game's `core` folder, `ctest` runs the data tests. Without it they're skipped, as they are in CI.
- For gameplay rules, the [difftest kit](difftest/README.md) compares the original game with this port.
- Say in your pull request how you checked your change.

## Contributor agreement

Before we can merge your first pull request, you need to agree to our [contributor agreement](CLA.md). It's short and in plain English.

When you open a pull request, the CLA Assistant bot comments with a link. Sign in with GitHub and accept the agreement, and the check turns green. You only do this once, unless the agreement changes.

**Why we ask.**
- The engine is under the Sustainable Use License, so nobody can sell it.
- The agreement lets the project's maintainer offer the engine under other terms as well, including commercial ones.
- You keep your copyright, and your work stays available to everyone under the project's license either way.
- If that's a problem for you, open an issue before you start and we'll talk.

**If the bot says you're not a GitHub user.** The bot matches each commit to a GitHub account by its email address. Either add the email you commit with to your GitHub account, or commit with an email that's already on it. Then use the "recheck" link in the bot's comment.

## What you can contribute

- **Your own code and docs.** Comments with the original game's addresses and function names are welcome. They're how we check our code against the original.
- **Never files from the game.** That means no art, sound, maps, text or program files, and nothing extracted or rendered from them. Players bring their own copy of the game, and this repo never holds any of it.
- **Never pasted decompiler output.** Read Ghidra's pseudo-C, work out what the function does, then write it as clean code like the rest of the engine.
- **AI tools are fine.** You're responsible for what you submit: check it, and make sure it follows these rules.

## If you make your own version

The engine's license asks one thing of anyone who hands out a changed version, including by running it on a website: publish its source code, free and under the same license.

Better still, send your changes here as a pull request. We can only merge changes whose authors have signed the contributor agreement, so please open the pull request yourself rather than asking someone else to copy your code in.

## Code of conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).
