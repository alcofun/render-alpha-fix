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
constexpr std::size_t kInlineSnapshots = 512;
constexpr std::uint32_t kDefaultMaxMaterials = 4096;
constexpr std::size_t kObjectGameEntityOffset = 0x40;
constexpr std::size_t kObjectMaterialPointerOffset = 0x217;
constexpr std::size_t kObjectMaterialTypeOffset = 0x2DB;
constexpr std::size_t kObjectTextureCreatedOffset = 0x31B;
constexpr std::size_t kObjectPoolFlagsOffset = 0x04;
constexpr std::size_t kObjectPoolPointersOffset = 0xFA4;
constexpr std::int32_t kSampMaxObjects = 1000;

using RenderAlphaAtomicFn = void(__cdecl*)(void*, int);
using AtomicDefaultRenderFn = void*(__cdecl*)(void*);
using ObjectPoolProcessFn = void(__thiscall*)(void*);
using ConstructMaterialTextFn = void(__thiscall*)(void*);
using dMainLoop = void(__stdcall*)();
using RenderHook = kthook::kthook_simple<RenderAlphaAtomicFn>;
using MainLoopHook = kthook::kthook_simple<dMainLoop>;
using ObjectPoolHook = kthook::kthook_simple<ObjectPoolProcessFn>;

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
ObjectPoolHook* g_objectPoolHook{};
dMainLoop g_timerUpdate{};
AtomicDefaultRenderFn g_defaultRender{};
std::uint32_t g_maxMaterials = kDefaultMaxMaterials;
bool g_logRejected = true;
std::uint64_t g_rejected{};
std::atomic_bool g_inGameSelfTestPending{true};
std::atomic_bool g_sampRepairRequested{false};
std::uint8_t* g_sampBase{};
ConstructMaterialTextFn g_constructMaterialText{};
void* g_objectPool{};
DWORD g_lastRepairTick{};
DWORD g_lastSampProbeTick{};
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
    if (reinterpret_cast<std::uintptr_t>(atomic) < 0x10000) return RejectReason::NullAtomic;
    void* geometryField = OffsetPointer(atomic, kAtomicGeometryOffset);
    if (!geometryField) return RejectReason::InvalidAtomic;

    void* geometry{};
    if (!SafeReadPointer(geometryField, &geometry))
        return RejectReason::InvalidAtomic;
    if (reinterpret_cast<std::uintptr_t>(geometry) < 0x10000) return RejectReason::NullGeometry;

    auto* geometryBytes = static_cast<std::uint8_t*>(geometry);
    void* geometryRange = OffsetPointer(geometry, kGeometryFlagsOffset);
    void* flagsField = geometryRange;
    void* materialsField = OffsetPointer(geometry, kGeometryMaterialsOffset);
    void* countField = OffsetPointer(geometry, kGeometryCountOffset);
    void* spaceField = OffsetPointer(geometry, kGeometrySpaceOffset);
    if (!geometryRange || !materialsField || !countField || !spaceField)
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

    if (reinterpret_cast<std::uintptr_t>(view.materials) < 0x10000)
        return {RejectReason::InvalidMaterialArray, -1, nullptr};

    for (std::int32_t index = 0; index < view.count; ++index) {
        void* rawMaterial{};
        if (!SafeReadPointer(view.materials + index, &rawMaterial))
            return {RejectReason::InvalidMaterialArray, index, nullptr};

        auto* material = static_cast<std::uint8_t*>(rawMaterial);

        void* alphaField = OffsetPointer(material, kMaterialAlphaOffset);
        if (reinterpret_cast<std::uintptr_t>(material) < 0x10000 || !alphaField) {
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
    g_sampRepairRequested.store(true, std::memory_order_relaxed);
    if (!g_logRejected || (rejected > 1 && rejected % 1000 != 0)) return;
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
            reinterpret_cast<std::uintptr_t>(invalidMaterial) < 0x10000;
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
    if (!flagsField || !SafeWriteU32(flagsField, view.flags | kRenderFlag)) {
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

    RenderWithFinally(atomic, &view, snapshots, heapAllocated);
}

bool HasMalformedTextSlot(void* object) noexcept {
    if (reinterpret_cast<std::uintptr_t>(object) < 0x10000) return false;
    auto* objectBytes = static_cast<std::uint8_t*>(object);
    for (std::int32_t slot = 0; slot < 16; ++slot) {
        std::int32_t type{};
        std::int32_t textureCreated{};
        void* materialPointer{};
        if (!SafeReadI32(objectBytes + kObjectMaterialTypeOffset + slot * 4, &type) ||
            !SafeReadI32(objectBytes + kObjectTextureCreatedOffset + slot * 4,
                         &textureCreated) ||
            !SafeReadPointer(objectBytes + kObjectMaterialPointerOffset + slot * 4,
                             &materialPointer)) return false;
        if (type == 2 && textureCreated == 0 && materialPointer != nullptr) return true;
    }
    return false;
}

bool SyntheticTests() noexcept {
    constexpr std::int32_t count = 513;
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

    alignas(4) std::uint8_t object[0x400]{};
    *reinterpret_cast<std::int32_t*>(object + kObjectMaterialTypeOffset + 7 * 4) = 2;
    *reinterpret_cast<void**>(object + kObjectMaterialPointerOffset + 7 * 4) =
        reinterpret_cast<void*>(0xC1000000);
    const bool malformedDetected = HasMalformedTextSlot(object);
    *reinterpret_cast<std::int32_t*>(object + kObjectTextureCreatedOffset + 7 * 4) = 1;
    passed = passed && malformedDetected && !HasMalformedTextSlot(object);

    if (heapAllocated && snapshots) HeapFree(GetProcessHeap(), 0, snapshots);
    HeapFree(GetProcessHeap(), 0, blocks);
    HeapFree(GetProcessHeap(), 0, materials);
    Log("SYNTHETIC SELFTEST %s: invalid materials rejected; 513 snapshots; SA-MP text-slot invariant",
        passed ? "PASS" : "FAIL");
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
            pattern->bytes[pattern->length] = static_cast<std::uint8_t>((high << 4) | low);
            pattern->exact[pattern->length++] = true;
            text += 2;
        }
        while (*text == ' ') ++text;
    }
    return pattern->length != 0;
}

