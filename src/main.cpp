#include <windows.h>

#include <kthook/kthook.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <new>
#include <string>

namespace {

constexpr std::uintptr_t kMainLoopCallRva = 0x13E968;
constexpr std::uintptr_t kTimerUpdateRva = 0x161B10;
constexpr std::uintptr_t kRenderAlphaAtomicAddress = 0x732480;
constexpr std::uintptr_t kAtomicDefaultRenderAddress = 0x7491C0;
constexpr std::uintptr_t kAtomicSetGeometrySphereCopyAddress = 0x749D7B;
constexpr std::uintptr_t kAtomicSetGeometryAfterSphereCopyAddress = 0x749D9A;
constexpr std::uintptr_t kAtomicCloneSphereCopyAddress = 0x749EDB;
constexpr std::uintptr_t kAtomicCloneAfterMorphSphereCopyAddress = 0x749EFA;
constexpr std::uintptr_t kResetTagAlphaAddress = 0x49CD50;
constexpr std::uintptr_t kGetTagAlphaAddress = 0x49CF90;
constexpr std::uintptr_t kSetTagAlphaAddress = 0x49CEC0;
constexpr std::uintptr_t kTagCountAddress = 0xA9AD70;
constexpr std::uintptr_t kTagDescriptorsAddress = 0xA9A8C0;
constexpr std::uintptr_t kSetAtomicUserValueAddress = 0x732380;
constexpr std::size_t kObjectGameEntityOffset = 0x40;
constexpr std::size_t kObjectCustomMaterialOffset = 0x118B;
constexpr std::size_t kObjectMaterialPointerOffset = 0x217;
constexpr std::size_t kObjectMaterialTypeOffset = 0x2DB;
constexpr std::size_t kObjectTextureCreatedOffset = 0x31B;
constexpr std::size_t kObjectTextInfoOffset = 0x3E5;
constexpr std::size_t kObjectTextInfoStride = 215;
constexpr std::size_t kObjectTextPointerOffset = 0x10CB;
constexpr std::size_t kObjectBackgroundTextureOffset = 0x110B;
constexpr std::size_t kObjectTextureOffset = 0x114B;
constexpr std::size_t kObjectPoolFlagsOffset = 0x04;
constexpr std::size_t kEntityMatrixOffset = 0x14;
constexpr std::size_t kMatrixPositionOffset = 0x30;
constexpr std::uintptr_t kPlayerPositionAddress = 0xB6F9CC;
constexpr std::size_t kConstructPoolPointerDisplacementOffset = 15;
constexpr std::size_t kConstructHookOffset = 12;
constexpr std::size_t kConstructEpilogueOffset = 57;
constexpr std::size_t kShutdownDirtyHookOffset = 3;
constexpr std::int32_t kStandardObjectCapacity = 1000;
constexpr std::int32_t kExtendedObjectCapacity = 2100;
constexpr std::int32_t kMaximumObjectCapacity = kExtendedObjectCapacity;
constexpr std::uint32_t kRepairTimeSliceMs = 5;
constexpr std::uint32_t kMinimumNearestRepairs = 5;
constexpr std::int32_t kMaterialSlotCount = 16;
constexpr std::int32_t kRenderAlphaMaterialLimit = 152;
constexpr std::int32_t kMaximumGeometryMaterialCapacity = 4096;
constexpr std::int32_t kMaximumTagCount = 150;
constexpr std::size_t kMaximumTextureNameLength = 63;
constexpr std::size_t kMaximumMaterialTextLength = 2048;
constexpr std::size_t kMaximumFontNameLength = 64;

using ConstructMaterialTextFn = void(__thiscall*)(void*);
using ObjectPoolDeleteFn = int(__thiscall*)(void*, unsigned short);
using SetMaterialFn = void(__thiscall*)(void*, int, int, const char*,
                                        const char*, unsigned int);
using SetMaterialTextFn = void(__thiscall*)(void*, int, const char*, char,
                                            const char*, char, char,
                                            unsigned int, int, char);
using RenderAlphaAtomicFn = void(__cdecl*)(void*, int);
using ResetTagAlphaFn = void(__cdecl*)(void*);
using GetTagAlphaFn = std::uint8_t(__cdecl*)(void*);
using SetTagAlphaFn = void(__cdecl*)(void*, std::uint8_t);
using SetAtomicUserValueFn = void(__cdecl*)(void*, int);
using AtomicDefaultRenderFn = void*(__cdecl*)(void*);
using dMainLoop = void(__stdcall*)();
using MainLoopHook = kthook::kthook_simple<dMainLoop>;
using SetMaterialHook = kthook::kthook_simple<SetMaterialFn>;
using SetMaterialTextHook = kthook::kthook_simple<SetMaterialTextFn>;
using RenderAlphaAtomicHook = kthook::kthook_simple<RenderAlphaAtomicFn>;
using ResetTagAlphaHook = kthook::kthook_simple<ResetTagAlphaFn>;
using GetTagAlphaHook = kthook::kthook_simple<GetTagAlphaFn>;
using SetTagAlphaHook = kthook::kthook_simple<SetTagAlphaFn>;
using NakedHook = kthook::kthook_naked;

struct PoolLayout {
    std::size_t pointersOffset{};
    std::int32_t capacity{};
};

struct RepairCandidate {
    void* object{};
    std::int32_t id{};
    float distanceSquared{FLT_MAX};
};

struct QuarantinedObject {
    void* object{};
    void* gameEntity{};
    int slot{};
    const char* operation{};
    const char* reason{};
};

struct ObjectDiagnostics {
    std::int32_t modelId{-1};
    float x{};
    float y{};
    float z{};
    bool hasPosition{};
};

struct AlphaSnapshot {
    std::uint8_t* material{};
    std::uint8_t alpha{};
};

struct AlphaReject {
    void* geometry{};
    void* material{};
    std::int32_t materialCount{-1};
    std::int32_t materialIndex{-1};
    const char* reason{};
};

enum class ObjectState {
    Healthy,
    Malformed,
    Fault,
};

HMODULE g_module{};
HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;
MainLoopHook* g_mainLoopHook{};
NakedHook* g_constructMaterialsHook{};
NakedHook* g_shutdownMaterialsHook{};
NakedHook* g_atomicSetGeometrySphereHook{};
NakedHook* g_atomicCloneSphereHook{};
SetMaterialHook* g_setMaterialHook{};
SetMaterialTextHook* g_setMaterialTextHook{};
RenderAlphaAtomicHook* g_renderAlphaAtomicHook{};
ResetTagAlphaHook* g_resetTagAlphaHook{};
GetTagAlphaHook* g_getTagAlphaHook{};
SetTagAlphaHook* g_setTagAlphaHook{};
dMainLoop g_timerUpdate{};
std::uint8_t* g_sampBase{};
ConstructMaterialTextFn g_constructMaterialText{};
ObjectPoolDeleteFn g_objectPoolDelete{};
DWORD g_lastSampProbeTick{};
PoolLayout g_poolLayout{};
RepairCandidate g_candidates[kMaximumObjectCapacity]{};
QuarantinedObject g_quarantine[kMaximumObjectCapacity]{};
QuarantinedObject g_quarantineWork[kMaximumObjectCapacity]{};
SRWLOCK g_quarantineLock = SRWLOCK_INIT;
std::int32_t g_quarantineCount{};
DWORD g_lastRepairSummaryTick{};
volatile LONG g_repairDirty{1};
bool g_logRepairs = true;
bool g_repairActive{};
bool g_sampProbeLogged{};
bool g_sampRepairUnsupported{};
std::int32_t* g_tagCount{};
std::uint8_t* g_tagDescriptors{};
SetAtomicUserValueFn g_setAtomicUserValue{};
AtomicDefaultRenderFn g_atomicDefaultRender{};
void* g_lastObjectPool{};
std::int32_t g_lastLargestObjectId{-1};
void* g_loggedInvalidAtomics[64]{};
std::int32_t g_loggedInvalidAtomicCount{};
SRWLOCK g_invalidAtomicLogLock = SRWLOCK_INIT;
void* g_loggedInvalidTags[64]{};
std::int32_t g_loggedInvalidTagCount{};
volatile LONG g_atomicCloneSphereRejects{};
volatile LONG g_atomicSetGeometrySphereRejects{};

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

bool CheckRange(const void* pointer, std::size_t size) noexcept;
bool IsValidMaterialIndex(int index) noexcept;
bool IsBoundedString(const char* value, std::size_t maximumLength) noexcept;
bool IsValidMaterialSize(std::uint8_t size) noexcept;
bool ValidateAtomic(const void* atomic, const char** reason) noexcept;
bool ValidateFirstMorphSphereForUse(const void* geometry, const char** reason,
                                    std::int32_t* count,
                                    const void** morphTarget) noexcept;
bool SafeRenderAlphaAtomic(void* atomic, int drawAlpha,
                           AlphaReject* reject) noexcept;
bool ValidateObjectMaterialState(void* object, const char** reason,
                                 int* invalidSlot) noexcept;

bool ContainsTagDescriptor(const void* entity, const std::uint8_t* descriptors,
                           std::int32_t count) noexcept {
    if (!entity || !descriptors || count < 0 || count > kMaximumTagCount)
        return false;
    for (std::int32_t index = 0; index < count; ++index) {
        const void* stored{};
        std::memcpy(&stored, descriptors + index * 8, sizeof(stored));
        if (stored == entity) return true;
    }
    return false;
}

bool DecodePoolLayout(const std::uint8_t* constructMaterials,
                      PoolLayout* layout) noexcept {
    if (!constructMaterials || !layout ||
        !CheckRange(constructMaterials + kConstructPoolPointerDisplacementOffset,
                    sizeof(std::int32_t))) return false;

    std::int32_t pointersOffset{};
    std::memcpy(&pointersOffset,
                constructMaterials + kConstructPoolPointerDisplacementOffset,
                sizeof(pointersOffset));
    if (pointersOffset < 4 || (pointersOffset - 4) % 4 != 0) return false;

    const std::int32_t capacity = (pointersOffset - 4) / 4;
    if (capacity != kStandardObjectCapacity &&
        capacity != kExtendedObjectCapacity) return false;
    layout->pointersOffset = static_cast<std::size_t>(pointersOffset);
    layout->capacity = capacity;
    return true;
}

ObjectState InspectObject(void* object, std::int32_t id,
                          RepairCandidate* candidate) noexcept {
    if (reinterpret_cast<std::uintptr_t>(object) < 0x10000)
        return ObjectState::Fault;

    __try {
        auto* bytes = static_cast<std::uint8_t*>(object);
        if (!bytes[kObjectCustomMaterialOffset])
            return ObjectState::Healthy;

        auto* gameEntity = *reinterpret_cast<std::uint8_t**>(
            bytes + kObjectGameEntityOffset);
        if (!gameEntity) return ObjectState::Healthy;

        for (std::int32_t slot = 0; slot < 16; ++slot) {
            const auto type = *reinterpret_cast<std::int32_t*>(
                bytes + kObjectMaterialTypeOffset + slot * 4);
            if (type != 2) continue;
            const auto textureCreated = *reinterpret_cast<std::int32_t*>(
                bytes + kObjectTextureCreatedOffset + slot * 4);
            if (textureCreated != 0) continue;
            const auto materialPointer = *reinterpret_cast<void**>(
                bytes + kObjectMaterialPointerOffset + slot * 4);
            if (!materialPointer) continue;

            if (candidate) {
                candidate->object = object;
                candidate->id = id;
                candidate->distanceSquared = FLT_MAX;

                auto* matrix = *reinterpret_cast<std::uint8_t**>(
                    gameEntity + kEntityMatrixOffset);
                if (!matrix) return ObjectState::Malformed;
                const auto* player = reinterpret_cast<const float*>(
                    kPlayerPositionAddress);
                const auto* position = reinterpret_cast<const float*>(
                    matrix + kMatrixPositionOffset);
                const float dx = position[0] - player[0];
                const float dy = position[1] - player[1];
                const float dz = position[2] - player[2];
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (std::isfinite(distanceSquared))
                    candidate->distanceSquared = distanceSquared;
            }
            return ObjectState::Malformed;
        }
        return ObjectState::Healthy;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return ObjectState::Fault;
    }
}

bool HasMalformedTextSlot(void* object) noexcept {
    return InspectObject(object, 0, nullptr) == ObjectState::Malformed;
}

std::uint32_t g_syntheticAtomicRenders{};
void* __cdecl SyntheticAtomicDefaultRender(void* atomic) noexcept {
    ++g_syntheticAtomicRenders;
    return atomic;
}

bool SyntheticTests() noexcept {
    alignas(4) std::uint8_t object[0x1200]{};
    alignas(4) std::uint8_t gameEntity[0x40]{};
    constexpr std::int32_t slot = 7;
    *reinterpret_cast<void**>(object + kObjectGameEntityOffset) = gameEntity;
    object[kObjectCustomMaterialOffset] = 1;
    *reinterpret_cast<std::int32_t*>(object + kObjectMaterialTypeOffset + slot * 4) = 2;
    *reinterpret_cast<void**>(object + kObjectMaterialPointerOffset + slot * 4) =
        reinterpret_cast<void*>(1);
    const bool malformedDetected = HasMalformedTextSlot(object);
    *reinterpret_cast<std::int32_t*>(object + kObjectTextureCreatedOffset + slot * 4) = 1;
    const bool healthyIgnored = !HasMalformedTextSlot(object);
    *reinterpret_cast<std::int32_t*>(object + kObjectTextureCreatedOffset + slot * 4) = 0;
    *reinterpret_cast<void**>(object + kObjectMaterialPointerOffset + slot * 4) = nullptr;
    const bool emptyIgnored = !HasMalformedTextSlot(object);
    alignas(4) std::uint8_t standardConstruct[80]{};
    alignas(4) std::uint8_t extendedConstruct[80]{};
    *reinterpret_cast<std::int32_t*>(standardConstruct +
        kConstructPoolPointerDisplacementOffset) = 0xFA4;
    *reinterpret_cast<std::int32_t*>(extendedConstruct +
        kConstructPoolPointerDisplacementOffset) = 0x20D4;
    PoolLayout standard{};
    PoolLayout extended{};
    const bool layouts = DecodePoolLayout(standardConstruct, &standard) &&
        standard.capacity == kStandardObjectCapacity &&
        DecodePoolLayout(extendedConstruct, &extended) &&
        extended.capacity == kExtendedObjectCapacity;

    const bool indexBounds = !IsValidMaterialIndex(-1) &&
        IsValidMaterialIndex(0) && IsValidMaterialIndex(15) &&
        !IsValidMaterialIndex(16);
    const char* validationReason{};
    int validationSlot{-1};
    const bool invalidGraphRejected = !ValidateObjectMaterialState(
        object, &validationReason, &validationSlot);
    const bool payloadBounds = IsBoundedString("vehiclegrunge256", 63) &&
        !IsBoundedString(nullptr, 63) && IsValidMaterialSize(10) &&
        IsValidMaterialSize(140) && !IsValidMaterialSize(0) &&
        !IsValidMaterialSize(141);
    alignas(4) std::uint8_t atomic[0x4C]{};
    alignas(4) std::uint8_t geometry[0x60]{};
    alignas(4) std::uint8_t morphTarget[0x18]{};
    alignas(4) std::uint8_t material[0x1C]{};
    void* materials[153]{};
    std::fill(std::begin(materials), std::end(materials),
              static_cast<void*>(material));
    atomic[0] = 1;
    geometry[0] = 8;
    *reinterpret_cast<void**>(atomic + 0x18) = geometry;
    *reinterpret_cast<std::int32_t*>(geometry + 0x18) = 1;
    *reinterpret_cast<void**>(geometry + 0x5C) = morphTarget;
    *reinterpret_cast<void**>(morphTarget) = geometry;
    *reinterpret_cast<void***>(geometry + 0x20) = materials;
    *reinterpret_cast<std::int32_t*>(geometry + 0x24) = 1;
    *reinterpret_cast<std::int32_t*>(geometry + 0x28) = 153;
    const char* rwReason{};
    const bool validAtomicAccepted = ValidateAtomic(atomic, &rwReason);
    *reinterpret_cast<void**>(geometry + 0x5C) = nullptr;
    const bool missingMorphRejected = !ValidateAtomic(atomic, &rwReason);
    *reinterpret_cast<void**>(geometry + 0x5C) = morphTarget;
    *reinterpret_cast<std::int32_t*>(geometry + 0x18) = 0;
    const bool zeroMorphCountRejected = !ValidateAtomic(atomic, &rwReason);
    *reinterpret_cast<std::int32_t*>(geometry + 0x18) = 1;
    geometry[0] = 0;
    const bool invalidGeometryTypeRejected = !ValidateAtomic(atomic, &rwReason);
    geometry[0] = 8;
    *reinterpret_cast<std::int32_t*>(geometry + 0x24) = 153;
    const bool materialOverflowRejected = !ValidateAtomic(atomic, &rwReason);
    *reinterpret_cast<std::int32_t*>(geometry + 0x24) = 1;
    *reinterpret_cast<std::int32_t*>(geometry + 8) = 0x2E;
    material[7] = 200;
    const auto savedDefaultRender = g_atomicDefaultRender;
    g_atomicDefaultRender = &SyntheticAtomicDefaultRender;
    g_syntheticAtomicRenders = 0;
    AlphaReject alphaReject{};
    const bool guardedRenderAccepted = SafeRenderAlphaAtomic(
        atomic, 64, &alphaReject) && g_syntheticAtomicRenders == 1 &&
        material[7] == 200 &&
        *reinterpret_cast<std::int32_t*>(geometry + 8) == 0x2E;
    materials[0] = reinterpret_cast<void*>(1);
    alphaReject = {};
    const bool guardedInvalidRejected = !SafeRenderAlphaAtomic(
        atomic, 64, &alphaReject) && g_syntheticAtomicRenders == 1 &&
        alphaReject.materialIndex == 0;
    materials[0] = material;
    g_atomicDefaultRender = savedDefaultRender;
    alignas(4) std::uint8_t tagDescriptors[16]{};
    *reinterpret_cast<void**>(tagDescriptors + 8) = gameEntity;
    const bool tagInvariant =
        ContainsTagDescriptor(gameEntity, tagDescriptors, 2) &&
        !ContainsTagDescriptor(object, tagDescriptors, 2) &&
        !ContainsTagDescriptor(gameEntity, tagDescriptors,
                               kMaximumTagCount + 1);
    const bool passed = malformedDetected && healthyIgnored && emptyIgnored &&
                        layouts && indexBounds && invalidGraphRejected &&
                        payloadBounds && validAtomicAccepted &&
                        missingMorphRejected && zeroMorphCountRejected &&
                        invalidGeometryTypeRejected &&
                        materialOverflowRejected && guardedRenderAccepted &&
                        guardedInvalidRejected && tagInvariant;
    Log("SYNTHETIC SELFTEST %s: SA-MP material-text invariant; "
        "full payload/slot graph; material indices 0..15; "
        "RenderWare material limit 152; first morph-target sphere invariant; "
        "zero-allocation alpha guard; "
        "tag descriptor invariant; pool layouts 1000/2100",
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
            pattern->bytes[pattern->length] =
                static_cast<std::uint8_t>((high << 4) | low);
            pattern->exact[pattern->length++] = true;
            text += 2;
        }
        while (*text == ' ') ++text;
    }
    return pattern->length != 0;
}

