#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

#pragma section(".mock", read, write, execute)
__declspec(allocate(".mock")) volatile unsigned char g_mockSpan[0x400000];
volatile std::uint32_t g_timerCalls{};
volatile std::uint32_t g_geometryDestroyCalls{};
volatile std::uint32_t g_materialDestroyCalls{};
volatile std::uint32_t g_materialListDeinitializeCalls{};
volatile std::uint32_t g_textureDestroyCalls{};

namespace {
constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;
constexpr std::uintptr_t kAtomicSetGeometrySphereCopyAddress = 0x749D7B;
constexpr std::uintptr_t kAtomicSetGeometryAfterSphereCopyAddress = 0x749D9A;
constexpr std::uintptr_t kAtomicCloneSphereCopyAddress = 0x749EDB;
constexpr std::uintptr_t kAtomicCloneAfterMorphSphereCopyAddress = 0x749EFA;
constexpr std::uintptr_t kGeometryDestroyAddress = 0x74CCC0;
constexpr std::uintptr_t kMaterialDestroyAddress = 0x74DA20;
constexpr std::uintptr_t kMaterialListDeinitializeAddress = 0x74E150;
constexpr std::uintptr_t kTextureDestroyAddress = 0x7F3820;

using DestroyFn = int(__cdecl*)(void*);
using ListDeinitializeFn = void*(__cdecl*)(void*);

int __cdecl MockTextureDestroy(void* texture) {
    ++g_textureDestroyCalls;
    (void)texture; // the reproduced native entry already decremented +0x54
    return 1;
}

int __cdecl MockMaterialDestroy(void* material) {
    ++g_materialDestroyCalls;
    auto* bytes = static_cast<std::uint8_t*>(material);
    auto& references = *reinterpret_cast<std::int16_t*>(bytes + 0x18);
    if (references > 1) {
        --references;
    } else {
        void* texture = *reinterpret_cast<void**>(bytes);
        if (texture)
            reinterpret_cast<DestroyFn>(kTextureDestroyAddress)(texture);
        *reinterpret_cast<void**>(bytes) = nullptr;
    }
    return 1;
}

void* __cdecl MockMaterialListDeinitialize(void* list) {
    ++g_materialListDeinitializeCalls;
    auto* bytes = static_cast<std::uint8_t*>(list);
    auto** materials = *reinterpret_cast<void***>(bytes);
    const auto count = *reinterpret_cast<std::int32_t*>(bytes + 4);
    for (std::int32_t index = 0; index < count; ++index) {
        reinterpret_cast<DestroyFn>(kMaterialDestroyAddress)(materials[index]);
        materials[index] = nullptr;
    }
    std::memset(bytes, 0, 12);
    return list;
}

int __cdecl MockGeometryDestroy(void* geometry) {
    ++g_geometryDestroyCalls;
    auto* bytes = static_cast<std::uint8_t*>(geometry);
    auto& references = *reinterpret_cast<std::int16_t*>(bytes + 0xE);
    if (references > 1)
        --references;
    else
        reinterpret_cast<ListDeinitializeFn>(
            kMaterialListDeinitializeAddress)(bytes + 0x20);
    return 1;
}

bool AllocateCodePage(std::uintptr_t address) {
    const auto page = address & ~static_cast<std::uintptr_t>(0xFFF);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT) {
        DWORD oldProtect{};
        if (VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                           PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE)
            return true;
        std::fwprintf(stderr, L"VirtualProtect failed at %p: %lu\n",
                      reinterpret_cast<void*>(page), GetLastError());
        return false;
    }
    const auto allocationBase = address & ~static_cast<std::uintptr_t>(0xFFFF);
    if (VirtualAlloc(reinterpret_cast<void*>(allocationBase), 0x10000,
                     MEM_RESERVE | MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE) != nullptr) return true;
    std::fwprintf(stderr, L"VirtualAlloc failed at %p: %lu\n",
                  reinterpret_cast<void*>(allocationBase), GetLastError());
    return false;
}

