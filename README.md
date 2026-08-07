# RenderAlphaFix

RenderAlphaFix is a focused x86 ASI plugin for GTA San Andreas 1.0 US. It prevents crashes in `CVisibilityPlugins::RenderAlphaAtomic` caused by corrupted RenderWare material lists and removes the function's original 152-material stack limit.

The plugin works at the GTA renderer level. It does not depend on a particular `samp.dll`, server protocol revision, or SAMP AddOn installation.

## Background

The original field report describes a custom SA-MP client with increased limits crashing at irregular intervals while server objects are enabled. The reported exception is:

```text
Unhandled exception at 0x007324B6 in gta_sa.exe (+0x3324B6)
0xC0000005: Access violation reading location 0x00000008
EAX: 0x00000001
```

See the [original report and discussion on BlastHack](https://www.blast.hk/threads/256784/#post-1689979).

At `0x7324B6`, the game executes the equivalent of:

```asm
mov cl, byte ptr [eax+7]
```

`EAX` is expected to contain an `RpMaterial*`; offset `+7` is the material alpha byte. In the captured crash, the material-list entry contains `1`, so the game attempts to read address `0x00000008`.

Reverse engineering also exposed a separate defect in the same function. The original implementation reserves `0x98` bytes on the stack to save material alpha values. That is exactly 152 entries. A geometry with 153 or more materials writes past the buffer and can overwrite the return address. Raising client limits makes this latent bug more relevant.

## What the fix does

RenderAlphaFix replaces the complete `CVisibilityPlugins::RenderAlphaAtomic` entry at `gta_sa.exe+0x332480` using [kthook](https://github.com/kin4stat/kthook).

Before rendering an alpha atomic, it validates:

- the atomic and its geometry;
- the material count and allocated material-list capacity;
- the complete material pointer array;
- every `RpMaterial` object and its writable alpha field.

Validation is performed for the entire list before any alpha value is changed. If the list is inconsistent, the affected atomic is skipped for that frame. A damaged object may briefly disappear, but the invalid list is never passed into the RenderWare pipeline.

For valid lists, the plugin stores exact material pointers and alpha values in per-call storage, invokes GTA's original render callback, then restores values in reverse order. The first 152 entries use inline storage; larger lists use dynamically sized storage up to the configured safety limit. Restoration of material alpha and atomic flags is protected with structured exception handling.

Additional safeguards include:

- byte-signature checks before either game-code hook is installed;
- one-time initialization from the GTA game loop, without a worker thread;
- chaining when another ASI has already redirected the selected game-loop call;
- module pinning to prevent unsafe hot-unload while hooks are active;
- a startup self-test and an in-game hook-path self-test;
- throttled diagnostics for rejected atomics.

## SAMP AddOn comparison

SAMP AddOn is useful prior art and is reported to prevent this crash in supported installations. RenderAlphaFix has a different scope: it is a standalone last line of defence inside GTA's RenderWare path.

| | RenderAlphaFix | SAMP AddOn approach |
|---|---|---|
| Deployment | One single-purpose ASI | Part of a broader client add-on |
| SA-MP coupling | No `samp.dll` offsets or object layouts | Can depend on supported SA-MP/client layouts |
| Protection point | Validates the final RenderWare material list | Can repair known object/material states earlier |
| Raised material counts | Removes the original 152-entry stack overflow | Not the focus of the reported workaround |
| Failure policy | Skips one unsafe atomic for the current frame | Upstream repair may preserve the object |
| Diagnostics | Signature checks, self-tests, rejection log | Depends on the add-on build |

This does not make either design universally superior. Repairing bad SA-MP object state at its source can preserve more rendering information. RenderAlphaFix is preferable when SAMP AddOn is incompatible with a custom client, when a small auditable fix is required, or when corruption may originate outside a known SA-MP material-text path.

## Compatibility

- GTA San Andreas 1.0 US (Hoodlum executable layout)
- 32-bit Windows process
- any compatible x86 ASI loader
- SA-MP and custom clients that preserve the verified GTA code signatures

Unknown executables are not patched. A signature mismatch is written to the log and initialization stops safely.

## Installation

1. Copy `RenderAlphaFix.asi` and `RenderAlphaFix.ini` next to `gta_sa.exe`.
2. Start the game through the usual client or launcher.
3. Open `RenderAlphaFix.log` in the game directory.

A successful startup contains these milestones:

```text
GAMELOOP HOOK INSTALLED
HOOK INSTALLED
SYNTHETIC SELFTEST PASS
IN-GAME SELFTEST PASS
```

The in-game self-test passes a local invalid material pointer through the actual hook. It does not alter a live GTA material list and deliberately skips one atomic before normal rendering begins.

## Configuration

```ini
[Fix]
MaxMaterials=4096
LogRejected=1
```

`MaxMaterials` is an explicit upper bound for one geometry. Counts above it are rejected instead of allocating unbounded memory. `LogRejected=0` disables individual rejection messages while keeping the protection active.

## Verification

For initial deployment, use a separate game copy and temporarily remove other anti-crash plugins so the result is attributable to this ASI.

1. Confirm all four startup milestones in `RenderAlphaFix.log`.
2. Play normally for 15–30 minutes and check alpha-rendered objects, vehicles and peds.
3. Exercise object stream-in/out, moving objects, editing objects, `SetObjectMaterial` and `SetObjectMaterialText`.
4. Run an extended session on the affected server. Real corruptions appear as `REJECT` records and must not terminate the process.

The repository also contains a mock GTA host used during development. It verifies game-loop chaining, hook installation, in-game self-test completion, valid alpha restoration, and rejection of the observed `RpMaterial*=1` case.

## Building

Clone the repository with submodules, then build the Win32 target with MSVC:

```bat
git clone --recursive https://github.com/moreveal/render-alpha-fix.git
cd render-alpha-fix
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

The resulting plugin is `build/Release/RenderAlphaFix.asi`. The MSVC runtime is linked statically.

## Limitations

Windows page validation can reject null, low, inaccessible and protection-incompatible pointers. It cannot prove that an object in otherwise committed memory is still logically alive. The plugin therefore contains the renderer crash but does not replace a proper fix for the upstream object-lifetime bug.

Hot-unloading is not supported. The plugin is pinned until process exit. GTA's original RenderWare path is expected to execute on the game render thread; custom multithreaded renderers are outside the supported configuration.

## License and third-party code

RenderAlphaFix is released under the MIT License. kthook and its bundled dependencies retain their respective licenses; exact revisions are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