bool MatchesExecutablePatternAt(const void* address,
                                const char* signature) noexcept {
    ParsedPattern pattern{};
    if (!address || !ParsePattern(signature, &pattern) ||
        !CheckRange(address, pattern.length)) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    for (std::size_t index = 0; index < pattern.length; ++index) {
        if (pattern.exact[index] && bytes[index] != pattern.bytes[index])
            return false;
    }
    return true;
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

std::uint8_t* FindUniquePatternByCallTarget(std::uint8_t* module,
                                            const char* signature,
                                            std::size_t callOffset,
                                            const void* expectedTarget,
                                            bool requireEqual) noexcept {
    ParsedPattern pattern{};
    if (!module || !ParsePattern(signature, &pattern) ||
        callOffset + 5 > pattern.length) return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (!CheckRange(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    if (!CheckRange(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    std::uint8_t* selected{};
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
            auto* candidate = start + offset;
            if (candidate[callOffset] != 0xE8) continue;
            std::int32_t relative{};
            std::memcpy(&relative, candidate + callOffset + 1, sizeof(relative));
            const auto* target = candidate + callOffset + 5 + relative;
            if ((target == expectedTarget) != requireEqual) continue;
            if (selected) return nullptr;
            selected = candidate;
        }
    }
    return selected;
}

void DisableRepair(const char* reason) noexcept {
    if (g_repairActive) Log("SA-MP OBJECT REPAIR disabled: %s", reason);
    g_repairActive = false;
    g_constructMaterialText = nullptr;
}

bool ReadPoolObject(void* pool, std::int32_t id, void** object) noexcept {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(pool);
        const auto occupied = *reinterpret_cast<std::int32_t*>(
            bytes + kObjectPoolFlagsOffset + id * sizeof(std::int32_t));
        if (!occupied) {
            *object = nullptr;
            return true;
        }
        *object = *reinterpret_cast<void**>(
            bytes + g_poolLayout.pointersOffset + id * sizeof(void*));
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        *object = nullptr;
        return false;
    }
}

bool IsValidMaterialIndex(int index) noexcept {
    return index >= 0 && index < 16;
}

ObjectDiagnostics ReadObjectDiagnostics(void* gameEntity) noexcept {
    ObjectDiagnostics result{};
    if (!gameEntity) return result;
    __try {
        auto* entity = static_cast<std::uint8_t*>(gameEntity);
        result.modelId = *reinterpret_cast<const std::uint16_t*>(entity + 0x22);
        auto* matrix = *reinterpret_cast<std::uint8_t**>(entity + kEntityMatrixOffset);
        const auto* position = matrix
            ? reinterpret_cast<const float*>(matrix + kMatrixPositionOffset)
            : reinterpret_cast<const float*>(entity + 4);
        result.x = position[0];
        result.y = position[1];
        result.z = position[2];
        result.hasPosition = std::isfinite(result.x) &&
            std::isfinite(result.y) && std::isfinite(result.z);
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        result = {};
        result.modelId = -1;
    }
    return result;
}

bool IsBoundedString(const char* value, std::size_t maximumLength) noexcept {
    if (!value) return false;
    __try {
        for (std::size_t index = 0; index <= maximumLength; ++index) {
            if (value[index] == '\0') return true;
        }
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
    return false;
}

bool IsValidMaterialSize(std::uint8_t size) noexcept {
    return size >= 10 && size <= 140 && size % 10 == 0;
}

bool IsValidComObject(const void* object) noexcept {
    if (!CheckRange(object, sizeof(void*))) return false;
    void* vtable{};
    __try {
        vtable = *reinterpret_cast<void* const*>(object);
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
    return CheckRange(vtable, 3 * sizeof(void*));
}

bool ValidateFirstMorphSphereForUse(const void* geometry, const char** reason,
                                    std::int32_t* count,
                                    const void** morphTarget) noexcept {
    if (count) *count = -1;
    if (morphTarget) *morphTarget = nullptr;
    if (!CheckRange(geometry, 0x60)) {
        *reason = "missing or unreadable RpGeometry";
        return false;
    }

    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(geometry);
        if (bytes[0] != 8) {
            *reason = "invalid RpGeometry type";
            return false;
        }
        const auto morphCount = *reinterpret_cast<const std::int32_t*>(
            bytes + 0x18);
        const auto* firstMorph = *reinterpret_cast<std::uint8_t* const*>(
            bytes + 0x5C);
        if (count) *count = morphCount;
        if (morphTarget) *morphTarget = firstMorph;
        if (morphCount <= 0) {
            *reason = "invalid RpMorphTarget count";
            return false;
        }
        const auto morphAddress = reinterpret_cast<std::uintptr_t>(firstMorph);
        if (!firstMorph || morphAddress > UINTPTR_MAX - 4 ||
            !CheckRange(reinterpret_cast<const void*>(morphAddress + 4), 16)) {
            *reason = "missing or unreadable first RpMorphTarget sphere";
            return false;
        }
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        *reason = "RpMorphTarget access fault";
        return false;
    }
}

bool ValidateAtomic(const void* atomic, const char** reason) noexcept {
    if (!CheckRange(atomic, 0x1C)) {
        *reason = "unreadable RpAtomic";
        return false;
    }

    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(atomic);
        if (bytes[0] != 1) {
            *reason = "invalid RpAtomic type";
            return false;
        }
        const auto* geometry = *reinterpret_cast<std::uint8_t* const*>(bytes + 0x18);
        if (!ValidateFirstMorphSphereForUse(geometry, reason, nullptr, nullptr))
            return false;

        auto* materials = *reinterpret_cast<void* const* const*>(geometry + 0x20);
        const auto count = *reinterpret_cast<const std::int32_t*>(geometry + 0x24);
        const auto capacity = *reinterpret_cast<const std::int32_t*>(geometry + 0x28);
        if (count < 0 || capacity < 0 || count > capacity ||
            capacity > kMaximumGeometryMaterialCapacity) {
            *reason = "invalid RpMaterialList bounds";
            return false;
        }
        if (count > kRenderAlphaMaterialLimit) {
            *reason = "RpGeometry exceeds RenderAlphaAtomic material limit";
            return false;
        }
        if (count && !CheckRange(materials,
                                 static_cast<std::size_t>(count) * sizeof(void*))) {
            *reason = "unreadable RpMaterialList";
            return false;
        }
        for (std::int32_t index = 0; index < count; ++index) {
            if (!CheckRange(materials[index], 0x1C)) {
                *reason = "invalid RpMaterial pointer";
                return false;
            }
        }
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        *reason = "RenderWare graph access fault";
        return false;
    }
}

bool ValidateRenderWareObject(const void* rwObject, const char** reason) noexcept {
    if (!CheckRange(rwObject, 8)) {
        *reason = "unreadable RwObject";
        return false;
    }
    __try {
        const auto type = *static_cast<const std::uint8_t*>(rwObject);
        if (type == 1) return ValidateAtomic(rwObject, reason);
        if (type != 2) {
            *reason = "invalid entity RwObject type";
            return false;
        }

        const auto* clump = static_cast<const std::uint8_t*>(rwObject);
        if (!CheckRange(clump, 0x10)) {
            *reason = "unreadable RpClump";
            return false;
        }
        const auto* head = reinterpret_cast<const void* const*>(clump + 8);
        auto* link = *head;
        std::int32_t visited{};
        while (link != head) {
            if (!CheckRange(link, 2 * sizeof(void*)) || ++visited > 4096) {
                *reason = "corrupt RpClump atomic list";
                return false;
            }
            const auto* atomic = static_cast<const std::uint8_t*>(link) - 0x40;
            if (!ValidateAtomic(atomic, reason)) return false;
            link = *static_cast<void* const*>(link);
        }
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        *reason = "RpClump traversal access fault";
        return false;
    }
}

bool ValidateObjectMaterialState(void* object, const char** reason,
                                 int* invalidSlot) noexcept {
    if (!CheckRange(object, kObjectCustomMaterialOffset + 1)) {
        *reason = "unreadable CObject";
        return false;
    }

    __try {
        auto* bytes = static_cast<std::uint8_t*>(object);
        if (!bytes[kObjectCustomMaterialOffset])
            return true;

        for (std::int32_t slot = 0; slot < kMaterialSlotCount; ++slot) {
            const auto type = *reinterpret_cast<std::int32_t*>(
                bytes + kObjectMaterialTypeOffset + slot * 4);
            const auto created = *reinterpret_cast<std::int32_t*>(
                bytes + kObjectTextureCreatedOffset + slot * 4);
            auto* resource = *reinterpret_cast<void**>(
                bytes + kObjectMaterialPointerOffset + slot * 4);
            auto* background = *reinterpret_cast<void**>(
                bytes + kObjectBackgroundTextureOffset + slot * 4);
            auto* texture = *reinterpret_cast<void**>(
                bytes + kObjectTextureOffset + slot * 4);
            auto* text = *reinterpret_cast<const char**>(
                bytes + kObjectTextPointerOffset + slot * 4);
            *invalidSlot = slot;

            if (type < 0 || type > 2) {
                *reason = "invalid SA-MP material type";
                return false;
            }
            if (created != 0 && created != 1) {
                *reason = "invalid texture-created state";
                return false;
            }
            if (resource && !CheckRange(resource, sizeof(void*))) {
                *reason = "invalid material resource pointer";
                return false;
            }
            if (background && !IsValidComObject(background)) {
                *reason = "invalid background texture pointer";
                return false;
            }
            if (texture && !IsValidComObject(texture)) {
                *reason = "invalid generated texture pointer";
                return false;
            }
            if (created && !texture) {
                *reason = "created material has no texture";
                return false;
            }
            if (!created && texture) {
                *reason = "unowned generated texture";
                return false;
            }
            if (type != 2) {
                if (created || texture || text || background ||
                    (type == 0 && resource)) {
                    *reason = "text resources attached to non-text material";
                    return false;
                }
                continue;
            }

            if (!resource) {
                *reason = "text material has no sprite resource";
                return false;
            }

            const auto info = kObjectTextInfoOffset +
                static_cast<std::size_t>(slot) * kObjectTextInfoStride;
            const auto size = bytes[info];
            const auto* font = reinterpret_cast<const char*>(bytes + info + 1);
            const auto align = bytes[info + 76];
            if (!text || !IsBoundedString(text, kMaximumMaterialTextLength)) {
                *reason = "invalid material text storage";
                return false;
            }
            if (!IsValidMaterialSize(size)) {
                *reason = "invalid material texture size";
                return false;
            }
            if (!IsBoundedString(font, kMaximumFontNameLength)) {
                *reason = "unterminated material font";
                return false;
            }
            if (align > 2) {
                *reason = "invalid material text alignment";
                return false;
            }
        }

        auto* entity = *reinterpret_cast<std::uint8_t**>(
            bytes + kObjectGameEntityOffset);
        if (!entity) return true;
        if (!CheckRange(entity, 0x1C)) {
            *reason = "unreadable GTA entity";
            *invalidSlot = -1;
            return false;
        }
        auto* rwObject = *reinterpret_cast<void**>(entity + 0x18);
        if (!rwObject) return true;
        *invalidSlot = -1;
        return ValidateRenderWareObject(rwObject, reason);
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        *reason = "CObject material graph access fault";
        *invalidSlot = -1;
        return false;
    }
}

bool NeutralizeInvalidObjectState(void* object, int slot) noexcept {
    if (!CheckRange(object, kObjectCustomMaterialOffset + 1))
        return false;
    __try {
        auto* bytes = static_cast<std::uint8_t*>(object);
        if (slot >= 0 && slot < kMaterialSlotCount) {
            *reinterpret_cast<void**>(bytes + kObjectMaterialPointerOffset + slot * 4) = nullptr;
            *reinterpret_cast<void**>(bytes + kObjectBackgroundTextureOffset + slot * 4) = nullptr;
            *reinterpret_cast<void**>(bytes + kObjectTextureOffset + slot * 4) = nullptr;
            *reinterpret_cast<void**>(bytes + kObjectTextPointerOffset + slot * 4) = nullptr;
            *reinterpret_cast<std::int32_t*>(bytes + kObjectMaterialTypeOffset + slot * 4) = 0;
            *reinterpret_cast<std::int32_t*>(bytes + kObjectTextureCreatedOffset + slot * 4) = 0;
        } else {
            // Do not let CObject destruction walk a RenderWare graph which the
            // object validator has already proved unsafe. The corrupt GTA entity is
            // intentionally leaked instead of being dereferenced.
            *reinterpret_cast<void**>(bytes + kObjectGameEntityOffset) = nullptr;
        }
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
}

void QueueQuarantine(void* object, int slot, const char* operation,
                     const char* reason) noexcept {
    if (!object) return;

    void* gameEntity{};
    __try {
        gameEntity = *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(object) + kObjectGameEntityOffset);
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        Log("SA-MP OBJECT QUARANTINE rejected unreadable object=%p "
            "operation=%s slot=%d reason=%s", object, operation, slot, reason);
        return;
    }

    bool queued{};
    AcquireSRWLockExclusive(&g_quarantineLock);
    for (std::int32_t index = 0; index < g_quarantineCount; ++index) {
        if (g_quarantine[index].object == object) {
            ReleaseSRWLockExclusive(&g_quarantineLock);
            return;
        }
    }
    if (g_quarantineCount < kMaximumObjectCapacity) {
        g_quarantine[g_quarantineCount++] = {
            object, gameEntity, slot, operation, reason};
        queued = true;
    }
    ReleaseSRWLockExclusive(&g_quarantineLock);

    if (queued) {
        InterlockedExchange(&g_repairDirty, 1);
        Log("SA-MP OBJECT QUARANTINE queued object=%p operation=%s "
            "slot=%d reason=%s", object, operation, slot, reason);
    } else {
        Log("SA-MP OBJECT QUARANTINE queue full; rejected operation=%s "
            "object=%p slot=%d reason=%s", operation, object, slot, reason);
    }
}

void DeleteQuarantinedObjects(void* pool) noexcept {
    if (!pool || !g_objectPoolDelete) return;

    std::int32_t count{};
    AcquireSRWLockExclusive(&g_quarantineLock);
    count = g_quarantineCount;
    if (count > 0) {
        std::memcpy(g_quarantineWork, g_quarantine,
                    static_cast<std::size_t>(count) * sizeof(QuarantinedObject));
        g_quarantineCount = 0;
    }
    ReleaseSRWLockExclusive(&g_quarantineLock);

    for (std::int32_t entry = 0; entry < count; ++entry) {
        bool found{};
        for (std::int32_t id = 0; id < g_poolLayout.capacity; ++id) {
            void* object{};
            if (!ReadPoolObject(pool, id, &object)) {
                Log("SA-MP OBJECT QUARANTINE pool access fault; object=%p",
                    g_quarantineWork[entry].object);
                break;
            }
            if (!object) continue;
            if (g_quarantineWork[entry].object &&
                object != g_quarantineWork[entry].object) continue;
            void* currentEntity{};
            __try {
                currentEntity = *reinterpret_cast<void**>(
                    static_cast<std::uint8_t*>(object) +
                    kObjectGameEntityOffset);
            } __except (MemoryAccessFilter(GetExceptionCode())) {
                currentEntity = nullptr;
            }
            if (!g_quarantineWork[entry].object &&
                currentEntity != g_quarantineWork[entry].gameEntity) {
                continue;
            }
            if (!g_quarantineWork[entry].gameEntity ||
                currentEntity != g_quarantineWork[entry].gameEntity) {
                Log("SA-MP OBJECT QUARANTINE stale object skipped objectId=%ld "
                    "object=%p oldEntity=%p currentEntity=%p", id, object,
                    g_quarantineWork[entry].gameEntity, currentEntity);
                found = true;
                break;
            }

            const auto diagnostics = ReadObjectDiagnostics(currentEntity);
            const int deleted = g_objectPoolDelete(
                pool, static_cast<unsigned short>(id));
            Log("SA-MP OBJECT QUARANTINE %s objectId=%ld object=%p "
                "entity=%p modelId=%ld position=(%.3f,%.3f,%.3f) "
                "positionValid=%d operation=%s slot=%d reason=%s "
                "pawn=CreateDynamicObject(%ld, %.3f, %.3f, %.3f, ...);",
                deleted ? "deleted" : "delete-failed", id, object,
                currentEntity, static_cast<long>(diagnostics.modelId),
                diagnostics.x, diagnostics.y, diagnostics.z,
                diagnostics.hasPosition ? 1 : 0,
                g_quarantineWork[entry].operation,
                g_quarantineWork[entry].slot,
                g_quarantineWork[entry].reason,
                static_cast<long>(diagnostics.modelId), diagnostics.x,
                diagnostics.y, diagnostics.z);
            found = true;
            break;
        }
        if (!found) {
            Log("SA-MP OBJECT QUARANTINE object already absent object=%p "
                "operation=%s slot=%d reason=%s",
                g_quarantineWork[entry].object,
                g_quarantineWork[entry].operation,
                g_quarantineWork[entry].slot,
                g_quarantineWork[entry].reason);
        }
    }
}

void SanitizeSampObjects(void* pool, std::int32_t largest) noexcept {
    if (!pool || !g_objectPoolDelete || largest < 0) return;
    if (largest >= g_poolLayout.capacity) largest = g_poolLayout.capacity - 1;

    std::int32_t rejected{};
    for (std::int32_t id = 0; id <= largest; ++id) {
        void* object{};
        if (!ReadPoolObject(pool, id, &object)) {
            DisableRepair("object-pool access fault during validation");
            return;
        }
        if (!object) continue;

        const char* reason = nullptr;
        int slot = -1;
        if (ValidateObjectMaterialState(object, &reason, &slot)) continue;

        void* gameEntity{};
        __try {
            gameEntity = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(object) + kObjectGameEntityOffset);
        } __except (MemoryAccessFilter(GetExceptionCode())) {
            gameEntity = nullptr;
        }
        const auto diagnostics = ReadObjectDiagnostics(gameEntity);

        if (!NeutralizeInvalidObjectState(object, slot)) {
            Log("SA-MP OBJECT QUARANTINE retained unreadable objectId=%ld "
                "object=%p entity=%p modelId=%ld position=(%.3f,%.3f,%.3f) "
                "positionValid=%d slot=%d reason=%s "
                "pawn=CreateDynamicObject(%ld, %.3f, %.3f, %.3f, ...);",
                id, object, gameEntity,
                static_cast<long>(diagnostics.modelId), diagnostics.x,
                diagnostics.y, diagnostics.z,
                diagnostics.hasPosition ? 1 : 0, slot,
                reason ? reason : "unknown validation failure",
                static_cast<long>(diagnostics.modelId), diagnostics.x,
                diagnostics.y, diagnostics.z);
            continue;
        }

        const int deleted = g_objectPoolDelete(
            pool, static_cast<unsigned short>(id));
        Log("SA-MP OBJECT QUARANTINE %s objectId=%ld object=%p entity=%p "
            "modelId=%ld position=(%.3f,%.3f,%.3f) positionValid=%d "
            "slot=%d reason=%s phase=full-preflight "
            "pawn=CreateDynamicObject(%ld, %.3f, %.3f, %.3f, ...);",
            deleted ? "deleted" : "delete-failed", id, object, gameEntity,
            static_cast<long>(diagnostics.modelId), diagnostics.x,
            diagnostics.y, diagnostics.z,
            diagnostics.hasPosition ? 1 : 0, slot,
            reason ? reason : "unknown validation failure",
            static_cast<long>(diagnostics.modelId), diagnostics.x,
            diagnostics.y, diagnostics.z);
        ++rejected;
    }
    if (rejected)
        Log("SA-MP OBJECT QUARANTINE preflight complete: rejected=%ld",
            static_cast<long>(rejected));
}

bool BudgetExpired(DWORD started) noexcept {
    return static_cast<DWORD>(GetTickCount() - started) >= kRepairTimeSliceMs;
}

bool CandidateNearer(const RepairCandidate& left,
                     const RepairCandidate& right) noexcept {
    if (left.distanceSquared != right.distanceSquared)
        return left.distanceSquared < right.distanceSquared;
    return left.id < right.id;
}

void RepairSampObjects(void* pool, std::int32_t largest) noexcept {
    if (!g_repairActive || !g_constructMaterialText || !pool ||
        g_poolLayout.capacity <= 0) return;

    if (largest < 0) return;
    if (largest >= g_poolLayout.capacity) largest = g_poolLayout.capacity - 1;

    DWORD sequentialStarted{};
    std::uint32_t sequentialRepairs{};
    bool useNearestPass{};
    for (std::int32_t id = 0; id <= largest; ++id) {
        void* object{};
        if (!ReadPoolObject(pool, id, &object)) {
            DisableRepair("object-pool slot access fault");
            return;
        }
        if (!object) continue;

        const auto state = InspectObject(object, id, nullptr);
        if (state == ObjectState::Fault) {
            DisableRepair("object inspection access fault");
            return;
        }
        if (state != ObjectState::Malformed) continue;

        if (!sequentialStarted) sequentialStarted = GetTickCount();
        g_constructMaterialText(object);
        ++sequentialRepairs;
        if (BudgetExpired(sequentialStarted)) {
            InterlockedExchange(&g_repairDirty, 1);
            useNearestPass = true;
            break;
        }
    }
    if (!useNearestPass) {
        if (g_logRepairs && sequentialRepairs) {
            Log("SA-MP OBJECT REPAIR pass: rebuilt=%lu deferred=0 phase=ConstructMaterials",
                static_cast<unsigned long>(sequentialRepairs));
        }
        return;
    }

    std::int32_t candidateCount{};
    for (std::int32_t id = 0; id <= largest; ++id) {
        void* object{};
        if (!ReadPoolObject(pool, id, &object)) {
            DisableRepair("object-pool slot access fault");
            return;
        }
        if (!object) continue;
        RepairCandidate candidate{};
        const auto state = InspectObject(object, id, &candidate);
        if (state == ObjectState::Fault) {
            DisableRepair("object inspection access fault");
            return;
        }
        if (state == ObjectState::Malformed &&
            candidate.distanceSquared != FLT_MAX &&
            candidateCount < g_poolLayout.capacity)
            g_candidates[candidateCount++] = candidate;
    }
    if (!candidateCount) return;

    std::sort(g_candidates, g_candidates + candidateCount, CandidateNearer);

    DWORD nearestStarted{};
    std::uint32_t rebuilt{};
    std::int32_t considered{};
    for (; considered < candidateCount; ++considered) {
        const auto& candidate = g_candidates[considered];
        const auto state = InspectObject(candidate.object, candidate.id, nullptr);
        if (state == ObjectState::Fault) {
            DisableRepair("candidate revalidation access fault");
            return;
        }
        if (state != ObjectState::Malformed) continue;

        if (!nearestStarted) nearestStarted = GetTickCount();
        g_constructMaterialText(candidate.object);
        ++rebuilt;
        if (rebuilt >= kMinimumNearestRepairs && BudgetExpired(nearestStarted)) {
            ++considered;
            break;
        }
    }

    if (considered < candidateCount) InterlockedExchange(&g_repairDirty, 1);
    const DWORD summaryNow = GetTickCount();
    if (g_logRepairs &&
        (!g_lastRepairSummaryTick ||
         static_cast<DWORD>(summaryNow - g_lastRepairSummaryTick) >= 1000)) {
        Log("SA-MP OBJECT REPAIR pass: sequential=%lu nearest=%lu deferred=%ld "
            "phase=ConstructMaterials",
            static_cast<unsigned long>(sequentialRepairs),
            static_cast<unsigned long>(rebuilt),
            candidateCount - considered);
        g_lastRepairSummaryTick = summaryNow ? summaryNow : 1;
    }
}

std::uintptr_t g_constructMaterialsEpilogue{};

void ConstructMaterialsReplacement(const NakedHook& hook) {
    const auto& context = hook.get_context();
    auto* pool = reinterpret_cast<void*>(context.ebx);
    g_lastObjectPool = pool;
    g_lastLargestObjectId = static_cast<std::int32_t>(context.eax);
    DeleteQuarantinedObjects(pool);
    if (InterlockedExchange(&g_repairDirty, 0) != 0) {
        const auto largest = static_cast<std::int32_t>(context.eax);
        SanitizeSampObjects(pool, largest);
        RepairSampObjects(pool, largest);
    }
    hook.get_return_address() = g_constructMaterialsEpilogue;
}

void MarkMaterialsDirty(const NakedHook&) noexcept {
    InterlockedExchange(&g_repairDirty, 1);
}

void SetMaterialReplacement(const SetMaterialHook& hook, void*& object,
                            int& model, int& index, const char*& txd,
                            const char*& texture, unsigned int& color) {
    HookReturnGuard<SetMaterialHook> returnGuard(hook);
    if (!IsValidMaterialIndex(index)) {
        QueueQuarantine(object, index, "SetMaterial", "material index outside 0..15");
        return;
    }
    if (!IsBoundedString(txd, kMaximumTextureNameLength) ||
        !IsBoundedString(texture, kMaximumTextureNameLength)) {
        QueueQuarantine(object, index, "SetMaterial",
                        "invalid or unterminated texture name");
        return;
    }
    hook.get_trampoline()(object, model, index, txd, texture, color);
    InterlockedExchange(&g_repairDirty, 1);
}

void SetMaterialTextReplacement(
    const SetMaterialTextHook& hook, void*& object, int& index,
    const char*& text, char& materialSize, const char*& font, char& fontSize,
    char& bold, unsigned int& fontColor, int& backgroundColor, char& align) {
    HookReturnGuard<SetMaterialTextHook> returnGuard(hook);
    if (!IsValidMaterialIndex(index)) {
        QueueQuarantine(object, index, "SetMaterialText",
                        "material index outside 0..15");
        return;
    }
    if (!IsBoundedString(text, kMaximumMaterialTextLength)) {
        QueueQuarantine(object, index, "SetMaterialText",
                        "invalid or unterminated material text");
        return;
    }
    if (!IsBoundedString(font, kMaximumFontNameLength)) {
        QueueQuarantine(object, index, "SetMaterialText",
                        "invalid or unterminated font name");
        return;
    }
    if (!IsValidMaterialSize(static_cast<std::uint8_t>(materialSize))) {
        QueueQuarantine(object, index, "SetMaterialText",
                        "invalid material texture size");
        return;
    }
    if (static_cast<std::uint8_t>(fontSize) == 0 ||
        static_cast<std::uint8_t>(align) > 2) {
        QueueQuarantine(object, index, "SetMaterialText",
                        "invalid font size or text alignment");
        return;
    }
    hook.get_trampoline()(object, index, text, materialSize, font, fontSize,
                          bold, fontColor, backgroundColor, align);
    InterlockedExchange(&g_repairDirty, 1);
}

std::uint32_t g_syntheticRepairCalls{};
void* g_syntheticFirstObject{};
void* g_syntheticSecondObject{};
bool g_syntheticOrderValid{};

void __fastcall SyntheticConstructMaterialText(void* object, void*) noexcept {
    if (g_syntheticRepairCalls == 0)
        g_syntheticOrderValid = object == g_syntheticFirstObject;
    else if (g_syntheticRepairCalls == 1)
        g_syntheticOrderValid = g_syntheticOrderValid &&
                                object == g_syntheticSecondObject;
    ++g_syntheticRepairCalls;
    auto* bytes = static_cast<std::uint8_t*>(object);
    *reinterpret_cast<std::int32_t*>(bytes + kObjectTextureCreatedOffset) = 1;
}

bool SchedulingSelfTest() noexcept {
    constexpr std::size_t extendedPointersOffset = 0x20D4;
    const std::size_t poolSize = extendedPointersOffset +
        kExtendedObjectCapacity * sizeof(void*);
    auto* pool = static_cast<std::uint8_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, poolSize));
    auto* first = static_cast<std::uint8_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x1200));
    auto* second = static_cast<std::uint8_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x1200));
    auto* firstEntity = static_cast<std::uint8_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x40));
    auto* secondEntity = static_cast<std::uint8_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x40));
    if (!pool || !first || !second || !firstEntity || !secondEntity) {
        if (pool) HeapFree(GetProcessHeap(), 0, pool);
        if (first) HeapFree(GetProcessHeap(), 0, first);
        if (second) HeapFree(GetProcessHeap(), 0, second);
        if (firstEntity) HeapFree(GetProcessHeap(), 0, firstEntity);
        if (secondEntity) HeapFree(GetProcessHeap(), 0, secondEntity);
        Log("SCHEDULING SELFTEST FAIL: allocation");
        return false;
    }

    *reinterpret_cast<std::int32_t*>(pool) = kExtendedObjectCapacity - 1;
    *reinterpret_cast<std::int32_t*>(pool + kObjectPoolFlagsOffset) = 1;
    *reinterpret_cast<std::int32_t*>(pool + kObjectPoolFlagsOffset +
        (kExtendedObjectCapacity - 1) * 4) = 1;
    *reinterpret_cast<void**>(pool + extendedPointersOffset) = first;
    *reinterpret_cast<void**>(pool + extendedPointersOffset +
        (kExtendedObjectCapacity - 1) * 4) = second;

    for (auto* object : {first, second}) {
        object[kObjectCustomMaterialOffset] = 1;
        *reinterpret_cast<std::int32_t*>(object + kObjectMaterialTypeOffset) = 2;
        *reinterpret_cast<void**>(object + kObjectMaterialPointerOffset) =
            reinterpret_cast<void*>(1);
    }
    *reinterpret_cast<void**>(first + kObjectGameEntityOffset) = firstEntity;
    *reinterpret_cast<void**>(second + kObjectGameEntityOffset) = secondEntity;

    const auto savedConstruct = g_constructMaterialText;
    const auto savedLayout = g_poolLayout;
    const auto savedActive = g_repairActive;
    const auto savedLogRepairs = g_logRepairs;
    const auto savedDirty = g_repairDirty;
    g_constructMaterialText = reinterpret_cast<ConstructMaterialTextFn>(
        &SyntheticConstructMaterialText);
    g_poolLayout = {extendedPointersOffset, kExtendedObjectCapacity};
    g_repairActive = true;
    g_logRepairs = false;
    g_syntheticRepairCalls = 0;
    g_syntheticFirstObject = first;
    g_syntheticSecondObject = second;
    g_syntheticOrderValid = true;

    RepairCandidate orderTest[2] = {
        {first, 0, 25.0F},
        {second, 1, 4.0F},
    };
    std::sort(orderTest, orderTest + 2, CandidateNearer);
    const bool nearestOrdering = orderTest[0].object == second;

    RepairSampObjects(pool, kExtendedObjectCapacity - 1);
    const bool passed = nearestOrdering && g_syntheticRepairCalls == 2 &&
                        g_syntheticOrderValid &&
                        !HasMalformedTextSlot(first) &&
                        !HasMalformedTextSlot(second);

    g_constructMaterialText = savedConstruct;
    g_poolLayout = savedLayout;
    g_repairActive = savedActive;
    g_logRepairs = savedLogRepairs;
    InterlockedExchange(&g_repairDirty, savedDirty);
    g_syntheticFirstObject = nullptr;
    g_syntheticSecondObject = nullptr;

    HeapFree(GetProcessHeap(), 0, secondEntity);
    HeapFree(GetProcessHeap(), 0, firstEntity);
    HeapFree(GetProcessHeap(), 0, second);
    HeapFree(GetProcessHeap(), 0, first);
    HeapFree(GetProcessHeap(), 0, pool);
    Log("SCHEDULING SELFTEST %s: full 2100-object scan; nearest ordering; "
        "ConstructMaterials phase",
        passed ? "PASS" : "FAIL");
    return passed;
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

    constexpr const char* constructMaterialTextSignature =
        "83 EC 08 53 55 56 8B E9 57 8D BD DB 02 00 00 8D B5 31 04 00 00 "
        "BB 10 00 00 00 8D 9B 00 00 00 00 8B 87 3C FF FF FF 85 C0 74 6A "
        "83 3F 02 75 65 8B 47 40 85 C0 75 5E 0F B6 56 B4";
    constexpr const char* poolMaterialLifecycleSignature =
        "53 8B D9 8B 03 57 33 FF 85 C0 7C 2D 56 8D B3 ? ? ? ? "
        "8B 86 ? ? ? ? 85 C0 74 11 8B 0E 8B 81 8B 11 00 00 "
        "85 C0 74 05 E8 ? ? ? ? 8B 03 47 83 C6 04 3B F8 7E DB "
        "5E 5F 5B C3";
    constexpr const char* objectPoolDeleteSignature =
        "66 8B 44 24 04 66 3D ? ? 56 57 8B F1 77 ? 0F B7 F8 "
        "8B 44 BE 04 85 C0 74 ? 8B 84 BE ? ? ? ? 85 C0 74 ?";
    constexpr const char* setMaterialSignature =
        "53 8B 5C 24 0C 83 FB 10 55 8B E9 0F 8F 60 01 00 00 "
        "56 8B 74 24 10 57 33 FF 3B F7 7E 0E";
    constexpr const char* setMaterialTextSignature =
        "53 8B 5C 24 08 83 FB 10 56 8B F1 0F 8F 6E 01 00 00 "
        "8B 84 9E 17 02 00 00 55 33 ED 3B C5 74 22 50 E8 ? ? ? ? "
        "8B 84 9E 0B 11 00 00 83 C4 04 3B C5 89 AC 9E 17 02 00 00";
    constexpr std::size_t lifecycleCallOffset = 41;

    auto* constructMaterialText = FindUniqueExecutablePattern(
        g_sampBase, constructMaterialTextSignature);
    auto* constructMaterials = FindUniquePatternByCallTarget(
        g_sampBase, poolMaterialLifecycleSignature, lifecycleCallOffset,
        constructMaterialText, true);
    auto* shutdownMaterials = FindUniquePatternByCallTarget(
        g_sampBase, poolMaterialLifecycleSignature, lifecycleCallOffset,
        constructMaterialText, false);
    auto* objectPoolDelete = FindUniqueExecutablePattern(
        g_sampBase, objectPoolDeleteSignature);
    auto* setMaterial = FindUniqueExecutablePattern(
        g_sampBase, setMaterialSignature);
    auto* setMaterialText = FindUniqueExecutablePattern(
        g_sampBase, setMaterialTextSignature);
    PoolLayout layout{};
    if (!constructMaterialText || !constructMaterials || !shutdownMaterials ||
        !objectPoolDelete || !setMaterial || !setMaterialText ||
        !DecodePoolLayout(constructMaterials, &layout)) {
        Log("SA-MP OBJECT REPAIR disabled: unique R1/R3-compatible signatures not found");
        g_sampBase = nullptr;
        g_sampRepairUnsupported = true;
        return;
    }

    g_poolLayout = layout;
    g_constructMaterialText = reinterpret_cast<ConstructMaterialTextFn>(
        constructMaterialText);
    g_objectPoolDelete = reinterpret_cast<ObjectPoolDeleteFn>(objectPoolDelete);
    g_constructMaterialsEpilogue = reinterpret_cast<std::uintptr_t>(
        constructMaterials + kConstructEpilogueOffset);

    g_constructMaterialsHook = new (std::nothrow) NakedHook(
        constructMaterials + kConstructHookOffset,
        &ConstructMaterialsReplacement, false);
    g_shutdownMaterialsHook = new (std::nothrow) NakedHook(
        shutdownMaterials + kShutdownDirtyHookOffset,
        &MarkMaterialsDirty, false);
    g_setMaterialHook = new (std::nothrow) SetMaterialHook(
        reinterpret_cast<SetMaterialFn>(setMaterial),
        &SetMaterialReplacement, false);
    g_setMaterialTextHook = new (std::nothrow) SetMaterialTextHook(
        reinterpret_cast<SetMaterialTextFn>(setMaterialText),
        &SetMaterialTextReplacement, false);
    if (!g_constructMaterialsHook || !g_shutdownMaterialsHook ||
        !g_setMaterialHook || !g_setMaterialTextHook ||
        !g_constructMaterialsHook->install() ||
        !g_shutdownMaterialsHook->install() || !g_setMaterialHook->install() ||
        !g_setMaterialTextHook->install()) {
        delete g_setMaterialTextHook;
        delete g_setMaterialHook;
        delete g_shutdownMaterialsHook;
        delete g_constructMaterialsHook;
        g_setMaterialTextHook = nullptr;
        g_setMaterialHook = nullptr;
        g_shutdownMaterialsHook = nullptr;
        g_constructMaterialsHook = nullptr;
        g_constructMaterialText = nullptr;
        g_objectPoolDelete = nullptr;
        g_sampBase = nullptr;
        g_sampRepairUnsupported = true;
        Log("SA-MP OBJECT REPAIR disabled: AddOn-compatible hook installation failed");
        return;
    }

    g_repairActive = true;
    InterlockedExchange(&g_repairDirty, 1);
    Log("SA-MP OBJECT REPAIR enabled by signatures: constructMaterials=%p "
        "shutdownMaterials=%p constructMaterialText=%p setMaterial=%p "
        "setMaterialText=%p poolDelete=%p; layout=%ld "
        "scheduling=AddOn-compatible quarantine=full-object",
        constructMaterials, shutdownMaterials, constructMaterialText,
        setMaterial, setMaterialText, objectPoolDelete,
        static_cast<long>(g_poolLayout.capacity));
}

