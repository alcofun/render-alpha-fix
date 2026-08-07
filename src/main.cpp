#include <windows.h>

#include <kthook/kthook.hpp>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <new>
#include <string>

namespace {

constexpr std::uintptr_t kRenderAlphaAtomicRva = 0x332480;
constexpr std::uintptr_t kAtomicDefaultRenderRva = 0x3491C0;
constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;
constexpr std::uint8_t kExpectedEntry[] = {
    0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xAC, 0x24
};

constexpr std::size_t kAtomicGeometryOffset = 0x18;
constexpr std::size_t kGeometryFlagsOffset = 0x08;
constexpr std::size_t kGeometryMaterialsOffset = 0x20;
constexpr std::size_t kGeometryCountOffset = 0x24;
constexpr std::size_t kGeometrySpaceOffset = 0x28;
constexpr std::size_t kMaterialAlphaOffset = 0x07;
constexpr std::size_t kMaterialSize = 0x1C;
constexpr std::uint32_t kRenderFlag = 0x40;
constexpr std::size_t kInlineSnapshots = 152;
constexpr std::uint32_t kDefaultMaxMaterials = 4096;

using RenderAlphaAtomicFn = void(__cdecl*)(void*, int);
using AtomicDefaultRenderFn = void*(__cdecl*)(void*);
using dMainLoop = void(__stdcall*)();
using RenderHook = kthook::kthook_simple<RenderAlphaAtomicFn>;
using MainLoopHook = kthook::kthook_simple<dMainLoop>;

enum class RejectReason : std::uint32_t {
    None,
    NullAtomic,
    InvalidAtomic,
    NullGeometry,
    InvalidGeometry,
    NegativeCount,
    CountExceedsSpace,
    TooManyMaterials,
    InvalidMaterialArray,
    InvalidMaterial,
    AllocationFailed,
    WriteFailed,
};

struct Snapshot {
    std::uint8_t* material;
    std::uint8_t alpha;
};

struct GeometryView {
    std::uint8_t* geometry;
    std::uint8_t** materials;
    std::int32_t count;
    std::int32_t space;
    std::uint32_t flags;
};

struct PreflightResult {
    RejectReason reason;
    std::int32_t badIndex;
    std::uint8_t* badMaterial;
};

HMODULE g_module{};
HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;
RenderHook* g_hook{};
MainLoopHook* g_mainLoopHook{};
dMainLoop g_timerUpdate{};
AtomicDefaultRenderFn g_defaultRender{};
std::uint32_t g_maxMaterials = kDefaultMaxMaterials;
bool g_logRejected = true;
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_rendered{0};
std::atomic<std::uint64_t> g_rejected{0};
std::atomic_bool g_inGameSelfTestPending{true};

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

const char* ReasonName(RejectReason reason) noexcept {
    switch (reason) {
    case RejectReason::None: return "none";
    case RejectReason::NullAtomic: return "null_atomic";
    case RejectReason::InvalidAtomic: return "invalid_atomic";
    case RejectReason::NullGeometry: return "null_geometry";
    case RejectReason::InvalidGeometry: return "invalid_geometry";
    case RejectReason::NegativeCount: return "negative_count";
    case RejectReason::CountExceedsSpace: return "count_exceeds_space";
    case RejectReason::TooManyMaterials: return "too_many_materials";
    case RejectReason::InvalidMaterialArray: return "invalid_material_array";
    case RejectReason::InvalidMaterial: return "invalid_material";
    case RejectReason::AllocationFailed: return "allocation_failed";
    case RejectReason::WriteFailed: return "write_failed";
    }
    return "unknown";
}

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
    FlushFileBuffers(g_log);
    ReleaseSRWLockExclusive(&g_logLock);
}

