# RenderAlphaFix

RenderAlphaFix is a small x86 ASI plugin that repairs a malformed SA-MP server-object material state associated with crashes in GTA San Andreas RenderWare code.

The implementation is based on reverse engineering of the working object-repair path in SAMP AddOn. It deliberately operates at the SA-MP object layer and does not detour GTA's renderer.

## Background

The original field report describes a custom SA-MP client crashing at irregular intervals while server objects are enabled. Reported locations include:

```text
0x007324B6  CVisibilityPlugins::RenderAlphaAtomic
0x0081214A  RpMatFXMaterialGetEffects
0x00756B89  D3D9 geometry instancing
0x00749EE4  RpAtomicClone
```

See the [original report and discussion on BlastHack](https://www.blast.hk/threads/256784/#post-1689979).

The `0x7324B6` dump contains `RpMaterial*=1`, causing GTA to read address `0x00000008`. A later `0x749EE4` dump contains a geometry whose morph-target pointer is null. These are different downstream symptoms of an object resource that is no longer internally consistent.

## What the fix does

Analysis of SAMP AddOn found an upstream repair for server objects. It considers a material-text slot pending or malformed when all three conditions are true:

```text
materialType[slot] == 2
textureWasCreated[slot] == 0
materialOrBackgroundPointer[slot] != nullptr
```

For such an object, AddOn invokes SA-MP's own `CObject::ConstructMaterialText`. RenderAlphaFix reproduces that operation without depending on AddOn.

At runtime the plugin:

- locates `CObjectPool::Process` and `CObject::ConstructMaterialText` through long, unique executable signatures;
- captures the active object pool from the real process call;
- checks a small slice of the pool after each process call;
- validates the game-entity pointer and all 16 material slots;
- rebuilds at most one malformed object per process call;
- applies a per-object retry cooldown;
- disables repair safely if an unexpected access fault occurs.

No SA-MP RVA, PE entry point or version flag is used. The signatures were verified as unique in SA-MP 0.3.7-R1 and R3.

## Render-path safety

The preceding development approach intercepted every alpha atomic and guarded GTA resource construction. Those measures were intentionally removed: they operated too broadly inside performance-critical RenderWare paths, could interfere with world rendering on custom clients, and contained downstream symptoms instead of repairing the known source state.

The plugin does not execute validation, allocation, logging or detour bookkeeping inside `RenderAlphaAtomic`, `_RpAtomicClone` or another RenderWare hot path. Its only GTA patch is the game-loop initialization call-site; object repair runs from SA-MP's own object-pool process callback.

The replacement follows the known working AddOn strategy: repair the inconsistent SA-MP object before it reaches GTA's renderer. This minimizes compatibility risk with custom renderers and other ASI plugins.

## SAMP AddOn comparison

The relevant AddOn object fix and RenderAlphaFix perform the same recovery action: identify the inconsistent material-text state and call `CObject::ConstructMaterialText`.

A separate AddOn buffer initially believed to store object material text was verified to be a vehicle license-plate workaround. Its hooks target `CVehicle::SetLicensePlateText`, `CVehicle::ConstructLicensePlate` and `CVehiclePool`; it is unrelated to this object crash and is intentionally not copied.

RenderAlphaFix differs mainly in deployment and discovery:

- it is a single-purpose ASI;
- it finds R1/R3 functions by code signatures;
- it scans incrementally instead of running a full pool pass;
- it logs every actual reconstruction by default.

## Compatibility

- GTA San Andreas 1.0 US (Hoodlum executable layout for game-loop initialization)
- 32-bit Windows process
- compatible x86 ASI loader
- SA-MP 0.3.7-R1 and R3 signatures verified

R2 and R4 may share compatible compiler patterns, but have not been field-tested. If either signature is absent or ambiguous, the plugin logs the condition and makes no SA-MP patch.

## Installation

1. Copy `RenderAlphaFix.asi` and `RenderAlphaFix.ini` next to `gta_sa.exe`.
2. Start the game and open `RenderAlphaFix.log`.

A successful startup contains:

```text
RenderAlphaFix 1.1 ASI LOADED
GAMELOOP HOOK INSTALLED
RenderAlphaFix 1.1 initializing from GTA GameLoop; mode=SA-MP object repair only
SYNTHETIC SELFTEST PASS: SA-MP material-text invariant
SA-MP OBJECT REPAIR enabled by signatures
```

When a real affected object is found:

```text
SA-MP OBJECT REPAIR rebuilt malformed object id=...
```

The absence of this line simply means that no matching object state has been observed yet.

## Configuration

```ini
[Repair]
ScanBudget=32
RetryCooldownMs=10000
LogRepairs=1
```

- `ScanBudget` controls how many object IDs are checked per pool-process call. Valid range: 1–256.
- `RetryCooldownMs` prevents a persistently failing object from being rebuilt every frame. Valid range: 1000–60000.
- `LogRepairs=0` suppresses individual reconstruction records without disabling repair.

The defaults scan a full 1000-object pool over multiple frames and never rebuild more than one object in a single call.

## Verification

1. Confirm all startup milestones in the log.
2. Check that normal world rendering and the HUD continue updating.
3. Exercise object stream-in/out, moving objects, `SetObjectMaterial`, and `SetObjectMaterialText`.
4. Run an extended session on the affected server.
5. If a repair line appears, retain the complete log and confirm that the game continues normally afterward.

The standalone mock host verifies game-loop hook installation, chaining to the original timer update, and startup self-test execution. The self-test covers the exact three-field SA-MP invariant without changing live game or SA-MP memory.

## Building

Clone the repository with submodules, then build the Win32 target with MSVC:

```bat
git clone --recursive https://github.com/moreveal/render-alpha-fix.git
cd render-alpha-fix
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

The resulting plugin is `build/Release/RenderAlphaFix.asi`. The MSVC runtime is linked statically.

To build the mock host:

```bat
cmake -S . -B build-tests -G "Visual Studio 18 2026" -A Win32 -DRENDER_ALPHA_FIX_BUILD_TESTS=ON
cmake --build build-tests --config Release
build-tests\Release\mock_host.exe build-tests\Release\RenderAlphaFix.asi
```

## Limitations

The plugin targets the specific malformed material-text state demonstrated by SAMP AddOn. It does not suppress arbitrary GTA or RenderWare exceptions. A crash caused by unrelated model data, graphics drivers, video-memory exhaustion or another plugin remains outside its scope.

Hot-unloading is not supported. The module is pinned until process exit.

## License and third-party code

RenderAlphaFix is released under the MIT License. kthook and its bundled dependencies retain their respective licenses; exact revisions are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
