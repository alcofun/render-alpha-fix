#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

#pragma section(".mock", read, write, execute)
__declspec(allocate(".mock")) volatile unsigned char g_mockSpan[0x380000];
volatile std::uint32_t g_timerCalls{};

namespace {
constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;
constexpr std::uintptr_t kAtomicSetGeometrySphereCopyAddress = 0x749D7B;
constexpr std::uintptr_t kAtomicSetGeometryAfterSphereCopyAddress = 0x749D9A;
constexpr std::uintptr_t kAtomicCloneSphereCopyAddress = 0x749EDB;
constexpr std::uintptr_t kAtomicCloneAfterMorphSphereCopyAddress = 0x749EFA;

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
        !PrepareSphereGuardSites()) {
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
    std::wprintf(L"mock host: gameloop_hook=PASS timer_chain=%ls "
                 L"sphere_guards=%ls synthetic_selftest=see_log\n",
                 timerChained ? L"PASS" : L"FAIL",
                 sphereGuards ? L"PASS" : L"FAIL");
    return timerChained && sphereGuards ? 0 : 6;
}