void ProbeSampRepair() noexcept {
    if (g_constructMaterialsHook || g_sampRepairUnsupported) return;
    const DWORD now = GetTickCount();
    if (now - g_lastSampProbeTick < 1000) return;
    g_lastSampProbeTick = now;
    InitializeSampRepair();
}

bool ObjectOwnsAtomic(void* object, void* atomic) noexcept {
    if (!object || !atomic) return false;
    __try {
        auto* entity = *reinterpret_cast<std::uint8_t**>(
            static_cast<std::uint8_t*>(object) + kObjectGameEntityOffset);
        if (!entity) return false;
        auto* rwObject = *reinterpret_cast<std::uint8_t**>(entity + 0x18);
        if (!rwObject) return false;
        if (rwObject[0] == 1) return rwObject == atomic;
        if (rwObject[0] != 2) return false;

        auto* head = reinterpret_cast<void**>(rwObject + 8);
        auto* link = *head;
        std::int32_t visited{};
        while (link != head && ++visited <= 4096) {
            if (static_cast<std::uint8_t*>(link) - 0x40 == atomic) return true;
            link = *static_cast<void**>(link);
        }
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        return false;
    }
    return false;
}

bool QueueAtomicOwner(void* atomic, std::int32_t materialIndex,
                      const char* operation, const char* reason) noexcept {
    auto* pool = g_lastObjectPool;
    if (!pool || g_poolLayout.capacity <= 0) return false;
    auto largest = g_lastLargestObjectId;
    if (largest < 0) return false;
    if (largest >= g_poolLayout.capacity) largest = g_poolLayout.capacity - 1;

    for (std::int32_t id = 0; id <= largest; ++id) {
        void* object{};
        if (!ReadPoolObject(pool, id, &object)) return false;
        if (!object || !ObjectOwnsAtomic(object, atomic)) continue;
        QueueQuarantine(object, materialIndex, operation, reason);
        return true;
    }
    return false;
}