std::uint8_t* FindUniqueExecutablePattern(std::uint8_t* module,
                                          const char* signature) noexcept {
    ParsedPattern pattern{};
    if (!module || !ParsePattern(signature, &pattern)) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (!CheckRange(dos, sizeof(*dos), false) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    if (!CheckRange(nt, sizeof(*nt), false) || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    std::uint8_t* match{};
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::size_t size = section->Misc.VirtualSize;
        auto* start = module + section->VirtualAddress;
        if (size < pattern.length || !CheckRange(start, size, false)) continue;
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

void ObjectPoolProcessReplacement(const ObjectPoolHook& hook, void*& pool) {
    HookReturnGuard<ObjectPoolHook> returnGuard(hook);
    g_objectPool = pool;
    hook.get_trampoline()(pool);
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
    Log("SA-MP OBJECT REPAIR enabled by signatures: process=%p construct=%p", process, construct);
}

void ProbeSampRepair() noexcept {
    if (g_objectPoolHook || g_sampRepairUnsupported) return;
    const DWORD now = GetTickCount();
    if (now - g_lastSampProbeTick < 1000) return;
    g_lastSampProbeTick = now;
    InitializeSampRepair();
}

void RepairSampObjects() noexcept {
    if (!g_sampBase || !g_constructMaterialText ||
        !g_sampRepairRequested.load(std::memory_order_relaxed)) return;

    const DWORD now = GetTickCount();
    if (now - g_lastRepairTick < 2000) return;
    g_lastRepairTick = now;
    g_sampRepairRequested.store(false, std::memory_order_relaxed);

    void* pool = g_objectPool;
    if (!pool) return;

    std::int32_t largest{};
    if (!SafeReadI32(pool, &largest) || largest < 0) return;
    if (largest >= kSampMaxObjects) largest = kSampMaxObjects - 1;

    std::uint32_t repaired{};
    for (std::int32_t id = 0; id <= largest && repaired < 5; ++id) {
        std::int32_t occupied{};
        void* object{};
        auto* bytes = static_cast<std::uint8_t*>(pool);
        if (!SafeReadI32(bytes + kObjectPoolFlagsOffset + id * sizeof(std::int32_t), &occupied) ||
            !occupied ||
            !SafeReadPointer(bytes + kObjectPoolPointersOffset + id * sizeof(void*), &object) ||
            !object) continue;

        void* gameEntity{};
        if (!SafeReadPointer(static_cast<std::uint8_t*>(object) + kObjectGameEntityOffset,
                             &gameEntity) || !gameEntity) continue;

        if (!HasMalformedTextSlot(object)) continue;

        __try {
            g_constructMaterialText(object);
            ++repaired;
        } __except (MemoryAccessFilter(GetExceptionCode())) {
            Log("SA-MP OBJECT REPAIR faulted for object id=%ld; repair disabled", id);
            g_sampBase = nullptr;
            return;
        }
    }

    if (repaired)
        Log("SA-MP OBJECT REPAIR rebuilt %lu malformed object(s)",
            static_cast<unsigned long>(repaired));
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
    Log("RenderAlphaFix 1.1 initializing from GTA GameLoop; maxMaterials=%lu",
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
    RepairSampObjects();
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
        Log("RenderAlphaFix 1.1 ASI LOADED in DllMain; installing GameLoop hook");
        if (!InstallMainLoopHook()) {
            Log("FATAL: GameLoop hook installation failed; render fix is inactive");
            OutputDebugStringA("RenderAlphaFix: failed to install GameLoop hook at 0x53E968\n");
        }
    }
    return TRUE;
}