bool PrepareDestructionGuardSites() {
    if (!AllocateCodePage(kGeometryDestroyAddress) ||
        !AllocateCodePage(kMaterialDestroyAddress) ||
        !AllocateCodePage(kMaterialListDeinitializeAddress) ||
        !AllocateCodePage(kTextureDestroyAddress)) return false;

    const auto writeTail = [](std::uint8_t* destination, std::size_t at,
                              std::uintptr_t target,
                              const std::uint8_t* unwind,
                              std::size_t unwindSize) {
        std::memcpy(destination + at, unwind, unwindSize);
        at += unwindSize;
        destination[at++] = 0x68; // push handler
        std::memcpy(destination + at, &target, 4);
        at += 4;
        destination[at++] = 0xC3; // ret to handler
        return at;
    };

    std::uint8_t geometry[32] = {
        0x56, 0x8B, 0x74, 0x24, 0x08, 0x66, 0x8B, 0x46, 0x0E,
        0x0F, 0xBF, 0xC8, 0x49};
    const std::uint8_t popEsi[] = {0x5E};
    writeTail(geometry, 13,
              reinterpret_cast<std::uintptr_t>(&MockGeometryDestroy),
              popEsi, sizeof(popEsi));

    std::uint8_t material[32] = {
        0x56, 0x8B, 0x74, 0x24, 0x08, 0x66, 0x8B, 0x46, 0x18,
        0x66, 0x3D, 0x01, 0x00};
    writeTail(material, 13,
              reinterpret_cast<std::uintptr_t>(&MockMaterialDestroy),
              popEsi, sizeof(popEsi));

    std::uint8_t list[32] = {
        0x55, 0x8B, 0x6C, 0x24, 0x08, 0x57, 0x8B, 0x7D, 0x00,
        0x85, 0xFF};
    const std::uint8_t popEdiEbp[] = {0x5F, 0x5D};
    writeTail(list, 11,
              reinterpret_cast<std::uintptr_t>(&MockMaterialListDeinitialize),
              popEdiEbp, sizeof(popEdiEbp));

    std::uint8_t texture[32] = {
        0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x56, 0x54, 0x4A,
        0x8B, 0xC2, 0x89, 0x56, 0x54};
    writeTail(texture, 14,
              reinterpret_cast<std::uintptr_t>(&MockTextureDestroy),
              popEsi, sizeof(popEsi));

    std::memcpy(reinterpret_cast<void*>(kGeometryDestroyAddress), geometry,
                sizeof(geometry));
    std::memcpy(reinterpret_cast<void*>(kMaterialDestroyAddress), material,
                sizeof(material));
    std::memcpy(reinterpret_cast<void*>(kMaterialListDeinitializeAddress), list,
                sizeof(list));
    std::memcpy(reinterpret_cast<void*>(kTextureDestroyAddress), texture,
                sizeof(texture));
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

bool PrepareSphereGuardSites() {
    if (!AllocateCodePage(kAtomicSetGeometrySphereCopyAddress) ||
        !AllocateCodePage(kAtomicCloneSphereCopyAddress)) return false;

    const std::uint8_t setGeometryCode[] = {
        0x8B, 0x47, 0x5C,             // mov eax,[edi+5C]
        0x8D, 0x4E, 0x1C,             // lea ecx,[esi+1C]
        0x83, 0xC0, 0x04,             // add eax,4
        0x8B, 0x10,                   // mov edx,[eax]
        0x89, 0x11,                   // mov [ecx],edx
        0x8B, 0x50, 0x04,             // mov edx,[eax+4]
        0x89, 0x51, 0x04,             // mov [ecx+4],edx
        0x8B, 0x50, 0x08,             // mov edx,[eax+8]
        0x89, 0x51, 0x08,             // mov [ecx+8],edx
        0x8B, 0x40, 0x0C,             // mov eax,[eax+C]
        0x89, 0x41, 0x0C,             // mov [ecx+C],eax
        0xC3                          // ret (mock continuation)
    };
    const std::uint8_t cloneCode[] = {
        0x8B, 0x56, 0x5C,             // mov edx,[esi+5C]
        0x8D, 0x43, 0x1C,             // lea eax,[ebx+1C]
        0x83, 0xC2, 0x04,             // add edx,4
        0x8B, 0x0A,                   // mov ecx,[edx]
        0x89, 0x08,                   // mov [eax],ecx
        0x8B, 0x4A, 0x04,             // mov ecx,[edx+4]
        0x89, 0x48, 0x04,             // mov [eax+4],ecx
        0x8B, 0x4A, 0x08,             // mov ecx,[edx+8]
        0x89, 0x48, 0x08,             // mov [eax+8],ecx
        0x8B, 0x52, 0x0C,             // mov edx,[edx+C]
        0x89, 0x50, 0x0C,             // mov [eax+C],edx
        0xC3                          // ret (mock continuation)
    };
    std::memcpy(reinterpret_cast<void*>(kAtomicSetGeometrySphereCopyAddress),
                setGeometryCode, sizeof(setGeometryCode));
    std::memcpy(reinterpret_cast<void*>(kAtomicCloneSphereCopyAddress),
                cloneCode, sizeof(cloneCode));
    *reinterpret_cast<std::uint8_t*>(
        kAtomicSetGeometryAfterSphereCopyAddress) = 0xC3;
    const std::uint8_t cloneContinuation[] = {
        0x8B, 0x45, 0x1C, 0x89, 0x43, 0x1C,
        0x8B, 0x45, 0x20, 0x89, 0x43, 0x20,
        0x8B, 0x45, 0x24, 0x89, 0x43, 0x24,
        0x8B, 0x45, 0x28, 0x89, 0x43, 0x28,
        0xC3
    };
    std::memcpy(reinterpret_cast<void*>(
                    kAtomicCloneAfterMorphSphereCopyAddress),
                cloneContinuation, sizeof(cloneContinuation));
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

using GuardProbeFn = void(__cdecl*)();

GuardProbeFn MakeGuardProbe(std::uintptr_t site, std::uintptr_t esiValue,
                            std::uintptr_t ediValue,
                            std::uintptr_t ebpValue,
                            std::uintptr_t ebxValue) {
    auto* code = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!code) return nullptr;
    std::size_t at{};
    code[at++] = 0x53; // push ebx
    code[at++] = 0x55; // push ebp
    code[at++] = 0x56; // push esi
    code[at++] = 0x57; // push edi
    code[at++] = 0xBE; std::memcpy(code + at, &esiValue, 4); at += 4;
    code[at++] = 0xBF; std::memcpy(code + at, &ediValue, 4); at += 4;
    code[at++] = 0xBD; std::memcpy(code + at, &ebpValue, 4); at += 4;
    code[at++] = 0xBB; std::memcpy(code + at, &ebxValue, 4); at += 4;
    code[at++] = 0xE8;
    const auto callNext = reinterpret_cast<std::uintptr_t>(code + at + 4);
    const auto relative = static_cast<std::int32_t>(site - callNext);
    std::memcpy(code + at, &relative, 4); at += 4;
    code[at++] = 0x5F; // pop edi
    code[at++] = 0x5E; // pop esi
    code[at++] = 0x5D; // pop ebp
    code[at++] = 0x5B; // pop ebx
    code[at++] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), code, at);
    return reinterpret_cast<GuardProbeFn>(code);
}