bool ShouldLogSphereReject(volatile LONG* counter, LONG* observed) noexcept {
    const LONG value = InterlockedIncrement(counter);
    if (observed) *observed = value;
    return value <= 20 || value % 1000 == 0;
}

void AtomicCloneSphereGuard(const NakedHook& hook) noexcept {
    const auto& context = hook.get_context();
    auto* geometry = reinterpret_cast<void*>(context.esi);
    const char* reason{};
    std::int32_t morphCount{-1};
    const void* morphTarget{};
    if (ValidateFirstMorphSphereForUse(geometry, &reason, &morphCount,
                                      &morphTarget)) return;

    LONG rejected{};
    if (ShouldLogSphereReject(&g_atomicCloneSphereRejects, &rejected)) {
        Log("RW SPHERE GUARD skipped operation=RpAtomicClone "
            "sourceAtomic=%p cloneAtomic=%p geometry=%p morphCount=%ld "
            "morphTarget=%p reason=%s rejected=%ld",
            reinterpret_cast<void*>(context.ebp),
            reinterpret_cast<void*>(context.ebx), geometry,
            static_cast<long>(morphCount), morphTarget,
            reason ? reason : "invalid RpMorphTarget",
            static_cast<long>(rejected));
    }

    // The skipped sphere is only an intermediate value. Continue through the
    // stock frame-update block; RpAtomicClone then copies the source atomic's
    // bounding sphere over this destination unconditionally at 0x749F17.
    hook.get_return_address() = kAtomicCloneAfterMorphSphereCopyAddress;
}

