# Animated Poisons Redone — SKSE plugin source

Source of `AnimatedPoisonsRedone.dll`, the SKSE plugin of the mod **Animated Poisons Redone**: the poison-applying
animation of *Immersive Interactions - New Anims* (GiraPomba) through *Offset Movement Animation*, without Immersive
Interactions and without Papyrus. One DLL for Skyrim **SE 1.5.97, AE 1.6.x and 1.7.x**.

What the DLL does:

- catches the moment a poison is applied (`PoisonedWeapon` event) and plays the pose through OMA with the mod's own
  behavior events (Nemesis / Pandora patch of the mod), the clip is picked by Open Animation Replacer;
- puts the poison's / arrow's **own model** in the hand (3rd and 1st person): bottles placed from
  `SKSE/Plugins/AnimatedPoisonsRedone/{ThirdPerson,FirstPerson}/*.json` (built-in values otherwise), arrows and bolts
  fitted along the shaft of the loaded model (a JSON entry overrides); the loaded crossbow bolt stays in its groove;
- draws a sheathed weapon first, waits for a draw / sheathe in progress, skips the animation while attacking, blocking,
  casting, dodging...; the pose is interrupted by attacks, blocks, dodges (TK Dodge RE, The Ultimate Dodge Mod), BFCO;
- repeats the pose seamlessly for a poison applied in its last 40 % (`fRepeatWindow`), follows 1st / 3rd person switches during the pose;
- writes a full diagnostic log with `bDebugLog = true` (`Documents/My Games/Skyrim Special Edition/SKSE/AnimatedPoisonsRedone.log`).

## Build

Requirements: Visual Studio 2022 (MSVC, C++20), CMake 3.21+, [vcpkg](https://github.com/microsoft/vcpkg) with the
`VCPKG_ROOT` environment variable set, Git.

```
git clone https://github.com/CharmedBaryon/CommonLibSSE-NG external/CommonLibSSE-NG
git -C external/CommonLibSSE-NG checkout b93280e832f263dbef44e44cbe2936622a02f91a
cmake --preset default
cmake --build build --config Release
```

Result: `build/Release/AnimatedPoisonsRedone.dll`; the INI is `data/SKSE/Plugins/AnimatedPoisonsRedone.ini`.

CommonLibSSE-NG is used as is plus one patch, `external/patches/commonlibsse-ng-stb.patch`, applied automatically at
configure time (`external/CommonLibSSE-NG.cmake`): Skyrim 1.6+ / 1.7 is detected as AE (upstream took 1.7 for SE),
Address Library format 5 (`versionlib-1-7-*.bin`) is read, an id missing from the Address Library is a clear error.

## Layout

| File | What |
|---|---|
| `src/main.cpp` | plugin declaration (Address Library, no struct use — loads on every runtime), log, SKSE messages |
| `src/Hooks.cpp` | `PlayerCharacter` vtable hooks (Update, UpdateAnimation, NotifyAnimationGraph), event sinks |
| `src/Poison.cpp` | the session: when to play, draw / wait / skip, the pose, interrupts, repeats, view switch |
| `src/HandItems.cpp` | the poison's / projectile's own model on the 3P / 1P skeleton |
| `src/Placement.cpp` | positions: JSON, then built-in values (bottles: `BuiltinTransforms.inl`, from the original mod's script; arrows / bolts: the steel arrow's placement, fitted along the shaft in `HandItems.cpp`) |
| `src/Diagnostics.cpp` | the startup report in the log: game, SKSE, Address Library, dependencies, files, behavior graphs |
| `src/Flow.cpp` | tiny C++20 coroutines (wait for time / a condition) the session is written with |
| `src/Settings.cpp` | the INI (6 keys) and the fixed values |

## Credits

Animations, models and positions: GiraPomba (*Immersive Interactions - New Anims*),
Xing and GiraPomba (*Offset Movement Animation*). CommonLibSSE-NG: CharmedBaryon and contributors. Plugin: STB Team.

License: MIT (see `LICENSE`).
