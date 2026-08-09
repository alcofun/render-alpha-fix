#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

#pragma section(".mock", read, write, execute)
__declspec(allocate(".mock")) volatile unsigned char g_mockSpan[0x400000];
volatile std::uint32_t g_renderCalls{};

namespace {
constexpr std::uintptr_t kRenderRva = 0x332480;
constexpr std::uintptr_t kDefaultRenderRva = 0x3491C0;
constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;
constexpr std::uint8_t kEntry[] = {
    0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xAC, 0x24
};

bool AllocateCodePage(std::uintptr_t address) {
    const auto page = address & ~static_cast<std::uintptr_t>(0xFFF);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT) {
        DWORD oldProtect{};
        return VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                              PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE;
    }
    return VirtualAlloc(reinterpret_cast<void*>(page), 0x1000, MEM_RESERVE | MEM_COMMIT,
                        PAGE_EXECUTE_READWRITE) != nullptr;
}
}

int wmain(int argc, wchar_t** argv) {
    g_mockSpan[0] = 0;
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: mock_host.exe <RenderAlphaFix.asi>\n");
        return 2;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto entryAddress = reinterpret_cast<std::uintptr_t>(base) + kRenderRva;
    const auto callbackAddress = reinterpret_cast<std::uintptr_t>(base) + kDefaultRenderRva;
    const auto mainLoopAddress = reinterpret_cast<std::uintptr_t>(base) + kMainLoopCallRva;
    const auto timerAddress = reinterpret_cast<std::uintptr_t>(base) + kTimerUpdateRva;
    if (!AllocateCodePage(entryAddress) || !AllocateCodePage(callbackAddress) ||
        !AllocateCodePage(mainLoopAddress) || !AllocateCodePage(timerAddress)) {
        std::fwprintf(stderr, L"unable to allocate mock RVA pages (base=%p)\n", base);
        return 3;
    }

    std::memcpy(reinterpret_cast<void*>(entryAddress), kEntry, sizeof(kEntry));
    // inc dword ptr [g_renderCalls]; mov eax,[esp+4]; ret
    std::uint8_t callbackCode[] = {0xFF, 0x05, 0, 0, 0, 0, 0x8B, 0x44, 0x24, 0x04, 0xC3};
    const auto counterAddress = reinterpret_cast<std::uintptr_t>(&g_renderCalls);
    std::memcpy(callbackCode + 2, &counterAddress, sizeof(counterAddress));
    std::memcpy(reinterpret_cast<void*>(callbackAddress), callbackCode, sizeof(callbackCode));

    std::uint8_t mainLoopCall[] = {0xE8, 0, 0, 0, 0, 0xC3};
    const auto relativeTimer = static_cast<std::int32_t>(timerAddress - (mainLoopAddress + 5));
    std::memcpy(mainLoopCall + 1, &relativeTimer, sizeof(relativeTimer));
    std::memcpy(reinterpret_cast<void*>(mainLoopAddress), mainLoopCall, sizeof(mainLoopCall));
    *reinterpret_cast<std::uint8_t*>(timerAddress) = 0xC3;

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

    const bool hooked = *reinterpret_cast<const std::uint8_t*>(entryAddress) == 0xE9;
    if (!hooked) {
        std::wprintf(L"mock host: GameLoop ran; render entry not hooked\n");
        return 5;
    }

    alignas(4) std::uint8_t material[0x1C]{};
    alignas(4) std::uint8_t geometry[0x60]{};
    alignas(4) std::uint8_t atomic[0x70]{};
    auto* materialPointer = material;
    *reinterpret_cast<std::uint8_t***>(geometry + 0x20) = &materialPointer;
    *reinterpret_cast<std::int32_t*>(geometry + 0x24) = 1;
    *reinterpret_cast<std::int32_t*>(geometry + 0x28) = 1;
    *reinterpret_cast<std::uint32_t*>(geometry + 0x08) = 0x1234;
    *reinterpret_cast<std::uint8_t**>(atomic + 0x18) = geometry;
    material[0x07] = 200;

    using RenderFn = void(__cdecl*)(void*, int);
    auto render = reinterpret_cast<RenderFn>(entryAddress);
    render(atomic, 128); // one-shot in-game self-test intentionally skips this call
    const bool inGameSelfTest = g_renderCalls == 0 && material[0x07] == 200;
    render(atomic, 128);
    const bool restored = g_renderCalls == 1 && material[0x07] == 200 &&
                          *reinterpret_cast<std::uint32_t*>(geometry + 0x08) == 0x1234;

    materialPointer = reinterpret_cast<std::uint8_t*>(1);
    render(atomic, 128);
    const bool rejectedLow = g_renderCalls == 1;
    materialPointer = reinterpret_cast<std::uint8_t*>(0xC1000000);
    render(atomic, 128);
    const bool rejectedHigh = g_renderCalls == 1;
    std::wprintf(L"mock host: gameloop=PASS hook=PASS in_game_selftest=%ls valid_restore=%ls "
                 L"invalid_pointer_1=%ls invalid_pointer_C1000000=%ls\n",
                 inGameSelfTest ? L"PASS" : L"FAIL", restored ? L"PASS" : L"FAIL",
                 rejectedLow ? L"PASS" : L"FAIL", rejectedHigh ? L"PASS" : L"FAIL");
    return inGameSelfTest && restored && rejectedLow && rejectedHigh ? 0 : 6;
}