void AtomicSetGeometrySphereGuard(const NakedHook& hook) noexcept {
    const auto& context = hook.get_context();
    auto* atomic = reinterpret_cast<void*>(context.esi);
    auto* geometry = reinterpret_cast<void*>(context.edi);
    const char* reason{};
    std::int32_t morphCount{-1};
    const void* morphTarget{};
    if (ValidateFirstMorphSphereForUse(geometry, &reason, &morphCount,
                                      &morphTarget)) return;

    const bool ownerQueued = QueueAtomicOwner(
        atomic, -1, "RpAtomicSetGeometry",
        reason ? reason : "invalid RpMorphTarget");
    LONG rejected{};
    if (ShouldLogSphereReject(&g_atomicSetGeometrySphereRejects, &rejected)) {
        Log("RW SPHERE GUARD skipped operation=RpAtomicSetGeometry "
            "atomic=%p geometry=%p morphCount=%ld morphTarget=%p reason=%s "
            "sampOwnerQueued=%d rejected=%ld",
            atomic, geometry, static_cast<long>(morphCount), morphTarget,
            reason ? reason : "invalid RpMorphTarget", ownerQueued ? 1 : 0,
            static_cast<long>(rejected));
    }

    // Geometry ownership/refcounts were already committed. Preserve the
    // atomic's previous sphere and continue with the normal frame update.
    hook.get_return_address() = kAtomicSetGeometryAfterSphereCopyAddress;
}

