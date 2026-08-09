#include <windows.h>

#include <kthook/kthook.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>

namespace {

constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;
constexpr std::size_t kObjectGameEntityOffset = 0x40;
constexpr std::size_t kObjectMaterialPointerOffset = 0x217;
constexpr std::size_t kObjectMaterialTypeOffset = 0x2DB;
constexpr std::size_t kObjectTextureCreatedOffset = 0x31B;
constexpr std::size_t kObjectPoolFlagsOffset = 0x04;
constexpr std::size_t kObjectPoolPointersOffset = 0xFA4;
constexpr std::int32_t kSampMaxObjects = 1000;
constexpr std::uint32_t kDefaultScanBudget = 32;
constexpr std::uint32_t kDefaultRetryCooldownMs = 10000;

using ObjectPoolProcessFn = void(__thiscall*)(void*);
using ConstructMaterialTextFn = void(__thiscall*)(void*);
using dMainLoop = void(__stdcall*)();
using MainLoopHook = kthook::kthook_simple<dMainLoop>;
using ObjectPoolHook = kthook::kthook_simple<ObjectPoolProcessFn>;

HMODULE g_module{};
HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;
MainLoopHook* g_mainLoopHook{};
ObjectPoolHook* g_objectPoolHook{};
dMainLoop g_timerUpdate{};
std::uint8_t* g_sampBase{};
ConstructMaterialTextFn g_constructMaterialText{};
void* g_objectPool{};
DWORD g_lastSampProbeTick{};
DWORD g_objectRepairTick[kSampMaxObjects]{};
std::int32_t g_repairCursor{};
std::uint32_t g_scanBudget = kDefaultScanBudget;
std::uint32_t g_retryCooldownMs = kDefaultRetryCooldownMs;
bool g_logRepairs = true;
bool g_sampProbeLogged{};
bool g_sampRepairUnsupported{};

template <typename HookType>
struct HookReturnGuard {
    const HookType& hook;
    std::uintptr_t returnAddress;

    explicit HookReturnGuard(const HookType& current) noexcept
        : hook(current), returnAddress(current.get_return_address()) {}

