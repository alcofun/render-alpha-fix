# RenderAlphaFix

RenderAlphaFix is a small x86 ASI plugin that repairs or rejects malformed SA-MP server objects before their resources reach GTA San Andreas RenderWare code.

The implementation combines the upstream object-repair path reverse engineered from SAMP AddOn with a narrow fail-closed replacement for GTA's unsafe alpha-material loop.

## Background

The original field report describes a custom SA-MP client crashing at irregular intervals while server objects are enabled. Reported locations include:

```text
0x007324B6  CVisibilityPlugins::RenderAlphaAtomic
0x0081214A  RpMatFXMaterialGetEffects
0x00756B89  D3D9 geometry instancing
0x00749EE4  RpAtomicClone
0x0049CD77  CTagManager::ResetAlpha
0x0074DA25  RpMaterialDestroy
```

See the [original report and discussion on BlastHack](https://www.blast.hk/threads/256784/).

The `0x7324B6` dump contains `RpMaterial*=1`, causing GTA to read address `0x00000008`. A later `0x749EE4` dump contains a geometry whose morph-target pointer is null. These are different downstream symptoms of an object resource that is no longer internally consistent.

The `0x49CD77` crash is a separate stock-game invariant failure. GTA can identify an entity as a graffiti tag by its model while that entity has no corresponding entry in `CTagManager`'s 150-entry descriptor table. The original function dereferences the failed lookup without a null check. RenderAlphaFix guards this exact transition and skips only the invalid alpha reset; valid tags continue through the original function.

The `0x74DA25` crash is the first `RpMaterialDestroy` dereference of the
16-bit reference count at `material+0x18`; the reported null material therefore
faults at address `0x18`. Version 1.3 adds destruction-boundary preflight for
`RpGeometryDestroy`, `RpMaterialDestroy`, `_rpMaterialListDeinitialize`, and
`RwTextureDestroy`.

## What the fix does

Analysis of SAMP AddOn found an upstream repair for server objects. It considers a material-text slot pending or malformed when all three conditions are true:

```text
materialType[slot] == 2
textureWasCreated[slot] == 0
materialOrBackgroundPointer[slot] != nullptr
```

For such an object, AddOn invokes SA-MP's own `CObject::ConstructMaterialText`. RenderAlphaFix reproduces that operation without depending on AddOn.

At runtime the plugin:

- locates `CObjectPool::ConstructMaterials`, `CObjectPool::ShutdownMaterials`, the `CObject::SetMaterialText` completion path, and `CObject::ConstructMaterialText` through executable signatures;
- replaces the same internal section of `CObjectPool::ConstructMaterials` used by SAMP AddOn;
- marks repair work dirty when material text changes or SA-MP shuts material resources down;
- performs repairs only when SA-MP itself enters its material-construction phase;
- derives the pool layout from the matched code and supports both 1000- and 2100-object layouts;
- validates the game-entity pointer and all 16 material slots;
- uses AddOn's scheduling policy: a fast sequential pass first, then nearest-object ordering if reconstruction work exceeds the 5 ms slice;
- leaves unfinished work dirty for the next native `ConstructMaterials` invocation;
- does not catch and resume from exceptions raised halfway through SA-MP or Direct3D resource construction.

It also sanitizes incoming object-material commands. Both verified SA-MP clients contain an off-by-one check in `CObject::SetMaterial` and `CObject::SetMaterialText`: index `16` is accepted although the fixed arrays only contain entries `0–15`; negative indices are not rejected either. Either case writes outside the material arrays.

RenderAlphaFix rejects such a command before SA-MP touches object memory, queues the affected object for quarantine, and deletes it through `CObjectPool::Delete` from the next native material-construction phase. The queued GTA entity pointer is checked again before deletion so a recycled pool allocation cannot cause a different object to be removed.

The quarantine is not limited to the index check. On material lifecycle events it validates:

- the complete `SetMaterial` and `SetMaterialText` payload, including bounded strings, texture size and text alignment;
- type, ownership and creation state for every one of the object's 16 SA-MP material slots;
- stored text and font buffers, sprites, background textures and generated Direct3D textures;
- `CEntity -> RwObject -> RpAtomic/RpClump -> RpGeometry -> RpMaterialList` linkage;
- every material pointer and the RenderWare list count/capacity invariant;
- the geometry morph target required by `_RpAtomicClone`;
- GTA's hard 152-material stack limit in `CVisibilityPlugins::RenderAlphaAtomic`.

Invalid slot state is detached before object destruction so SA-MP does not call `Release` or `free` through a pointer already known to be corrupt. An invalid RenderWare graph is detached from the SA-MP wrapper for the same reason; leaking that already-corrupt resource is safer than executing its destructor.

Only rejected objects receive detailed diagnostic records. Each quarantine line includes the SA-MP object ID, GTA model ID, XYZ position, affected slot, operation and rejection reason. Healthy objects are never logged individually.

Two additional RenderWare use-site guards protect the shared geometry-sphere operations in `_RpAtomicClone` and `RpAtomicSetGeometry`. They validate the minimum first-morph-target sphere invariant immediately before the stock functions dereference it. For malformed geometry they skip only the unsafe sphere refresh: `_RpAtomicClone` continues to its authoritative copy from the source atomic, while `RpAtomicSetGeometry` preserves the atomic's previous sphere. Neither path returns a null atomic, so callers that assume clone success remain compatible.

The GTA alpha-render, geometry-sphere and tag-manager entry points use their fixed GTA San Andreas 1.0 US virtual addresses. Unlike SA-MP module offsets, these do not vary by client revision. Before installing a hook, the plugin verifies the expected original instructions at that address. If another plugin has already detoured a site, RenderAlphaFix leaves that individual guard disabled instead of layering a second hook over it.

The four destruction hooks are installed transactionally. Positive reference
counts and structurally valid owners continue through the stock functions,
including shared references (`2 -> 1`). Final-owner paths validate the material
list header, texture dictionary links, and minimum owned-resource ranges before
any plugin callback or free can run. A valid material list is still processed by
the original loop: malformed individual material entries return success from the
guard while healthy entries retain their normal reference-count and cleanup
behavior. If the list header itself is malformed, the guard clears its three
fields and intentionally leaks the untrusted storage rather than passing an
arbitrary pointer to RenderWare's allocator. No exception is caught and resumed
halfway through a destructor.

The two mid-function sphere guards use kthook's x86 naked context and support GTA/SA-MP's normal main-thread RenderWare execution path. Third-party mods that invoke `RpAtomicClone` or `RpAtomicSetGeometry` concurrently from worker threads are outside the supported execution model; RenderWare itself is not treated as a thread-safe API here.

The complete unsafe `CTagManager` entity-alpha family is protected: `GetAlpha`, `SetAlpha` and `ResetAlpha` are each located by independent GTA signatures. The guard decodes the live descriptor count and table from the matched code, rejects corrupt counts, and verifies entity membership before invoking the original routines. Missing descriptors return alpha zero; `SetAlpha` still updates a live atomic but skips descriptor bookkeeping; `ResetAlpha` returns safely. Valid tags follow the original code unchanged. There is no frame scan or allocation on these paths.

No SA-MP RVA, PE entry point or version flag is used. The signatures were verified as unique in SA-MP 0.3.7-R1 and R3.

## Render-path safety and cost

Broad replacements for `CEntity::CreateRwObject` and `_RpAtomicClone` are intentionally avoided. Those functions serve unrelated GTA systems such as 3D markers as well as streamed map entities, and returning failure can create invalid clone results in callers that assume success. The sphere guards instead intercept only the two instructions that consume the affected invariant and preserve each stock function's success contract.

Field logs showed that upstream reconstruction alone could still leave `RenderAlphaAtomic` receiving `RpMaterial*` values such as `1` and `0xC1000000`. The plugin therefore replaces that one GTA function with a bounded implementation. Its normal path performs no heap allocation, `VirtualQuery`, pool scan or logging. It validates the list bounds and material pointers with guarded reads, uses a fixed 152-entry stack snapshot, preserves GTA's alpha semantics, calls the original renderer, and restores the exact material pointers in reverse order. An invalid atomic is skipped instead of being passed into RenderWare; only this exceptional path searches for a matching SA-MP object and writes a diagnostic record.

`CEntity::CreateRwObject` and the remaining RenderWare construction pipeline are not replaced. `_RpAtomicClone` and `RpAtomicSetGeometry` receive narrow mid-function morph-target guards; the descriptor invariant is checked only across GTA's three entity-alpha tag operations; SA-MP object validation, deletion and repair run from the native `CObjectPool::ConstructMaterials` phase.

This phase is important. Material-text reconstruction creates Direct3D resources and must remain synchronized with SA-MP's own shutdown/rebuild lifecycle, including device loss and recovery during Alt+Tab. Calling the same routine from a generic frame or pool-processing hook is not equivalent and can stall world rendering while the HUD continues updating.

The replacement follows the known working AddOn strategy: repair the inconsistent SA-MP object before it reaches GTA's renderer. No fixed-size scan window is used, so an object cannot remain unchecked merely because its ID is outside the current cursor range.

## SAMP AddOn comparison

The relevant AddOn object fix and RenderAlphaFix use the same recovery action and lifecycle:

- intercept `CObjectPool::ConstructMaterials` after its prologue and replace the stock material loop;
- set a shared dirty state from `CObject::SetMaterialText` and `CObjectPool::ShutdownMaterials`;
- inspect the same three slot fields and call `CObject::ConstructMaterialText` only from the native construction phase;
- switch from sequential repair to nearest-object scheduling when work exceeds the same 5 ms slice.

The implementation is signature-driven rather than a byte-for-byte copy. It uses fixed storage instead of AddOn's temporary ordered container, but preserves the observable repair order and deferral rules without adding per-frame scanning.

A separate AddOn buffer initially believed to store object material text was verified to be a vehicle license-plate workaround. Its hooks target `CVehicle::SetLicensePlateText`, `CVehicle::ConstructLicensePlate` and `CVehiclePool`; it is unrelated to this object crash and is intentionally not copied.

RenderAlphaFix differs mainly in deployment and discovery:

- it is a single-purpose ASI;
- it finds R1/R3 functions by code signatures;
- it derives the standard or extended pool layout from the matched `ConstructMaterials` code;
- it reports completed repair passes and logs renderer state only when an invalid alpha atomic is actually rejected.
- it fails closed on malformed command payloads and full object/resource-graph invariants, then records the quarantined object ID and reason.

## Compatibility

- GTA San Andreas 1.0 US (Hoodlum executable layout for game-loop initialization)
- 32-bit Windows process
- compatible x86 ASI loader
- SA-MP 0.3.7-R1 and R3 signatures verified

R2 and R4 may share compatible compiler patterns, but have not been field-tested. If either signature is absent or ambiguous, the plugin logs the condition and makes no SA-MP patch.

## Installation

1. Copy `RenderAlphaFix.asi` next to `gta_sa.exe`.
2. Start the game and open `RenderAlphaFix.log`.

A successful startup contains:

```text
RenderAlphaFix 1.3 ASI LOADED
GAMELOOP HOOK INSTALLED
RenderAlphaFix 1.3 initializing from GTA GameLoop; mode=SA-MP object repair + invalid-object quarantine + RenderWare use-site guards + zero-allocation alpha guard
RENDER ALPHA GUARD enabled
RW SPHERE GUARD enabled
RW DESTRUCTION GUARD enabled
GTA TAG INVARIANT GUARD enabled
SYNTHETIC SELFTEST PASS: SA-MP material-text invariant; full payload/slot graph; material indices 0..15; RenderWare material limit 152; first morph-target sphere invariant; zero-allocation alpha guard; tag descriptor invariant; destruction refcounts/list bounds; null morph teardown; pool layouts 1000/2100
SCHEDULING SELFTEST PASS: full 2100-object scan; nearest ordering; ConstructMaterials phase
SA-MP OBJECT REPAIR enabled by signatures
```

When a real affected object is found:

```text
SA-MP OBJECT REPAIR pass: rebuilt=... deferred=... phase=ConstructMaterials
```

An invalid incoming command or object graph produces:

```text
SA-MP OBJECT QUARANTINE queued object=... operation=SetMaterialText slot=16 reason=material index outside 0..15
SA-MP OBJECT QUARANTINE deleted objectId=... slot=... reason=...
```

A malformed geometry caught at a shared RenderWare sphere use-site produces:

```text
RW SPHERE GUARD skipped operation=RpAtomicClone ... reason=...
RW SPHERE GUARD skipped operation=RpAtomicSetGeometry ... reason=... sampOwnerQueued=...
```

A tag model missing from GTA's live descriptor table produces:

```text
GTA TAG INVARIANT GUARD skipped operation=... entity=... modelId=... position=(...) tagCount=... reason=tag model has no live CTagManager descriptor
```

The absence of this line simply means that no matching object state has been observed yet. There is no INI file: the pool capacity is derived at runtime and the AddOn-compatible repair limits are fixed deliberately so every installation uses the tested behavior.

## Verification

1. Confirm all startup milestones in the log.
2. Check that normal world rendering and the HUD continue updating.
3. Exercise object stream-in/out, moving objects, `SetObjectMaterial`, and `SetObjectMaterialText`.
4. Alt+Tab several times in an area containing affected server mapping and confirm that both the world and HUD continue updating.
5. Run an extended session on the affected server.
6. If a repair line appears, retain the complete log and confirm that the game continues normally afterward.

The standalone mock host verifies game-loop hook installation, chaining to the original timer update, both geometry-sphere hooks, and the four actual destruction detours. Destruction probes cover null and guard-page materials, shared `2 -> 1` references, mixed valid/null material lists, malformed list detachment, texture dictionary-link rejection, and geometry owner rejection. Internal self-tests additionally cover the exact three-field SA-MP invariant, payload bounds, the 152-material boundary, alpha preservation/restoration, null/count/type morph-target failures, invalid RenderWare pointers, both supported pool layouts, complete-pool scanning, and nearest-first ordering without changing live GTA or SA-MP memory.

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
ctest --test-dir build-tests -C Release --output-on-failure
```

## Limitations

The plugin covers malformed SA-MP object material commands and the object resource graph they create. It does not suppress arbitrary GTA or RenderWare exceptions. Driver failures, global video-memory exhaustion and corruption introduced by unrelated plugins remain outside its scope.

`VirtualQuery` proves committed access and the guards check structural bounds and
reference counts, but Windows cannot prove that an otherwise plausible pointer
belongs to RenderWare's allocator. Plugin callbacks can also mutate a valid graph
after preflight. The destruction guards therefore reduce known invalid-pointer
and invalid-refcount failure modes; they are not a general heap-integrity system.
The mock host exercises the hook mechanics, but version 1.3 still requires an
extended runtime test in the affected GTA/SA-MP client.

Hot-unloading is not supported. The module is pinned until process exit.

## License and third-party code

RenderAlphaFix is released under the MIT License. kthook and its bundled dependencies retain their respective licenses; exact revisions are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