bool InstallGeometrySphereGuards() noexcept {
    auto* setGeometrySite = reinterpret_cast<std::uint8_t*>(
        kAtomicSetGeometrySphereCopyAddress);
    auto* cloneSite = reinterpret_cast<std::uint8_t*>(
        kAtomicCloneSphereCopyAddress);
    constexpr const char* setGeometrySignature =
        "8B 47 5C 8D 4E 1C 83 C0 04 8B 10 89 11";
    constexpr const char* cloneSignature =
        "8B 56 5C 8D 43 1C 83 C2 04 8B 0A 89 08";

    bool setGeometryInstalled{};
    bool cloneInstalled{};
    if (MatchesExecutablePatternAt(setGeometrySite, setGeometrySignature)) {
        g_atomicSetGeometrySphereHook = new (std::nothrow) NakedHook(
            setGeometrySite, &AtomicSetGeometrySphereGuard, false);
        setGeometryInstalled = g_atomicSetGeometrySphereHook &&
                               g_atomicSetGeometrySphereHook->install();
        if (!setGeometryInstalled) {
            delete g_atomicSetGeometrySphereHook;
            g_atomicSetGeometrySphereHook = nullptr;
        }
    }
    if (MatchesExecutablePatternAt(cloneSite, cloneSignature)) {
        g_atomicCloneSphereHook = new (std::nothrow) NakedHook(
            cloneSite, &AtomicCloneSphereGuard, false);
        cloneInstalled = g_atomicCloneSphereHook &&
                         g_atomicCloneSphereHook->install();
        if (!cloneInstalled) {
            delete g_atomicCloneSphereHook;
            g_atomicCloneSphereHook = nullptr;
        }
    }

    if (!setGeometryInstalled || !cloneInstalled) {
        Log("RW SPHERE GUARD partial: RpAtomicSetGeometry=%s RpAtomicClone=%s; "
            "missing site is unreadable, already detoured, or hook failed",
            setGeometryInstalled ? "enabled" : "disabled",
            cloneInstalled ? "enabled" : "disabled");
    } else {
        Log("RW SPHERE GUARD enabled: RpAtomicSetGeometry=%p RpAtomicClone=%p; "
            "mode=preserve-nonnull-result use-site invariant",
            setGeometrySite, cloneSite);
    }
    return setGeometryInstalled && cloneInstalled;
}