    ~HookReturnGuard() noexcept {
        hook.get_return_address() = returnAddress;
    }
};

void Log(const char* format, ...) noexcept {
    if (g_log == INVALID_HANDLE_VALUE) return;

    char message[1024]{};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    int prefix = std::snprintf(message, sizeof(message),
        "[%02u:%02u:%02u.%03u] [tid=%lu] ", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, GetCurrentThreadId());
    if (prefix < 0 || static_cast<std::size_t>(prefix) >= sizeof(message)) return;

    va_list args;
    va_start(args, format);
    int body = _vsnprintf_s(message + prefix, sizeof(message) - prefix,
                            _TRUNCATE, format, args);
    va_end(args);
    if (body < 0) body = static_cast<int>(std::strlen(message + prefix));

    std::size_t length = static_cast<std::size_t>(prefix + body);
    if (length + 2 >= sizeof(message)) length = sizeof(message) - 3;
    message[length++] = '\r';
    message[length++] = '\n';

    AcquireSRWLockExclusive(&g_logLock);
    DWORD written{};
    WriteFile(g_log, message, static_cast<DWORD>(length), &written, nullptr);
    ReleaseSRWLockExclusive(&g_logLock);
}

int MemoryAccessFilter(DWORD code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool SafeReadPointer(const void* address, void** value) noexcept {
    __try {
        *value = *reinterpret_cast<void* const*>(address);
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

bool SafeReadI32(const void* address, std::int32_t* value) noexcept {
    __try {
        *value = *reinterpret_cast<const std::int32_t*>(address);
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

bool HasMalformedTextSlot(void* object) noexcept {
    if (reinterpret_cast<std::uintptr_t>(object) < 0x10000) return false;
    auto* bytes = static_cast<std::uint8_t*>(object);
    for (std::int32_t slot = 0; slot < 16; ++slot) {
        std::int32_t type{};
        if (!SafeReadI32(bytes + kObjectMaterialTypeOffset + slot * 4, &type))
            return false;
        if (type != 2) continue;

        std::int32_t textureCreated{};
        if (!SafeReadI32(bytes + kObjectTextureCreatedOffset + slot * 4,
                         &textureCreated)) return false;
        if (textureCreated != 0) continue;

        void* materialPointer{};
        if (!SafeReadPointer(bytes + kObjectMaterialPointerOffset + slot * 4,
                             &materialPointer)) return false;
        if (type == 2 && textureCreated == 0 && materialPointer != nullptr) return true;
    }
    return false;
}

bool SyntheticTests() noexcept {
    alignas(4) std::uint8_t object[0x400]{};
    constexpr std::int32_t slot = 7;
    *reinterpret_cast<std::int32_t*>(object + kObjectMaterialTypeOffset + slot * 4) = 2;
    *reinterpret_cast<void**>(object + kObjectMaterialPointerOffset + slot * 4) =
        reinterpret_cast<void*>(1);
    const bool malformedDetected = HasMalformedTextSlot(object);
    *reinterpret_cast<std::int32_t*>(object + kObjectTextureCreatedOffset + slot * 4) = 1;
    const bool healthyIgnored = !HasMalformedTextSlot(object);
    *reinterpret_cast<std::int32_t*>(object + kObjectTextureCreatedOffset + slot * 4) = 0;
    *reinterpret_cast<void**>(object + kObjectMaterialPointerOffset + slot * 4) = nullptr;
    const bool emptyIgnored = !HasMalformedTextSlot(object);
    const bool passed = malformedDetected && healthyIgnored && emptyIgnored;
    Log("SYNTHETIC SELFTEST %s: SA-MP material-text invariant", passed ? "PASS" : "FAIL");
    return passed;
}

struct ParsedPattern {
    std::uint8_t bytes[128]{};
    bool exact[128]{};
    std::size_t length{};
};

int HexNibble(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool ParsePattern(const char* text, ParsedPattern* pattern) noexcept {
    while (*text) {
        while (*text == ' ') ++text;
        if (!*text) break;
        if (pattern->length == sizeof(pattern->bytes)) return false;
        if (*text == '?') {
            pattern->exact[pattern->length++] = false;
            while (*text == '?') ++text;
        } else {
            const int high = HexNibble(text[0]);
            const int low = HexNibble(text[1]);
            if (high < 0 || low < 0) return false;
            pattern->bytes[pattern->length] =
                static_cast<std::uint8_t>((high << 4) | low);
            pattern->exact[pattern->length++] = true;
            text += 2;
        }
        while (*text == ' ') ++text;
    }
    return pattern->length != 0;
}

bool ProtectionReadable(DWORD protect) noexcept {
    protect &= 0xFF;
    return protect == PAGE_READONLY || protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool CheckRange(const void* pointer, std::size_t size) noexcept {
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start < 0x10000 || size == 0 || start > UINTPTR_MAX - size) return false;
    const auto end = start + size;
    auto cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) !=
            sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !ProtectionReadable(mbi.Protect)) return false;
        const auto regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (regionStart > UINTPTR_MAX - mbi.RegionSize) return false;
        const auto regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = regionEnd < end ? regionEnd : end;
    }
    return true;
}

std::uint8_t* FindUniqueExecutablePattern(std::uint8_t* module,
                                          const char* signature) noexcept {
    ParsedPattern pattern{};
    if (!module || !ParsePattern(signature, &pattern)) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (!CheckRange(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    if (!CheckRange(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    std::uint8_t* match{};
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::size_t size = section->Misc.VirtualSize;
        auto* start = module + section->VirtualAddress;
        if (size < pattern.length || !CheckRange(start, size)) continue;
        for (std::size_t offset = 0; offset <= size - pattern.length; ++offset) {
            bool equal = true;
            for (std::size_t byte = 0; byte < pattern.length; ++byte) {
                if (pattern.exact[byte] && start[offset + byte] != pattern.bytes[byte]) {
                    equal = false;
                    break;
                }
            }
            if (!equal) continue;
            if (match) return nullptr;
            match = start + offset;
        }
    }
    return match;
}

void RepairSampObjects() noexcept {
    if (!g_constructMaterialText || !g_objectPool) return;

    std::int32_t largest{};
    if (!SafeReadI32(g_objectPool, &largest) || largest < 0) return;
    if (largest >= kSampMaxObjects) largest = kSampMaxObjects - 1;
    if (g_repairCursor > largest) g_repairCursor = 0;

    const DWORD now = GetTickCount();
    for (std::uint32_t scanned = 0;
         scanned < g_scanBudget && scanned <= static_cast<std::uint32_t>(largest);
         ++scanned) {
        const std::int32_t id = g_repairCursor;
        g_repairCursor = g_repairCursor >= largest ? 0 : g_repairCursor + 1;

        auto* pool = static_cast<std::uint8_t*>(g_objectPool);
        std::int32_t occupied{};
        void* object{};
        if (!SafeReadI32(pool + kObjectPoolFlagsOffset + id * sizeof(std::int32_t),
                         &occupied) ||
            !occupied ||
            !SafeReadPointer(pool + kObjectPoolPointersOffset + id * sizeof(void*),
                             &object) ||
            !object) continue;

        void* gameEntity{};
        if (!SafeReadPointer(static_cast<std::uint8_t*>(object) + kObjectGameEntityOffset,
                             &gameEntity) ||
            !gameEntity || !HasMalformedTextSlot(object)) continue;
        if (g_objectRepairTick[id] && now - g_objectRepairTick[id] < g_retryCooldownMs)
            continue;

        __try {
            g_constructMaterialText(object);
            g_objectRepairTick[id] = now ? now : 1;
            if (g_logRepairs)
                Log("SA-MP OBJECT REPAIR rebuilt malformed object id=%ld", id);
        } __except (MemoryAccessFilter(GetExceptionCode())) {
            Log("SA-MP OBJECT REPAIR faulted for object id=%ld; repair disabled", id);
            g_constructMaterialText = nullptr;
        }
        break;
    }
}

void ObjectPoolProcessReplacement(const ObjectPoolHook& hook, void*& pool) {
    HookReturnGuard<ObjectPoolHook> returnGuard(hook);
    g_objectPool = pool;
    hook.get_trampoline()(pool);
    RepairSampObjects();
}

void InitializeSampRepair() noexcept {
    g_sampBase = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"samp.dll"));
    if (!g_sampBase) {
        if (!g_sampProbeLogged) {
            Log("SA-MP OBJECT REPAIR waiting for samp.dll");
            g_sampProbeLogged = true;
        }
        return;
    }

    constexpr const char* constructSignature =
        "83 EC 08 53 55 56 8B E9 57 8D BD DB 02 00 00 8D B5 31 04 00 00 "
        "BB 10 00 00 00 8D 9B 00 00 00 00 8B 87 3C FF FF FF 85 C0 74 6A "
        "83 3F 02 75 65 8B 47 40 85 C0 75 5E 0F B6 56 B4";
    constexpr const char* processSignature =
        "51 A1 ? ? ? ? 85 C0 53 55 57 8B D9 75 0A E8 ? ? ? ? A3 ? ? ? ? "
        "E8 ? ? ? ? 8B 0D ? ? ? ? 8B E8 2B C1 89 44 24 0C DB 44 24 0C "
        "8B 03 33 FF 85 C0 D8 0D ? ? ? ? D9 5C 24 0C 7C 2F 56 8D B3 A4 0F 00 00";

    auto* construct = FindUniqueExecutablePattern(g_sampBase, constructSignature);
    auto* process = FindUniqueExecutablePattern(g_sampBase, processSignature);
    if (!construct || !process) {
        Log("SA-MP OBJECT REPAIR disabled: unique R1/R3-compatible signatures not found");
        g_sampBase = nullptr;
        g_sampRepairUnsupported = true;
        return;
    }

    g_constructMaterialText = reinterpret_cast<ConstructMaterialTextFn>(construct);
    g_objectPoolHook = new (std::nothrow) ObjectPoolHook(
        reinterpret_cast<ObjectPoolProcessFn>(process), &ObjectPoolProcessReplacement, false);
    if (!g_objectPoolHook || !g_objectPoolHook->install()) {
        delete g_objectPoolHook;
        g_objectPoolHook = nullptr;
        g_constructMaterialText = nullptr;
        g_sampBase = nullptr;
        g_sampRepairUnsupported = true;
        Log("SA-MP OBJECT REPAIR disabled: object-pool hook installation failed");
        return;
    }
    Log("SA-MP OBJECT REPAIR enabled by signatures: process=%p construct=%p; "
        "scanBudget=%lu retryCooldownMs=%lu", process, construct,
        static_cast<unsigned long>(g_scanBudget),
        static_cast<unsigned long>(g_retryCooldownMs));
}

void ProbeSampRepair() noexcept {
    if (g_objectPoolHook || g_sampRepairUnsupported) return;
    const DWORD now = GetTickCount();
    if (now - g_lastSampProbeTick < 1000) return;
    g_lastSampProbeTick = now;
    InitializeSampRepair();
}

std::wstring ModuleDirectory() {
    std::wstring path(512, L'\0');
    while (path.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(g_module, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0) return L".\\";
        if (length < path.size() - 1) {
            path.resize(length);
            const auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L".\\" : path.substr(0, slash + 1);
        }
        path.resize(path.size() * 2);
    }
    return L".\\";
}

void OpenLog() {
    if (g_log != INVALID_HANDLE_VALUE) return;
    const auto logPath = ModuleDirectory() + L"RenderAlphaFix.log";
    g_log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void LoadConfig() {
    const auto iniPath = ModuleDirectory() + L"RenderAlphaFix.ini";
    g_scanBudget = static_cast<std::uint32_t>(GetPrivateProfileIntW(
        L"Repair", L"ScanBudget", kDefaultScanBudget, iniPath.c_str()));
    if (g_scanBudget < 1 || g_scanBudget > 256) g_scanBudget = kDefaultScanBudget;
    g_retryCooldownMs = static_cast<std::uint32_t>(GetPrivateProfileIntW(
        L"Repair", L"RetryCooldownMs", kDefaultRetryCooldownMs, iniPath.c_str()));
    if (g_retryCooldownMs < 1000 || g_retryCooldownMs > 60000)
        g_retryCooldownMs = kDefaultRetryCooldownMs;
    g_logRepairs = GetPrivateProfileIntW(L"Repair", L"LogRepairs", 1,
                                         iniPath.c_str()) != 0;
}

void Initialize() {
    LoadConfig();
    Log("RenderAlphaFix 1.1 initializing from GTA GameLoop; mode=SA-MP object repair only");

    HMODULE pinnedModule{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(g_module), &pinnedModule)) {
        Log("FATAL: unable to pin ASI module; repair not installed");
        return;
    }

    SyntheticTests();
    InitializeSampRepair();
}

void MainLoopReplacement(const MainLoopHook& hook) {
    HookReturnGuard<MainLoopHook> returnGuard(hook);
    static bool init = false;
    if (!init) {
        init = true;
        Initialize();
    }
    ProbeSampRepair();
    g_timerUpdate();
}

bool InstallMainLoopHook() noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;

    auto* callSite = base + kMainLoopCallRva;
    if (callSite[0] != 0xE8) {
        Log("FATAL: GameLoop site gta_sa.exe+0x13E968 is not CALL: "
            "bytes=%02X %02X %02X %02X %02X", callSite[0], callSite[1],
            callSite[2], callSite[3], callSite[4]);
        return false;
    }

    std::int32_t relative{};
    std::memcpy(&relative, callSite + 1, sizeof(relative));
    auto* callTarget = callSite + 5 + relative;
    if (!CheckRange(callTarget, 1)) {
        Log("FATAL: existing GameLoop CALL target is not readable: %p", callTarget);
        return false;
    }

    g_timerUpdate = reinterpret_cast<dMainLoop>(callTarget);
    g_mainLoopHook = new (std::nothrow) MainLoopHook(
        reinterpret_cast<dMainLoop>(callSite), &MainLoopReplacement, false);
    if (!g_mainLoopHook || !g_mainLoopHook->install()) {
        delete g_mainLoopHook;
        g_mainLoopHook = nullptr;
        Log("FATAL: kthook failed to install GameLoop call-site hook at %p", callSite);
        return false;
    }
    Log("GAMELOOP HOOK INSTALLED at %p; previous target=%p; mode=%s", callSite,
        callTarget, callTarget == base + kTimerUpdateRva ? "original" : "chained");
    return true;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        OpenLog();
        Log("RenderAlphaFix 1.1 ASI LOADED in DllMain; installing GameLoop hook");
        if (!InstallMainLoopHook()) {
            Log("FATAL: GameLoop hook installation failed; SA-MP repair is inactive");
            OutputDebugStringA("RenderAlphaFix: failed to install GameLoop hook at 0x53E968\n");
        }
    }
    return TRUE;
}
