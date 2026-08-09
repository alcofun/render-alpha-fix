#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

#pragma section(".mock", read, write, execute)
__declspec(allocate(".mock")) volatile unsigned char g_mockSpan[0x200000];
volatile std::uint32_t g_timerCalls{};

namespace {
constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;

bool AllocateCodePage(std::uintptr_t address) {
    const auto page = address & ~static_cast<std::uintptr_t>(0xFFF);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT) {
        DWORD oldProtect{};
        return VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                              PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE;
    }
    return VirtualAlloc(reinterpret_cast<void*>(page), 0x1000,
                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) != nullptr;
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
    if (!AllocateCodePage(mainLoopAddress) || !AllocateCodePage(timerAddress)) {
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
    std::wprintf(L"mock host: gameloop_hook=PASS timer_chain=%ls synthetic_selftest=see_log\n",
                 timerChained ? L"PASS" : L"FAIL");
    return timerChained ? 0 : 6;
}