bool ShouldLogInvalidAtomic(void* atomic) noexcept {
    bool shouldLog{};
    AcquireSRWLockExclusive(&g_invalidAtomicLogLock);
    for (std::int32_t index = 0; index < g_loggedInvalidAtomicCount; ++index) {
        if (g_loggedInvalidAtomics[index] == atomic) {
            ReleaseSRWLockExclusive(&g_invalidAtomicLogLock);
            return false;
        }
    }
    if (g_loggedInvalidAtomicCount <
        static_cast<std::int32_t>(std::size(g_loggedInvalidAtomics))) {
        g_loggedInvalidAtomics[g_loggedInvalidAtomicCount++] = atomic;
        shouldLog = true;
    }
    ReleaseSRWLockExclusive(&g_invalidAtomicLogLock);
    return shouldLog;
}

bool SafeRenderAlphaAtomic(void* atomic, int drawAlpha,
                           AlphaReject* reject) noexcept {
    AlphaSnapshot snapshots[kRenderAlphaMaterialLimit];
    std::uint8_t* geometry{};
    void** materials{};
    std::int32_t materialCount{};
    std::int32_t materialCapacity{};
    std::int32_t originalFlags{};

    __try {
        if (reinterpret_cast<std::uintptr_t>(atomic) < 0x10000) {
            reject->reason = "invalid RpAtomic pointer";
            return false;
        }
        geometry = *reinterpret_cast<std::uint8_t**>(
            static_cast<std::uint8_t*>(atomic) + 0x18);
        reject->geometry = geometry;
        if (reinterpret_cast<std::uintptr_t>(geometry) < 0x10000) {
            reject->reason = "invalid RpGeometry pointer";
            return false;
        }
        materials = *reinterpret_cast<void***>(geometry + 0x20);
        materialCount = *reinterpret_cast<std::int32_t*>(geometry + 0x24);
        materialCapacity = *reinterpret_cast<std::int32_t*>(geometry + 0x28);
        reject->materialCount = materialCount;
        if (materialCount < 0 || materialCapacity < 0 ||
            materialCount > materialCapacity ||
            materialCapacity > kMaximumGeometryMaterialCapacity) {
            reject->reason = "invalid RpMaterialList bounds";
            return false;
        }
        if (materialCount > kRenderAlphaMaterialLimit) {
            reject->reason = "RenderAlphaAtomic material limit exceeded";
            return false;
        }
        if (materialCount &&
            reinterpret_cast<std::uintptr_t>(materials) < 0x10000) {
            reject->reason = "invalid RpMaterialList pointer";
            return false;
        }
        for (std::int32_t index = 0; index < materialCount; ++index) {
            auto* material = static_cast<std::uint8_t*>(materials[index]);
            reject->materialIndex = index;
            reject->material = material;
            if (reinterpret_cast<std::uintptr_t>(material) < 0x10000) {
                reject->reason = "invalid RpMaterial pointer";
                return false;
            }
            volatile const auto readableProbe = material[7];
            (void)readableProbe;
        }
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        reject->reason = "RpMaterialList access fault";
        return false;
    }

    __try {
        originalFlags = *reinterpret_cast<std::int32_t*>(geometry + 8);
        *reinterpret_cast<std::int32_t*>(geometry + 8) = originalFlags | 0x40;
        for (std::int32_t index = 0; index < materialCount; ++index) {
            auto* material = static_cast<std::uint8_t*>(materials[index]);
            snapshots[index].material = material;
            snapshots[index].alpha = material[7];
            if (static_cast<int>(snapshots[index].alpha) > drawAlpha)
                material[7] = static_cast<std::uint8_t>(drawAlpha);
        }
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        reject->reason = "RpMaterial mutation access fault";
        return false;
    }

    g_atomicDefaultRender(atomic);

    __try {
        for (std::int32_t index = materialCount; index > 0; --index)
            snapshots[index - 1].material[7] = snapshots[index - 1].alpha;
        *reinterpret_cast<std::int32_t*>(geometry + 8) = originalFlags;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        reject->reason = "RpMaterial restoration access fault";
        return false;
    }
    return true;
}

void RenderAlphaAtomicReplacement(const RenderAlphaAtomicHook& hook,
                                  void*& atomic, int& drawAlpha) {
    HookReturnGuard<RenderAlphaAtomicHook> returnGuard(hook);
    AlphaReject reject{};
    if (SafeRenderAlphaAtomic(atomic, drawAlpha, &reject)) return;

    const bool ownerQueued = QueueAtomicOwner(
        atomic, reject.materialIndex, "RenderAlphaAtomic",
        reject.reason ? reject.reason : "invalid alpha atomic");
    if (ShouldLogInvalidAtomic(atomic)) {
        Log("RENDER ALPHA GUARD skipped atomic=%p geometry=%p "
            "materialCount=%ld materialIndex=%ld material=%p reason=%s "
            "sampOwnerQueued=%d", atomic, reject.geometry,
            static_cast<long>(reject.materialCount),
            static_cast<long>(reject.materialIndex), reject.material,
            reject.reason ? reject.reason : "unknown", ownerQueued ? 1 : 0);
    }
}