bool ProbeSphereGuards() {
    alignas(4) std::uint8_t geometry[0x60]{};
    alignas(4) std::uint8_t morphTarget[0x1C]{};
    alignas(4) std::uint8_t sourceAtomic[0x4C]{};
    alignas(4) std::uint8_t cloneAtomic[0x4C]{};
    geometry[0] = 8;
    *reinterpret_cast<std::int32_t*>(geometry + 0x18) = 1;
    *reinterpret_cast<void**>(geometry + 0x5C) = morphTarget;
    *reinterpret_cast<void**>(morphTarget) = geometry;
    const std::uint32_t morphSphere[4] = {
        0x41200000, 0x41300000, 0x41400000, 0x41500000};
    const std::uint32_t sourceSphere[4] = {
        0x42300000, 0x42400000, 0x42500000, 0x42600000};
    std::memcpy(morphTarget + 4, morphSphere, sizeof(morphSphere));
    std::memcpy(sourceAtomic + 0x1C, sourceSphere, sizeof(sourceSphere));

    auto cloneProbe = MakeGuardProbe(
        kAtomicCloneSphereCopyAddress,
        reinterpret_cast<std::uintptr_t>(geometry), 0,
        reinterpret_cast<std::uintptr_t>(sourceAtomic),
        reinterpret_cast<std::uintptr_t>(cloneAtomic));
    auto setGeometryProbe = MakeGuardProbe(
        kAtomicSetGeometrySphereCopyAddress,
        reinterpret_cast<std::uintptr_t>(cloneAtomic),
        reinterpret_cast<std::uintptr_t>(geometry), 0, 0);
    if (!cloneProbe || !setGeometryProbe) return false;

    cloneProbe();
    const bool cloneValidCopied = std::memcmp(
        cloneAtomic + 0x1C, sourceSphere, sizeof(sourceSphere)) == 0;
    std::memset(cloneAtomic + 0x1C, 0x42, sizeof(morphSphere));
    setGeometryProbe();
    const bool setValidCopied = std::memcmp(
        cloneAtomic + 0x1C, morphSphere, sizeof(morphSphere)) == 0;
    std::memset(cloneAtomic + 0x1C, 0x42, sizeof(morphSphere));
    *reinterpret_cast<void**>(geometry + 0x5C) = nullptr;
    cloneProbe();
    const bool cloneInvalidUsedSourceSphere = std::memcmp(
        cloneAtomic + 0x1C, sourceSphere, sizeof(sourceSphere)) == 0;
    std::uint8_t preservedSphere[sizeof(morphSphere)]{};
    std::memset(preservedSphere, 0x42, sizeof(preservedSphere));
    std::memcpy(cloneAtomic + 0x1C, preservedSphere, sizeof(preservedSphere));
    setGeometryProbe();
    const bool setInvalidPreserved = std::memcmp(
        cloneAtomic + 0x1C, preservedSphere, sizeof(preservedSphere)) == 0;

    VirtualFree(reinterpret_cast<void*>(cloneProbe), 0, MEM_RELEASE);
    VirtualFree(reinterpret_cast<void*>(setGeometryProbe), 0, MEM_RELEASE);
    return cloneValidCopied && setValidCopied &&
           cloneInvalidUsedSourceSphere &&
           setInvalidPreserved;
}

