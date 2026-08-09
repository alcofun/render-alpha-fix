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

For valid lists, the plugin stores exact material pointers and alpha values in per-call storage, invokes GTA's original render callback, then restores values in reverse order. The first 512 entries use inline storage; larger lists use dynamically sized storage up to the configured safety limit. Restoration of material alpha and atomic flags is protected with structured exception handling.

The normal render path uses guarded memory accesses instead of issuing `VirtualQuery` system calls for every material. Rejection logging is buffered and repeated reports are heavily throttled, keeping disk I/O out of the frame-critical path.

On compatible SA-MP clients, a rejected material list schedules an event-driven object-pool repair. `CObjectPool::Process` and `CObject::ConstructMaterialText` are located through unique code signatures; no SA-MP RVA, PE entry point or global-address table is used. The repair reproduces the invariant used by SAMP AddOn: a material-text slot is malformed when its type is text, its texture-created flag is clear, and its material/background pointer is still populated. Up to five affected objects are rebuilt through SA-MP's own `CObject::ConstructMaterialText`; scanning is rate-limited and never runs during healthy rendering.

This upstream repair targets the shared damaged-resource state associated with failures reported at `0x7324B6` (`CVisibilityPlugins::RenderAlphaAtomic`), `0x81214A` (`RpMatFXMaterialGetEffects`) and `0x756B89` (`_rxD3D9Instance`). It also prevents malformed object resources from propagating into later hierarchy and skinned-frame updates reported at `0x7C5A8B` and `0x4D1996`. The separate `0x7F5A3A` dynamic-vertex-buffer failure is a video-memory exhaustion path and is not treated as the same defect.

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
| Protection point | RenderWare containment plus compatible SA-MP object repair | Can repair known object/material states earlier |
| Raised material counts | Removes the original 152-entry stack overflow | Not the focus of the reported workaround |
| Failure policy | Skips one unsafe atomic for the current frame | Upstream repair may preserve the object |
| Diagnostics | Signature checks, self-tests, rejection log | Depends on the add-on build |

This does not make either design universally superior. Repairing bad SA-MP object state at its source can preserve more rendering information. RenderAlphaFix is preferable when SAMP AddOn is incompatible with a custom client, when a small auditable fix is required, or when corruption may originate outside a known SA-MP material-text path.

## Compatibility

- GTA San Andreas 1.0 US (Hoodlum executable layout)
- 32-bit Windows process
- any compatible x86 ASI loader
- all SA-MP/custom clients that preserve the verified GTA code signatures for renderer containment
- upstream repair signature-tested against SA-MP 0.3.7-R1 and 0.3.7-R3

R2 and R4 are expected to share the matched compiler/layout pattern, but have not been run in a real client. The repair is enabled only when both long signatures are unique in the loaded module; otherwise renderer containment stays active and the optional SA-MP repair is disabled.

Unknown GTA executables are not patched. An unknown or substantially modified `samp.dll` keeps renderer containment active but disables the optional object repair.

### Verification status

| Client | Signature discovery | Runtime initialization | Field result |
|---|---|---|---|
| SA-MP 0.3.7-R1 | Verified, one unique match per function | Verified | Normal rendering and self-tests pass |
| SA-MP 0.3.7-R3 | Verified, one unique match per function | Verified | Reported render stutter is resolved; normal rendering and self-tests pass |
| SA-MP 0.3.7-R2/R4 | Expected compatible | Not tested | Not tested |

The captured R1/R3 sessions did not contain a live `REJECT` followed by `SA-MP OBJECT REPAIR rebuilt`, so signature installation and performance are field-verified while a naturally triggered object reconstruction remains subject to extended server testing.

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

The regression suite also covers the field-observed `0xC1000000` material value and the malformed SA-MP material-text slot invariant used to schedule object reconstruction.

## Building

Clone the repository with submodules, then build the Win32 target with MSVC:

```bat
git clone --recursive https://github.com/moreveal/render-alpha-fix.git
cd render-alpha-fix
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

The resulting plugin is `build/Release/RenderAlphaFix.asi`. The MSVC runtime is linked statically.

To build the mock host as well:

```bat
cmake -S . -B build-tests -G "Visual Studio 18 2026" -A Win32 -DRENDER_ALPHA_FIX_BUILD_TESTS=ON
cmake --build build-tests --config Release
build-tests\Release\mock_host.exe build-tests\Release\RenderAlphaFix.asi
```

## Limitations

Guarded reads and writes reject null, low and inaccessible pointers, but they cannot prove that an object in otherwise accessible memory is still logically alive. Renderer containment therefore remains a last line of defence. The optional SA-MP repair addresses the known malformed material-text state only on the explicitly supported object layout.

Hot-unloading is not supported. The plugin is pinned until process exit. GTA's original RenderWare path is expected to execute on the game render thread; custom multithreaded renderers are outside the supported configuration.

## License and third-party code

RenderAlphaFix is released under the MIT License. kthook and its bundled dependencies retain their respective licenses; exact revisions are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