bool InstallRenderAlphaGuard() noexcept {
    auto* renderAlphaAtomic = reinterpret_cast<std::uint8_t*>(
        kRenderAlphaAtomicAddress);
    auto* defaultRender = reinterpret_cast<std::uint8_t*>(
        kAtomicDefaultRenderAddress);
    constexpr const char* signature =
        "81 EC 98 00 00 00 53 55 8B AC 24 A4 00 00 00 56 57 "
        "8B 7D 18 8B 5F 08 8B C3 83 C8 40 89 47 08";
    if (!MatchesExecutablePatternAt(renderAlphaAtomic, signature) ||
        !CheckRange(defaultRender, 1)) {
        Log("RENDER ALPHA GUARD disabled: GTA entry is unreadable or already detoured");
        return false;
    }

    g_atomicDefaultRender = reinterpret_cast<AtomicDefaultRenderFn>(defaultRender);
    g_renderAlphaAtomicHook = new (std::nothrow) RenderAlphaAtomicHook(
        reinterpret_cast<RenderAlphaAtomicFn>(renderAlphaAtomic),
        &RenderAlphaAtomicReplacement, false);
    if (!g_renderAlphaAtomicHook || !g_renderAlphaAtomicHook->install()) {
        delete g_renderAlphaAtomicHook;
        g_renderAlphaAtomicHook = nullptr;
        g_atomicDefaultRender = nullptr;
        Log("RENDER ALPHA GUARD disabled: kthook installation failed");
        return false;
    }
    Log("RENDER ALPHA GUARD enabled at %p; mode=zero-allocation fail-closed; "
        "materialLimit=%ld; discovery=GTA 1.0 US VA", renderAlphaAtomic,
        static_cast<long>(kRenderAlphaMaterialLimit));
    return true;
}

bool HasLiveTagDescriptor(void* entity, std::int32_t* observedCount) noexcept {
    if (!g_tagCount || !g_tagDescriptors) return false;
    __try {
        const auto count = *g_tagCount;
        if (observedCount) *observedCount = count;
        return ContainsTagDescriptor(entity, g_tagDescriptors, count);
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        if (observedCount) *observedCount = -1;
        return false;
    }
}

bool ReadEntityAtomic(void* entity, void** atomic) noexcept {
    if (!entity || !atomic) return false;
    __try {
        *atomic = *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(entity) + 0x18);
        return true;
    } __except (MemoryAccessFilter(GetExceptionCode())) {
        *atomic = nullptr;
        return false;
    }
}

bool ShouldLogInvalidTag(void* entity) noexcept {
    for (std::int32_t index = 0; index < g_loggedInvalidTagCount; ++index) {
        if (g_loggedInvalidTags[index] == entity) return false;
    }
    if (g_loggedInvalidTagCount ==
        static_cast<std::int32_t>(std::size(g_loggedInvalidTags))) return false;
    g_loggedInvalidTags[g_loggedInvalidTagCount++] = entity;
    return true;
}

void LogInvalidTag(void* entity, std::int32_t tagCount,
                   const char* operation) noexcept {
    if (!ShouldLogInvalidTag(entity)) return;
    const auto diagnostics = ReadObjectDiagnostics(entity);
    Log("GTA TAG INVARIANT GUARD skipped operation=%s entity=%p modelId=%ld "
        "position=(%.3f,%.3f,%.3f) positionValid=%d tagCount=%ld "
        "reason=tag model has no live CTagManager descriptor",
        operation, entity, static_cast<long>(diagnostics.modelId),
        diagnostics.x, diagnostics.y, diagnostics.z,
        diagnostics.hasPosition ? 1 : 0, static_cast<long>(tagCount));
}

void ResetTagAlphaReplacement(const ResetTagAlphaHook& hook, void*& entity) {
    HookReturnGuard<ResetTagAlphaHook> returnGuard(hook);
    std::int32_t tagCount{-1};
    if (HasLiveTagDescriptor(entity, &tagCount)) {
        hook.get_trampoline()(entity);
        return;
    }
    LogInvalidTag(entity, tagCount, "ResetAlpha");
}

std::uint8_t GetTagAlphaReplacement(const GetTagAlphaHook& hook,
                                    void*& entity) {
    HookReturnGuard<GetTagAlphaHook> returnGuard(hook);
    void* atomic{};
    if (ReadEntityAtomic(entity, &atomic) && atomic)
        return hook.get_trampoline()(entity);

    std::int32_t tagCount{-1};
    if (HasLiveTagDescriptor(entity, &tagCount))
        return hook.get_trampoline()(entity);

    LogInvalidTag(entity, tagCount, "GetAlpha");
    return 0;
}

void SetTagAlphaReplacement(const SetTagAlphaHook& hook, void*& entity,
                            std::uint8_t& alpha) {
    HookReturnGuard<SetTagAlphaHook> returnGuard(hook);
    std::int32_t tagCount{-1};
    if (HasLiveTagDescriptor(entity, &tagCount)) {
        hook.get_trampoline()(entity, alpha);
        return;
    }

    void* atomic{};
    if (ReadEntityAtomic(entity, &atomic) && atomic && g_setAtomicUserValue)
        g_setAtomicUserValue(atomic, alpha);
    LogInvalidTag(entity, tagCount, "SetAlpha");
}

bool InstallTagInvariantGuards() noexcept {
    auto* resetAlpha = reinterpret_cast<std::uint8_t*>(
        kResetTagAlphaAddress);
    auto* getAlpha = reinterpret_cast<std::uint8_t*>(kGetTagAlphaAddress);
    auto* setAlpha = reinterpret_cast<std::uint8_t*>(kSetTagAlphaAddress);
    constexpr const char* resetSignature =
        "8B 4C 24 04 8B 51 18 85 D2 74 2C A1 ? ? ? ? 85 C0 56 74 10 "
        "8B 34 C5 ? ? ? ? 48 3B F1 74 17 85 C0 75 F0 33 C0 "
        "66 0F B6 40 04";
    constexpr const char* getSignature =
        "8B 4C 24 04 8B 41 18 85 C0 74 09 89 44 24 04 E9 ? ? ? ? "
        "A1 ? ? ? ? 85 C0 74 13 8D 49 00 8B 14 C5 ? ? ? ? 48 3B D1";
    constexpr const char* setSignature =
        "51 53 8A 5C 24 10 56 8B 74 24 10 8B 46 18 85 C0 57 "
        "C6 44 24 0F 00 74 0E 66 0F B6 CB 51 50 E8 ? ? ? ?";
    if (!MatchesExecutablePatternAt(resetAlpha, resetSignature) ||
        !MatchesExecutablePatternAt(getAlpha, getSignature) ||
        !MatchesExecutablePatternAt(setAlpha, setSignature)) {
        Log("GTA TAG INVARIANT GUARD disabled: GTA entries are unreadable or already detoured");
        return false;
    }

    g_tagCount = reinterpret_cast<std::int32_t*>(kTagCountAddress);
    g_tagDescriptors = reinterpret_cast<std::uint8_t*>(
        kTagDescriptorsAddress);
    g_setAtomicUserValue = reinterpret_cast<SetAtomicUserValueFn>(
        kSetAtomicUserValueAddress);
    if (!CheckRange(g_tagCount, sizeof(*g_tagCount)) ||
        !CheckRange(g_tagDescriptors,
                    static_cast<std::size_t>(kMaximumTagCount) * 8) ||
        !CheckRange(reinterpret_cast<void*>(g_setAtomicUserValue), 1)) {
        g_tagCount = nullptr;
        g_tagDescriptors = nullptr;
        g_setAtomicUserValue = nullptr;
        Log("GTA TAG INVARIANT GUARD disabled: decoded GTA state is unreadable");
        return false;
    }

    g_resetTagAlphaHook = new (std::nothrow) ResetTagAlphaHook(
        reinterpret_cast<ResetTagAlphaFn>(resetAlpha),
        &ResetTagAlphaReplacement, false);
    if (!g_resetTagAlphaHook || !g_resetTagAlphaHook->install()) {
        delete g_resetTagAlphaHook;
        g_resetTagAlphaHook = nullptr;
        g_tagCount = nullptr;
        g_tagDescriptors = nullptr;
        g_setAtomicUserValue = nullptr;
        Log("GTA TAG INVARIANT GUARD disabled: ResetAlpha hook installation failed");
        return false;
    }

    g_getTagAlphaHook = new (std::nothrow) GetTagAlphaHook(
        reinterpret_cast<GetTagAlphaFn>(getAlpha),
        &GetTagAlphaReplacement, false);
    g_setTagAlphaHook = new (std::nothrow) SetTagAlphaHook(
        reinterpret_cast<SetTagAlphaFn>(setAlpha),
        &SetTagAlphaReplacement, false);
    if (!g_getTagAlphaHook || !g_getTagAlphaHook->install() ||
        !g_setTagAlphaHook || !g_setTagAlphaHook->install()) {
        Log("GTA TAG INVARIANT GUARD partial: ResetAlpha protected; "
            "GetAlpha/SetAlpha hook installation failed");
        return false;
    }
    Log("GTA TAG INVARIANT GUARD enabled: reset=%p get=%p set=%p; "
        "tagCount=%p descriptors=%p; mode=complete descriptor invariant; "
        "discovery=GTA 1.0 US VA",
        resetAlpha, getAlpha, setAlpha, g_tagCount, g_tagDescriptors);
    return true;
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

void Initialize() {
    Log("RenderAlphaFix 1.2 initializing from GTA GameLoop; "
        "mode=SA-MP object repair + invalid-object quarantine + "
        "RenderWare use-site guards + zero-allocation alpha guard + "
        "narrow tag invariant guards");

    HMODULE pinnedModule{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(g_module), &pinnedModule)) {
        Log("FATAL: unable to pin ASI module; repair not installed");
        return;
    }

    InstallRenderAlphaGuard();
    InstallGeometrySphereGuards();
    InstallTagInvariantGuards();
    SyntheticTests();
    SchedulingSelfTest();
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
        Log("RenderAlphaFix 1.2 ASI LOADED in DllMain; installing GameLoop hook");
        if (!InstallMainLoopHook()) {
            Log("FATAL: GameLoop hook installation failed; SA-MP repair is inactive");
            OutputDebugStringA("RenderAlphaFix: failed to install GameLoop hook at 0x53E968\n");
        }
    }
    return TRUE;
}