bool ProbeDestructionGuards() {
    g_geometryDestroyCalls = 0;
    g_materialDestroyCalls = 0;
    g_materialListDeinitializeCalls = 0;
    g_textureDestroyCalls = 0;

    alignas(4) std::uint8_t sharedMaterial[0x1C]{};
    *reinterpret_cast<std::int16_t*>(sharedMaterial + 0x18) = 2;
    const bool sharedMaterialResult =
        reinterpret_cast<DestroyFn>(kMaterialDestroyAddress)(sharedMaterial) == 1;
    const bool sharedMaterialDecremented =
        *reinterpret_cast<std::int16_t*>(sharedMaterial + 0x18) == 1 &&
        g_materialDestroyCalls == 1;

    void* inaccessible = VirtualAlloc(nullptr, 0x1000,
        MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    const auto materialCallsBeforeReject = g_materialDestroyCalls;
    const bool nullRejected =
        reinterpret_cast<DestroyFn>(kMaterialDestroyAddress)(nullptr) == 1;
    const bool guardPageRejected = inaccessible &&
        reinterpret_cast<DestroyFn>(kMaterialDestroyAddress)(inaccessible) == 1;
    const bool rejectsSkippedNative =
        g_materialDestroyCalls == materialCallsBeforeReject;
    alignas(4) std::uint8_t negativeRefMaterial[0x1C]{};
    *reinterpret_cast<std::int16_t*>(negativeRefMaterial + 0x18) = -1;
    reinterpret_cast<DestroyFn>(kMaterialDestroyAddress)(negativeRefMaterial);
    const bool negativeRefRejected =
        g_materialDestroyCalls == materialCallsBeforeReject;

    alignas(4) std::uint8_t finalMaterial[0x1C]{};
    *reinterpret_cast<std::int16_t*>(finalMaterial + 0x18) = 1;
    void* mixedEntries[3]{sharedMaterial, nullptr, finalMaterial};
    alignas(4) std::uint8_t list[12]{};
    *reinterpret_cast<void***>(list) = mixedEntries;
    *reinterpret_cast<std::int32_t*>(list + 4) = 3;
    *reinterpret_cast<std::int32_t*>(list + 8) = 3;
    const auto materialCallsBeforeList = g_materialDestroyCalls;
    const bool mixedListResult =
        reinterpret_cast<ListDeinitializeFn>(
            kMaterialListDeinitializeAddress)(list) == list;
    const bool mixedListPreservedLifecycle = mixedListResult &&
        g_materialListDeinitializeCalls == 1 &&
        g_materialDestroyCalls == materialCallsBeforeList + 2 &&
        *reinterpret_cast<std::int16_t*>(sharedMaterial + 0x18) == 1 &&
        *reinterpret_cast<void**>(list) == nullptr &&
        *reinterpret_cast<std::int32_t*>(list + 4) == 0 &&
        *reinterpret_cast<std::int32_t*>(list + 8) == 0;

    void* corruptEntries[1]{finalMaterial};
    alignas(4) std::uint8_t corruptList[12]{};
    *reinterpret_cast<void***>(corruptList) = corruptEntries;
    *reinterpret_cast<std::int32_t*>(corruptList + 4) = 2;
    *reinterpret_cast<std::int32_t*>(corruptList + 8) = 1;
    const auto listCallsBeforeReject = g_materialListDeinitializeCalls;
    reinterpret_cast<ListDeinitializeFn>(
        kMaterialListDeinitializeAddress)(corruptList);
    const bool corruptListDetached =
        g_materialListDeinitializeCalls == listCallsBeforeReject &&
        *reinterpret_cast<void**>(corruptList) == nullptr &&
        *reinterpret_cast<std::int32_t*>(corruptList + 4) == 0 &&
        *reinterpret_cast<std::int32_t*>(corruptList + 8) == 0;

    alignas(4) std::uint8_t sharedTexture[0x58]{};
    *reinterpret_cast<std::int32_t*>(sharedTexture + 0x54) = 2;
    const bool sharedTextureResult =
        reinterpret_cast<DestroyFn>(kTextureDestroyAddress)(sharedTexture) == 1;
    const bool sharedTextureDecremented = sharedTextureResult &&
        *reinterpret_cast<std::int32_t*>(sharedTexture + 0x54) == 1 &&
        g_textureDestroyCalls == 1;

    alignas(4) std::uint8_t corruptTexture[0x58]{};
    *reinterpret_cast<void**>(corruptTexture + 4) = corruptTexture;
    *reinterpret_cast<std::int32_t*>(corruptTexture + 0x54) = 1;
    const auto textureCallsBeforeReject = g_textureDestroyCalls;
    reinterpret_cast<DestroyFn>(kTextureDestroyAddress)(corruptTexture);
    const bool corruptTextureRejected =
        g_textureDestroyCalls == textureCallsBeforeReject &&
        *reinterpret_cast<std::int32_t*>(corruptTexture + 0x54) == 1;

    alignas(4) std::uint8_t finalTexture[0x58]{};
    alignas(4) std::uint8_t raster[0x34]{};
    alignas(4) std::uint8_t nextLink[8]{};
    alignas(4) std::uint8_t previousLink[8]{};
    *reinterpret_cast<void**>(finalTexture) = raster;
    *reinterpret_cast<void**>(finalTexture + 4) = finalTexture;
    *reinterpret_cast<void**>(finalTexture + 8) = nextLink;
    *reinterpret_cast<void**>(finalTexture + 0xC) = previousLink;
    *reinterpret_cast<void**>(nextLink + 4) = finalTexture + 8;
    *reinterpret_cast<void**>(previousLink) = finalTexture + 8;
    *reinterpret_cast<std::int32_t*>(finalTexture + 0x54) = 1;
    const auto textureCallsBeforeFinal = g_textureDestroyCalls;
    reinterpret_cast<DestroyFn>(kTextureDestroyAddress)(finalTexture);
    const bool finalTextureAccepted =
        g_textureDestroyCalls == textureCallsBeforeFinal + 1 &&
        *reinterpret_cast<std::int32_t*>(finalTexture + 0x54) == 0;

    alignas(4) std::uint8_t zeroRefTexture[0x58]{};
    const auto textureCallsBeforeZero = g_textureDestroyCalls;
    reinterpret_cast<DestroyFn>(kTextureDestroyAddress)(zeroRefTexture);
    const bool zeroTextureRefRejected =
        g_textureDestroyCalls == textureCallsBeforeZero;

    auto* readonlyList = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool readonlyListRejected{};
    if (readonlyList) {
        *reinterpret_cast<void***>(readonlyList) = corruptEntries;
        *reinterpret_cast<std::int32_t*>(readonlyList + 4) = 1;
        *reinterpret_cast<std::int32_t*>(readonlyList + 8) = 1;
        DWORD oldProtect{};
        if (VirtualProtect(readonlyList, 0x1000, PAGE_READONLY,
                           &oldProtect)) {
            const auto calls = g_materialListDeinitializeCalls;
            reinterpret_cast<ListDeinitializeFn>(
                kMaterialListDeinitializeAddress)(readonlyList);
            readonlyListRejected =
                g_materialListDeinitializeCalls == calls &&
                *reinterpret_cast<std::int32_t*>(readonlyList + 4) == 1;
        }
        VirtualFree(readonlyList, 0, MEM_RELEASE);
    }

    auto* boundaryPages = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool exactBoundaryHeaderAccepted{};
    if (boundaryPages) {
        DWORD oldProtect{};
        VirtualProtect(boundaryPages + 0x1000, 0x1000, PAGE_NOACCESS,
                       &oldProtect);
        auto* boundaryList = boundaryPages + 0x1000 - 12;
        std::memset(boundaryList, 0, 12);
        const auto calls = g_materialListDeinitializeCalls;
        reinterpret_cast<ListDeinitializeFn>(
            kMaterialListDeinitializeAddress)(boundaryList);
        exactBoundaryHeaderAccepted =
            g_materialListDeinitializeCalls == calls + 1;
        VirtualFree(boundaryPages, 0, MEM_RELEASE);
    }

    alignas(4) std::uint8_t geometry[0x60]{};
    geometry[0] = 8;
    *reinterpret_cast<std::int16_t*>(geometry + 0xE) = 2;
    reinterpret_cast<DestroyFn>(kGeometryDestroyAddress)(geometry);
    const bool sharedGeometryDecremented =
        g_geometryDestroyCalls == 1 &&
        *reinterpret_cast<std::int16_t*>(geometry + 0xE) == 1;

    alignas(4) std::uint8_t finalGeometry[0x60]{};
    alignas(4) std::uint8_t geometryMaterial[0x1C]{};
    void* geometryEntries[1]{geometryMaterial};
    finalGeometry[0] = 8;
    *reinterpret_cast<std::int16_t*>(finalGeometry + 0xE) = 1;
    *reinterpret_cast<void***>(finalGeometry + 0x20) = geometryEntries;
    *reinterpret_cast<std::int32_t*>(finalGeometry + 0x24) = 1;
    *reinterpret_cast<std::int32_t*>(finalGeometry + 0x28) = 1;
    *reinterpret_cast<std::int16_t*>(geometryMaterial + 0x18) = 1;
    const auto geometryCallsBeforeFinal = g_geometryDestroyCalls;
    const auto listCallsBeforeFinal = g_materialListDeinitializeCalls;
    const auto materialCallsBeforeFinal = g_materialDestroyCalls;
    reinterpret_cast<DestroyFn>(kGeometryDestroyAddress)(finalGeometry);
    const bool finalGeometryTraversed =
        g_geometryDestroyCalls == geometryCallsBeforeFinal + 1 &&
        g_materialListDeinitializeCalls == listCallsBeforeFinal + 1 &&
        g_materialDestroyCalls == materialCallsBeforeFinal + 1 &&
        *reinterpret_cast<void**>(finalGeometry + 0x20) == nullptr;

    alignas(4) std::uint8_t badGeometry[0x60]{};
    badGeometry[0] = 8;
    *reinterpret_cast<std::int16_t*>(badGeometry + 0xE) = 1;
    *reinterpret_cast<void***>(badGeometry + 0x20) = corruptEntries;
    *reinterpret_cast<std::int32_t*>(badGeometry + 0x24) = 2;
    *reinterpret_cast<std::int32_t*>(badGeometry + 0x28) = 1;
    const auto geometryCallsBeforeReject = g_geometryDestroyCalls;
    reinterpret_cast<DestroyFn>(kGeometryDestroyAddress)(badGeometry);
    const bool corruptGeometryRejected =
        g_geometryDestroyCalls == geometryCallsBeforeReject;

    alignas(4) std::uint8_t callbackGeometry[0x60]{};
    alignas(4) std::uint8_t fakeResourceEntry[0x18]{};
    callbackGeometry[0] = 8;
    *reinterpret_cast<std::int16_t*>(callbackGeometry + 0xE) = 1;
    *reinterpret_cast<void**>(callbackGeometry + 0x58) = fakeResourceEntry;
    *reinterpret_cast<void**>(fakeResourceEntry + 0x14) =
        reinterpret_cast<void*>(1);
    const auto geometryCallsBeforeCallback = g_geometryDestroyCalls;
    reinterpret_cast<DestroyFn>(kGeometryDestroyAddress)(callbackGeometry);
    const bool invalidResourceCallbackRejected =
        g_geometryDestroyCalls == geometryCallsBeforeCallback;

    if (inaccessible) VirtualFree(inaccessible, 0, MEM_RELEASE);
    return sharedMaterialResult && sharedMaterialDecremented &&
        nullRejected && guardPageRejected && rejectsSkippedNative &&
        negativeRefRejected &&
        mixedListPreservedLifecycle && corruptListDetached &&
        sharedTextureDecremented && corruptTextureRejected &&
        finalTextureAccepted && zeroTextureRefRejected &&
        readonlyListRejected && exactBoundaryHeaderAccepted &&
        sharedGeometryDecremented && finalGeometryTraversed &&
        corruptGeometryRejected && invalidResourceCallbackRejected;
}
}

