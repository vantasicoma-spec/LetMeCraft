# LetMeCraft

A [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) C++ mod for **Gothic 1 Remake** that
lets you take over a crafting station occupied by an NPC. Press one key — the NPC
politely steps aside, you walk up, and the crafting interaction starts for you.

Current version: **0.8.18**.

## What it does

Stand near a crafting station that an NPC is using (forge, anvil, whetstone,
cauldron, alchemy table, workbench, stove, saw, tanning rack, etc.) and press
**E** on the keyboard or **Y** on an XInput-compatible gamepad. The mod then:

1. Cancels the NPC's crafting ability through the game's own request-end route
   (`OnRequestEndQuick` on its GAS interaction ability) — no teleports, no
   scripted hacks.
2. Applies a temporary "routine blocker" gameplay tag to the NPC so its daily
   routine cannot instantly re-grab the station. The tag is runtime-only and is
   never written into savegames.
3. Makes the NPC walk to a spot about 2 m behind your back and park there for
   17 seconds, then resume its routine naturally.
4. Auto-walks your character to within ~2 m of the station (inside the game's
   interaction-sensing radius) and briefly locks manual movement (10 s max,
   with multiple fail-safe unlock paths).
5. Claims the interaction spot for the player and automatically starts your
   interaction with the station, verifying it grabbed the **right** station —
   important in clusters of adjacent stations.

Details that make it feel natural:

- Works only when the station is within **4 m** of you and the NPC within
  **7 m**; the press is ignored while a menu/cursor is open.
- Repeated presses are throttled (1.8 s cooldown). Pressing E again near an
  already-evicted NPC refreshes the hold instead of starting a new eviction.
- If the auto-take cannot succeed (stuck spot, blocked path), it gives up after
  a bounded number of attempts, frees your movement, and shortens the NPC's
  parking to a 3-second tail.
- Gamepad support is loaded dynamically (`xinput1_4` → `xinput9_1_0` →
  `xinput1_3`); if XInput is unavailable, keyboard E still works.

### Robustness

- All game-state work runs on the game thread via the UE4SS engine-tick hook;
  input handlers only queue a request.
- Every tick is wrapped in both C++ exception handling and an SEH backstop, so
  an access violation inside the mod logs diagnostics and recovers instead of
  crashing the game.
- Object pointers are weak/serial-validated (GC-safe); all transient state is
  wiped on map change or savegame load.
- No object sweeps (`FindAllOf`) in steady-state hot paths — no FPS stutter.
- Every log line in `UE4SS.log` is prefixed with `[LetMeCraft]`.

## Requirements

In-game:

- Gothic 1 Remake (PC).
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) installed into the game's
  `G1R\Binaries\Win64` folder. The mod is built against UE4SS **v3.0.1**
  (commit `272ce2f80450dbcf11c53e2ae57fa5649d2d39be`).

For building:

- Windows, Visual Studio 2022 with the C++ workload (MSVC toolset **14.43+**,
  includes MASM).