bool ProtectionReadable(DWORD protect) noexcept {
    protect &= 0xFF;
    return protect == PAGE_READONLY || protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool ProtectionWritable(DWORD protect) noexcept {
    protect &= 0xFF;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool CheckRange(const void* pointer, std::size_t size, bool writable) noexcept {
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start < 0x10000 || size == 0) return false;
    if (start > UINTPTR_MAX - size) return false;
    const auto end = start + size;
    auto cursor = start;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        if (writable ? !ProtectionWritable(mbi.Protect) : !ProtectionReadable(mbi.Protect))
            return false;
        const auto regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (regionStart > UINTPTR_MAX - mbi.RegionSize) return false;
        const auto regionEnd = regionStart + mbi.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = regionEnd < end ? regionEnd : end;
    }
    return true;
}

void* OffsetPointer(const void* pointer, std::size_t offset) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(pointer);
    if (base > UINTPTR_MAX - offset) return nullptr;
    return reinterpret_cast<void*>(base + offset);
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

bool SafeReadU32(const void* address, std::uint32_t* value) noexcept {
    __try {
        *value = *reinterpret_cast<const std::uint32_t*>(address);
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

bool SafeReadU8(const void* address, std::uint8_t* value) noexcept {
    __try {
        *value = *reinterpret_cast<const std::uint8_t*>(address);
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

bool SafeWriteU32(void* address, std::uint32_t value) noexcept {
    __try {
        *reinterpret_cast<std::uint32_t*>(address) = value;
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

bool SafeWriteU8(void* address, std::uint8_t value) noexcept {
    __try {
        *reinterpret_cast<std::uint8_t*>(address) = value;
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

RejectReason ReadGeometry(void* atomic, GeometryView* view) noexcept {
    if (!atomic) return RejectReason::NullAtomic;
    void* geometryField = OffsetPointer(atomic, kAtomicGeometryOffset);
    if (!geometryField || !CheckRange(geometryField, sizeof(void*), false))
        return RejectReason::InvalidAtomic;

    void* geometry{};
    if (!SafeReadPointer(geometryField, &geometry))
        return RejectReason::InvalidAtomic;
    if (!geometry) return RejectReason::NullGeometry;

    auto* geometryBytes = static_cast<std::uint8_t*>(geometry);
    void* geometryRange = OffsetPointer(geometry, kGeometryFlagsOffset);
    void* flagsField = geometryRange;
    void* materialsField = OffsetPointer(geometry, kGeometryMaterialsOffset);
    void* countField = OffsetPointer(geometry, kGeometryCountOffset);
    void* spaceField = OffsetPointer(geometry, kGeometrySpaceOffset);
    if (!geometryRange || !materialsField || !countField || !spaceField ||
        !CheckRange(geometryRange,
                    kGeometrySpaceOffset + sizeof(std::int32_t) - kGeometryFlagsOffset, false))
        return RejectReason::InvalidGeometry;

    void* materials{};
    if (!SafeReadU32(flagsField, &view->flags) ||
        !SafeReadPointer(materialsField, &materials) ||
        !SafeReadI32(countField, &view->count) ||
        !SafeReadI32(spaceField, &view->space))
        return RejectReason::InvalidGeometry;

    view->geometry = geometryBytes;
    view->materials = static_cast<std::uint8_t**>(materials);
    return RejectReason::None;
}

PreflightResult BuildSnapshots(const GeometryView& view, Snapshot* snapshots) noexcept {
    if (view.count < 0) return {RejectReason::NegativeCount, -1, nullptr};
    if (view.count == 0) return {RejectReason::None, -1, nullptr};
    if (view.space < view.count) return {RejectReason::CountExceedsSpace, -1, nullptr};
    if (static_cast<std::uint32_t>(view.count) > g_maxMaterials)
        return {RejectReason::TooManyMaterials, -1, nullptr};

    const auto bytes = static_cast<std::size_t>(view.count) * sizeof(void*);
    if (!view.materials || !CheckRange(view.materials, bytes, false))
        return {RejectReason::InvalidMaterialArray, -1, nullptr};

    for (std::int32_t index = 0; index < view.count; ++index) {
        void* rawMaterial{};
        if (!SafeReadPointer(view.materials + index, &rawMaterial))
            return {RejectReason::InvalidMaterialArray, index, nullptr};

        auto* material = static_cast<std::uint8_t*>(rawMaterial);

        void* alphaField = OffsetPointer(material, kMaterialAlphaOffset);
        if (!material || !alphaField || !CheckRange(material, kMaterialSize, false) ||
            !CheckRange(alphaField, 1, true)) {
            return {RejectReason::InvalidMaterial, index, material};
        }

        std::uint8_t alpha{};
        if (!SafeReadU8(alphaField, &alpha))
            return {RejectReason::InvalidMaterial, index, material};
        snapshots[index] = {material, alpha};
    }
    return {RejectReason::None, -1, nullptr};
}

void RestoreState(const GeometryView* view, Snapshot* snapshots,
                  std::int32_t count) noexcept {
    for (std::int32_t index = count - 1; index >= 0; --index) {
        if (void* alphaField = OffsetPointer(snapshots[index].material, kMaterialAlphaOffset))
            SafeWriteU8(alphaField, snapshots[index].alpha);
    }
    if (void* flagsField = OffsetPointer(view->geometry, kGeometryFlagsOffset))
        SafeWriteU32(flagsField, view->flags);
}

void RenderWithFinally(void* atomic, const GeometryView* view,
                       Snapshot* snapshots, bool heapAllocated) {
    __try {
        g_defaultRender(atomic);
    } __finally {
        RestoreState(view, snapshots, view->count);
        if (heapAllocated) HeapFree(GetProcessHeap(), 0, snapshots);
    }
}

void LogRejection(const GeometryView* view, const PreflightResult& result,
                  void* atomic) noexcept {
    const auto rejected = ++g_rejected;
    if (!g_logRejected || (rejected > 20 && rejected % 1000 != 0)) return;
    Log("REJECT reason=%s atomic=%p geometry=%p materials=%p count=%ld space=%ld "
        "badIndex=%ld badMaterial=%p rejected=%llu",
        ReasonName(result.reason), atomic, view ? view->geometry : nullptr,
        view ? view->materials : nullptr, view ? view->count : -1,
        view ? view->space : -1, result.badIndex, result.badMaterial,
        static_cast<unsigned long long>(rejected));
}

Snapshot* AcquireSnapshots(std::int32_t count, Snapshot* inlineStorage,
                           std::size_t inlineCount, bool* heapAllocated) noexcept {
    *heapAllocated = false;
    if (count <= static_cast<std::int32_t>(inlineCount)) return inlineStorage;
    if (count < 0 || static_cast<std::uint32_t>(count) > g_maxMaterials) return nullptr;
    *heapAllocated = true;
    return static_cast<Snapshot*>(HeapAlloc(GetProcessHeap(), 0,
        static_cast<std::size_t>(count) * sizeof(Snapshot)));
}

void RenderAlphaReplacement(const RenderHook& hook, void*& atomic, int& requestedAlpha) {
    HookReturnGuard<RenderHook> returnGuard(hook);
    ++g_calls;
    GeometryView view{};
    const auto headerReason = ReadGeometry(atomic, &view);
    if (headerReason != RejectReason::None) {
        PreflightResult result{headerReason, -1, nullptr};
        LogRejection(nullptr, result, atomic);
        return;
    }

    Snapshot inlineSnapshots[kInlineSnapshots];
    bool heapAllocated{};
    Snapshot* snapshots = AcquireSnapshots(view.count, inlineSnapshots,
                                            kInlineSnapshots, &heapAllocated);
    if (!snapshots) {
        const auto reason = view.count < 0 ? RejectReason::NegativeCount
                          : static_cast<std::uint32_t>(view.count) > g_maxMaterials
                              ? RejectReason::TooManyMaterials
                              : RejectReason::AllocationFailed;
        PreflightResult result{reason, -1, nullptr};
        LogRejection(&view, result, atomic);
        return;
    }

    const auto preflight = BuildSnapshots(view, snapshots);
    if (preflight.reason != RejectReason::None) {
        LogRejection(&view, preflight, atomic);
        if (heapAllocated) HeapFree(GetProcessHeap(), 0, snapshots);
        return;
    }

    if (view.count > 0 && g_inGameSelfTestPending.exchange(false)) {
        auto* invalidMaterial = reinterpret_cast<std::uint8_t*>(1);
        void* invalidAlpha = OffsetPointer(invalidMaterial, kMaterialAlphaOffset);
        const bool rejected = !invalidAlpha ||
            !CheckRange(invalidMaterial, kMaterialSize, false) ||
            !CheckRange(invalidAlpha, 1, true);
        if (rejected) {
            Log("IN-GAME SELFTEST PASS: actual RenderAlphaAtomic hook rejected local "
                "material=00000001; renderer skipped once; GTA memory unchanged");
        } else {
            Log("IN-GAME SELFTEST FAIL: material=00000001 unexpectedly passed validation");
        }
        if (heapAllocated) HeapFree(GetProcessHeap(), 0, snapshots);
        return;
    }

    void* flagsField = OffsetPointer(view.geometry, kGeometryFlagsOffset);
    if (!flagsField || !CheckRange(flagsField, sizeof(std::uint32_t), true) ||
        !SafeWriteU32(flagsField, view.flags | kRenderFlag)) {
        PreflightResult result{RejectReason::WriteFailed, -1, nullptr};
        LogRejection(&view, result, atomic);
        if (heapAllocated) HeapFree(GetProcessHeap(), 0, snapshots);
        return;
    }

    std::int32_t changed = 0;
    for (; changed < view.count; ++changed) {
        void* alphaField = OffsetPointer(snapshots[changed].material, kMaterialAlphaOffset);
        if (static_cast<int>(snapshots[changed].alpha) > requestedAlpha &&
            (!alphaField || !SafeWriteU8(alphaField, static_cast<std::uint8_t>(requestedAlpha)))) {
            RestoreState(&view, snapshots, changed);
            PreflightResult result{RejectReason::WriteFailed, changed, snapshots[changed].material};
            LogRejection(&view, result, atomic);
            if (heapAllocated) HeapFree(GetProcessHeap(), 0, snapshots);
            return;
        }
    }

    ++g_rendered;
    RenderWithFinally(atomic, &view, snapshots, heapAllocated);
}

bool SyntheticTests() noexcept {
    constexpr std::int32_t count = 153;
    auto** materials = static_cast<std::uint8_t**>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        count * sizeof(std::uint8_t*)));
    auto* blocks = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        count * 28));
    if (!materials || !blocks) {
        if (materials) HeapFree(GetProcessHeap(), 0, materials);
        if (blocks) HeapFree(GetProcessHeap(), 0, blocks);
        Log("SYNTHETIC SELFTEST FAIL: allocation");
        return false;
    }

    for (std::int32_t index = 0; index < count; ++index) {
        materials[index] = blocks + index * 28;
        materials[index][kMaterialAlphaOffset] = static_cast<std::uint8_t>(100 + index % 100);
    }
    GeometryView view{blocks, materials, count, count, 0};

    Snapshot inlineSnapshots[kInlineSnapshots];
    bool heapAllocated{};
    Snapshot* snapshots = AcquireSnapshots(count, inlineSnapshots,
                                            kInlineSnapshots, &heapAllocated);
    PreflightResult valid{RejectReason::AllocationFailed, -1, nullptr};
    if (snapshots) valid = BuildSnapshots(view, snapshots);
    bool passed = snapshots && heapAllocated && valid.reason == RejectReason::None;
    for (std::int32_t index = 0; passed && index < count; ++index) {
        void* alphaField = OffsetPointer(snapshots[index].material, kMaterialAlphaOffset);
        passed = alphaField && SafeWriteU8(alphaField, 1);
    }
    if (passed) RestoreState(&view, snapshots, count);
    for (std::int32_t index = 0; passed && index < count; ++index) {
        std::uint8_t restored{};
        void* alphaField = OffsetPointer(snapshots[index].material, kMaterialAlphaOffset);
        passed = alphaField && SafeReadU8(alphaField, &restored) &&
                 restored == snapshots[index].alpha;
    }

    auto* saved = materials[12];
    materials[12] = reinterpret_cast<std::uint8_t*>(1);
    const auto invalid = BuildSnapshots(view, snapshots);
    materials[12] = saved;
    passed = passed && invalid.reason == RejectReason::InvalidMaterial &&
             invalid.badIndex == 12 && invalid.badMaterial == reinterpret_cast<std::uint8_t*>(1);

    if (heapAllocated && snapshots) HeapFree(GetProcessHeap(), 0, snapshots);
    HeapFree(GetProcessHeap(), 0, blocks);
    HeapFree(GetProcessHeap(), 0, materials);
    Log("SYNTHETIC SELFTEST %s: material=1 rejected; 153 snapshots handled dynamically",
        passed ? "PASS" : "FAIL");
    return passed;
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
    const auto directory = ModuleDirectory();
    const auto logPath = directory + L"RenderAlphaFix.log";
    g_log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void LoadConfig() {
    const auto iniPath = ModuleDirectory() + L"RenderAlphaFix.ini";
    g_maxMaterials = static_cast<std::uint32_t>(GetPrivateProfileIntW(
        L"Fix", L"MaxMaterials", kDefaultMaxMaterials, iniPath.c_str()));
    if (g_maxMaterials < 1 || g_maxMaterials > 65536) g_maxMaterials = kDefaultMaxMaterials;
    g_logRejected = GetPrivateProfileIntW(L"Fix", L"LogRejected", 1, iniPath.c_str()) != 0;
}

void Initialize() {
    LoadConfig();
    Log("RenderAlphaFix 1.0 initializing from GTA GameLoop; maxMaterials=%lu",
        static_cast<unsigned long>(g_maxMaterials));

    HMODULE pinnedModule{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(g_module), &pinnedModule)) {
        Log("FATAL: unable to pin ASI module; hook not installed");
        return;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base || std::memcmp(base + kRenderAlphaAtomicRva, kExpectedEntry,
                             sizeof(kExpectedEntry)) != 0) {
        Log("FATAL: unsupported/conflicting gta_sa.exe; signature at RVA 0x332480 does not match");
        return;
    }

    g_defaultRender = reinterpret_cast<AtomicDefaultRenderFn>(base + kAtomicDefaultRenderRva);
    g_hook = new (std::nothrow) RenderHook(
        reinterpret_cast<RenderAlphaAtomicFn>(base + kRenderAlphaAtomicRva),
        &RenderAlphaReplacement, false);
    if (!g_hook || !g_hook->install()) {
        Log("FATAL: kthook failed to install at gta_sa.exe+0x332480");
        delete g_hook;
        g_hook = nullptr;
        return;
    }

    Log("HOOK INSTALLED at %p; AtomicDefaultRenderCallBack=%p",
        base + kRenderAlphaAtomicRva, reinterpret_cast<void*>(g_defaultRender));
    SyntheticTests();
}

void MainLoopReplacement(const MainLoopHook& hook) {
    HookReturnGuard<MainLoopHook> returnGuard(hook);
    static bool init = false;
    if (!init) {
        init = true;
        Initialize();
    }
    g_timerUpdate();
}

bool InstallMainLoopHook() noexcept {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;

    auto* callSite = base + kMainLoopCallRva;
    if (callSite[0] != 0xE8) {
        Log("FATAL: GameLoop site gta_sa.exe+0x13E968 is not CALL: bytes=%02X %02X %02X %02X %02X",
            callSite[0], callSite[1], callSite[2], callSite[3], callSite[4]);
        return false;
    }

    std::int32_t relative{};
    std::memcpy(&relative, callSite + 1, sizeof(relative));
    auto* callTarget = callSite + 5 + relative;
    if (!CheckRange(callTarget, 1, false)) {
        Log("FATAL: existing GameLoop CALL target is not executable/readable: %p", callTarget);
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
        Log("RenderAlphaFix 1.0 ASI LOADED in DllMain; installing GameLoop hook");
        if (!InstallMainLoopHook()) {
            Log("FATAL: GameLoop hook installation failed; render fix is inactive");
            OutputDebugStringA("RenderAlphaFix: failed to install GameLoop hook at 0x53E968\n");
        }
    }
    return TRUE;
}