int wmain(int argc, wchar_t** argv) {
    g_mockSpan[0] = 0;
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: mock_host.exe <RenderAlphaFix.asi>\n");
        return 2;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto mainLoopAddress = reinterpret_cast<std::uintptr_t>(base) + kMainLoopCallRva;
    const auto timerAddress = reinterpret_cast<std::uintptr_t>(base) + kTimerUpdateRva;
    if (!AllocateCodePage(mainLoopAddress) || !AllocateCodePage(timerAddress) ||
        !PrepareSphereGuardSites() || !PrepareDestructionGuardSites()) {
        std::fwprintf(stderr, L"unable to allocate mock RVA pages (base=%p)\n", base);
        return 3;
    }

    // inc dword ptr [g_timerCalls]; ret
    std::uint8_t timerCode[] = {0xFF, 0x05, 0, 0, 0, 0, 0xC3};
    const auto counterAddress = reinterpret_cast<std::uintptr_t>(&g_timerCalls);
    std::memcpy(timerCode + 2, &counterAddress, sizeof(counterAddress));
    std::memcpy(reinterpret_cast<void*>(timerAddress), timerCode, sizeof(timerCode));

    std::uint8_t mainLoopCall[] = {0xE8, 0, 0, 0, 0, 0xC3};
    const auto relativeTimer = static_cast<std::int32_t>(timerAddress - (mainLoopAddress + 5));
    std::memcpy(mainLoopCall + 1, &relativeTimer, sizeof(relativeTimer));
    std::memcpy(reinterpret_cast<void*>(mainLoopAddress), mainLoopCall, sizeof(mainLoopCall));

    HMODULE asi = LoadLibraryW(argv[1]);
    if (!asi) {
        std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 4;
    }

    const bool loopHooked = *reinterpret_cast<const std::uint8_t*>(mainLoopAddress) == 0xE8 &&
        std::memcmp(reinterpret_cast<const void*>(mainLoopAddress), mainLoopCall, 5) != 0;
    if (!loopHooked) {
        std::wprintf(L"mock host: ASI loaded; GameLoop call-site not hooked\n");
        return 5;
    }

    using MainLoopFn = void(__stdcall*)();
    reinterpret_cast<MainLoopFn>(mainLoopAddress)();
    const bool timerChained = g_timerCalls == 1;
    const bool sphereGuards = ProbeSphereGuards();
    const bool destructionGuards = ProbeDestructionGuards();
    std::wprintf(L"mock host: gameloop_hook=PASS timer_chain=%ls "
                 L"sphere_guards=%ls destruction_guards=%ls "
                 L"synthetic_selftest=see_log\n",
                 timerChained ? L"PASS" : L"FAIL",
                 sphereGuards ? L"PASS" : L"FAIL",
                 destructionGuards ? L"PASS" : L"FAIL");
    return timerChained && sphereGuards && destructionGuards ? 0 : 6;
}