- CMake **3.22+** (the one bundled with VS 2022 works, or a standalone install).
- Rust **1.73+** (`rustc`/`cargo` on `PATH`, e.g. via [rustup](https://rustup.rs)) —
  required by UE4SS's `patternsleuth` dependency.
- Network access on the first configure (CMake fetches the Corrosion build tool).

## Building

The `RE-UE4SS` checkout is **not** tracked in this repository — clone it first.
All commands below run from the repository root.

1. Get UE4SS sources at the pinned commit:

   ```powershell
   cd cpp
   git clone --recursive https://github.com/UE4SS-RE/RE-UE4SS.git RE-UE4SS
   git -C RE-UE4SS checkout 272ce2f80450dbcf11c53e2ae57fa5649d2d39be
   git -C RE-UE4SS submodule update --init --recursive
   cd ..
   ```

   (If the checkout lives elsewhere, pass `-DRE_UE4SS_DIR=<path>` at configure
   time instead.)

2. Configure:

   ```powershell
   cmake -S cpp -B cpp/build -G "Visual Studio 17 2022" -A x64
   ```

3. Build the mod (the config name `Game__Shipping__Win64` is UE4SS's standard
   shipping triplet and is required):

   ```powershell
   cmake --build cpp/build --config Game__Shipping__Win64 --target LetMeCraft
   ```

The resulting DLL appears at:

```text
cpp\build\LetMeCraft\Game__Shipping__Win64\LetMeCraft.dll
```

## Installing into the game

1. Locate the UE4SS mods folder inside the game installation:

   ```text
   <Gothic 1 Remake install folder>\G1R\Binaries\Win64\Mods\
   ```

2. Copy the built DLL there as `main.dll` (the UE4SS C++ mod naming
   convention — the file **must** be renamed):

   ```text
   Mods\
   ├── mods.txt              ← add the enable line here
   └── LetMeCraft\
       └── dlls\
           └── main.dll      ← renamed LetMeCraft.dll
   ```

3. Enable the mod in `Mods\mods.txt` by adding this line **above** the
   `Keybinds : 1` entry:

   ```text
   LetMeCraft : 1
   ```

To update the mod after a rebuild, just overwrite `main.dll` with the new
build (with the game closed). To uninstall, remove the `LetMeCraft` folder and
its line from `mods.txt`.

## Verifying it works

1. Launch the game normally (without `-as-development-mode` — that flag hides
   savegames).
2. Load a save near an NPC visibly using a crafting station.
3. Walk up to the station (within ~4 m) and press **E** once.
4. The NPC should step behind you and your character should walk in and start
   using the station.
5. After the session, check `G1R\Binaries\Win64\UE4SS.log` for `[LetMeCraft]`
   lines. Note: the log is overwritten on every game launch.

A successful run logs (abridged):

```text
[LetMeCraft] loaded. Press E or gamepad Y near an occupied crafting NPC.
[LetMeCraft] ... calling OnRequestEndQuick on ability=...
[LetMeCraft] eviction started owner=... spot=... holdMs=17000 ...
[LetMeCraft] approach started ... / approach done ...
[LetMeCraft] claim ok spot=...
[LetMeCraft] player use station attempt=N ... activated=true
[LetMeCraft] player took station owner=... - NPC keeps parking until the hold expires.
[LetMeCraft] eviction finished ...
```

## Tuning

The main calibration knobs live as constants near the top of
[dllmain.cpp](cpp/LetMeCraft/dllmain.cpp) (rebuild + reinstall after changing):

| Constant | Default | Meaning |
|---|---|---|
| `kMaxStationDistance` | `400` | Max distance to the station pivot for the E press (4 m) |
| `kMaxTargetDistance` | `700` | Max distance to the NPC (7 m) |
| `kHoldDuration` | `17 s` | How long the NPC stays parked behind the player |
| `kMoveLockDuration` | `10 s` | Auto-take window / max player movement lock |
| `kManualRequestCooldown` | `1800 ms` | Cooldown between manual presses |

## Known limitations

- **E** may overlap the game's own interact binding — if pressing it next to an
  NPC instantly opens a dialog, change the key in
  [dllmain.cpp](cpp/LetMeCraft/dllmain.cpp) and rebuild.
- The mod talks to the game through reflected properties plus a few struct
  offsets taken from the UE4SS SDK dump, so a game patch may require
  re-verifying them. Layout mismatches degrade into logged no-ops rather than
  crashes.
- Windows only (UE4SS C++ mod).

## Repository layout

```text
cpp/
├── CMakeLists.txt           # wrapper project: RE-UE4SS + the mod
├── LetMeCraft/
│   ├── CMakeLists.txt       # the mod target (shared library, links UE4SS)
│   ├── dllmain.cpp          # thin mod shell: input, hooks, tick dispatch, exports
│   └── lmc/                 # the mod, split into cohesive components (header-only)
│       ├── Common.hpp       # stateless utilities: reflection, UFunction calls,
│       │                    #   geometry, GAS readers, constants, structs, run_guarded
│       ├── GameObjects.hpp  # weak-cached game-object lookups (subsystem, player, sensor)
│       ├── MovementLock.hpp  # player movement/jump lock + controller-function warm-up
│       ├── PlayerSensor.hpp  # interaction-sensor pin/unfreeze
│       ├── SpotClaims.hpp    # interaction-spot claim/token operations
│       ├── CraftingScanner.hpp # find & cancel the occupied crafting ability
│       ├── RoutineBlocker.hpp  # the GAS routine-blocker tag (apply/remove)
│       └── EvictionManager.hpp # the eviction state machine (orchestrates the above)
└── RE-UE4SS/                # UE4SS checkout (not tracked; clone manually)
```

The code is organized as a thin `LetMeCraft` mod class (in `dllmain.cpp`) that owns
the components in `lmc/` and forwards the UE4SS callbacks (keybind, engine tick, map
load) to them. Each component is one focused class under `namespace lmc`; the bulk of
the per-frame logic lives in `EvictionManager`, which holds references to the service
components it coordinates. All compile into one translation unit (only `dllmain.cpp`
is listed in CMake; the headers are `#include`d), so there is no build-system change.
