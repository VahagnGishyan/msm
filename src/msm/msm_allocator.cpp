#include "msm_allocator.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <string>

// =============================================================================
// Atomic helpers for lock-free SWMR protocol.
// Uses std::atomic_ref (C++20) to perform release/acquire operations on
// fields within raw byte slots without requiring std::atomic<T> layout.
//
// Slot fields that are "commit points" (size, length, count, data_ptr):
//   Writer: store with memory_order_release (publishes preceding writes)
//   Reader: load with memory_order_acquire (sees all writes before publish)
//
// Non-commit fields (capacity) use relaxed ordering — they are only read
// by the writer itself or after the commit point has been observed.
// =============================================================================
namespace
{
    // --- 4-byte (uint32) atomic helpers ---
    inline std::uint32_t atomic_load_u32(std::int8_t* addr, std::memory_order order = std::memory_order_acquire)
    {
        auto& ref = *reinterpret_cast<std::uint32_t*>(addr);
        return std::atomic_ref<std::uint32_t>(ref).load(order);
    }

    inline void atomic_store_u32(std::int8_t* addr, std::uint32_t val, std::memory_order order = std::memory_order_release)
    {
        auto& ref = *reinterpret_cast<std::uint32_t*>(addr);
        std::atomic_ref<std::uint32_t>(ref).store(val, order);
    }

    // --- 8-byte (pointer / uint64) atomic helpers ---
    inline std::int8_t* atomic_load_ptr(std::int8_t* addr, std::memory_order order = std::memory_order_acquire)
    {
        auto& ref = *reinterpret_cast<std::uintptr_t*>(addr);
        auto val = std::atomic_ref<std::uintptr_t>(ref).load(order);
        return reinterpret_cast<std::int8_t*>(val);
    }

    inline void atomic_store_ptr(std::int8_t* addr, std::int8_t* val, std::memory_order order = std::memory_order_release)
    {
        auto& ref = *reinterpret_cast<std::uintptr_t*>(addr);
        std::atomic_ref<std::uintptr_t>(ref).store(reinterpret_cast<std::uintptr_t>(val), order);
    }
}

// =============================================================================
// Contract enforcement macros (C1–C8).
// MSM_REQUIRE: checked in both debug and release (critical invariants)
// MSM_DEBUG_ASSERT: checked only in debug builds (performance-sensitive checks)
// =============================================================================

#define MSM_REQUIRE(cond, contract, ...) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "[MSM " contract "] FATAL: " __VA_ARGS__); \
            std::fprintf(stderr, "\n"); \
            std::abort(); \
        } \
    } while (0)

#ifdef NDEBUG
    #define MSM_DEBUG_ASSERT(cond, contract, ...) ((void)0)
#else
    #define MSM_DEBUG_ASSERT(cond, contract, ...) MSM_REQUIRE(cond, contract, __VA_ARGS__)
#endif

namespace
{
    struct entry
    {
        std::int8_t*      ptr;
        std::uint64_t     size;
        msm_destructor_fn destructor;
        std::uint64_t     schema_hash; // 0 = no schema attached (legacy alloc)
    };

    std::mutex& get_mutex()
    {
        static std::mutex mtx;
        return mtx;
    }

    std::unordered_map<std::string, entry>& get_registry()
    {
        static std::unordered_map<std::string, entry> reg;
        return reg;
    }
}

extern "C"
{

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_alloc(const char* name, std::uint64_t size, msm_destructor_fn destructor)
{
    if (!name || size == 0) return nullptr;

    std::lock_guard<std::mutex> lock(get_mutex());

    auto& reg = get_registry();

    // Duplicate name = programmer error -> terminate
    if (reg.find(name) != reg.end())
    {
        std::fprintf(stderr, "[msm_allocator] FATAL: duplicate name '%s'\n", name);
        std::abort();
    }

    auto* ptr = static_cast<std::int8_t*>(std::malloc(static_cast<std::size_t>(size)));
    if (!ptr) return nullptr;

    std::memset(ptr, 0, static_cast<std::size_t>(size));

    reg[name] = entry{ ptr, size, destructor, /*schema_hash=*/0 };
    return ptr;
}

// FNV-1a 64-bit hash of a null-terminated string.
// Reference: http://www.isthe.com/chongo/tech/comp/fnv/
EXPORT std::uint64_t PROJECT_SHARED_CCA msm_schema_hash_fnv1a(const char* schema_str)
{
    if (!schema_str) return 0;
    std::uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    while (*schema_str)
    {
        hash ^= static_cast<std::uint8_t>(*schema_str++);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_alloc_with_schema(
    const char* name, std::uint64_t size, std::uint64_t schema_hash, msm_destructor_fn destructor)
{
    if (!name || size == 0) return nullptr;

    std::lock_guard<std::mutex> lock(get_mutex());

    auto& reg = get_registry();

    if (reg.find(name) != reg.end())
    {
        std::fprintf(stderr, "[msm_allocator] FATAL: duplicate name '%s'\n", name);
        std::abort();
    }

    auto* ptr = static_cast<std::int8_t*>(std::malloc(static_cast<std::size_t>(size)));
    if (!ptr) return nullptr;

    std::memset(ptr, 0, static_cast<std::size_t>(size));

    reg[name] = entry{ ptr, size, destructor, schema_hash };
    return ptr;
}

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_get_with_schema(const char* name, std::uint64_t schema_hash)
{
    if (!name) return nullptr;

    std::lock_guard<std::mutex> lock(get_mutex());

    auto& reg = get_registry();
    auto it = reg.find(name);
    if (it == reg.end()) return nullptr;

    if (it->second.schema_hash != schema_hash)
    {
        // C2 (Schema agreement) violation — silent data corruption is the
        // worst possible failure mode for cross-language shared memory.
        // Fail-fast with diagnostic.
        std::fprintf(stderr,
            "[msm_allocator] FATAL: schema hash mismatch for '%s'\n"
            "  expected (caller): 0x%016llx\n"
            "  stored (segment):  0x%016llx\n"
            "  Caller and segment owner agreed on different layouts.\n"
            "  This would have caused silent corruption — aborting.\n",
            name,
            static_cast<unsigned long long>(schema_hash),
            static_cast<unsigned long long>(it->second.schema_hash));
        std::abort();
    }

    return it->second.ptr;
}

EXPORT std::uint64_t PROJECT_SHARED_CCA msm_get_schema_hash(const char* name)
{
    if (!name) return 0;
    std::lock_guard<std::mutex> lock(get_mutex());
    auto& reg = get_registry();
    auto it = reg.find(name);
    if (it == reg.end()) return 0;
    return it->second.schema_hash;
}

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_get(const char* name)
{
    if (!name) return nullptr;

    std::lock_guard<std::mutex> lock(get_mutex());

    auto& reg = get_registry();
    auto it = reg.find(name);
    if (it == reg.end()) return nullptr;

    return it->second.ptr;
}

EXPORT void PROJECT_SHARED_CCA msm_free(const char* name)
{
    if (!name) return;

    std::lock_guard<std::mutex> lock(get_mutex());

    auto& reg = get_registry();
    auto it = reg.find(name);
    if (it == reg.end()) return;

    // Call destructor before freeing (cleans up nested allocations)
    if (it->second.destructor)
    {
        it->second.destructor(it->second.ptr);
    }

    std::free(it->second.ptr);
    reg.erase(it);
}

// =============================================================================
// Array C API — Variant B layout (since 2026-05-26 evening).
//
// Slot layout (16 bytes, for API uniformity with string/object):
//   [header:8 | data_ptr:8]
//
// Buffer layout (allocated separately, lives in the segment heap):
//   [size:4 | cap:4 | elements: cap × item_size bytes]
//
// Rationale (consistent with string/object Variant B):
//   Embedding size+cap inside the buffer makes the (ptr, size) snapshot
//   physically inseparable — every grow/CoW operation publishes a fully
//   consistent state via a SINGLE release_store on data_ptr. For push_back,
//   size is updated in-place inside the existing buffer header with a
//   release_store, paired with reader's acquire_load on the same location.
// =============================================================================

namespace
{
    constexpr std::size_t ARR_BUF_HEADER = 8; // [size:4 | cap:4]

    inline std::uint32_t arr_buf_size(std::int8_t* buf, std::memory_order order = std::memory_order_acquire)
    {
        return atomic_load_u32(buf, order);
    }
    inline std::uint32_t arr_buf_cap(std::int8_t* buf)
    {
        return atomic_load_u32(buf + 4, std::memory_order_relaxed); // writer-only field
    }
    inline std::int8_t* arr_buf_data(std::int8_t* buf)
    {
        return buf + ARR_BUF_HEADER;
    }
    inline void arr_buf_set_size(std::int8_t* buf, std::uint32_t v, std::memory_order order = std::memory_order_release)
    {
        atomic_store_u32(buf, v, order);
    }
    inline void arr_buf_set_cap(std::int8_t* buf, std::uint32_t v)
    {
        atomic_store_u32(buf + 4, v, std::memory_order_relaxed);
    }
}

EXPORT std::uint32_t PROJECT_SHARED_CCA msm_array_size(std::int8_t* slot)
{
    if (!slot) return 0;
    std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire
    if (!buf) return 0;
    return arr_buf_size(buf); // acquire — pairs with writer's commit
}

EXPORT std::uint32_t PROJECT_SHARED_CCA msm_array_capacity(std::int8_t* slot)
{
    if (!slot) return 0;
    std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed); // writer-only typical call
    if (!buf) return 0;
    return arr_buf_cap(buf);
}

EXPORT void PROJECT_SHARED_CCA msm_array_reserve(std::int8_t* slot, std::uint32_t new_cap, std::uint32_t item_size)
{
    if (!slot || item_size == 0) return;

    // Writer-only reads (SWMR contract C1)
    std::int8_t* old_buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
    std::uint32_t old_cap = old_buf ? arr_buf_cap(old_buf) : 0;
    if (new_cap <= old_cap) return;
    std::uint32_t old_size = old_buf ? arr_buf_size(old_buf, std::memory_order_relaxed) : 0;

    // Allocate new buffer with header
    std::size_t new_bytes = ARR_BUF_HEADER + static_cast<std::size_t>(new_cap) * item_size;
    auto* new_buf = static_cast<std::int8_t*>(std::malloc(new_bytes));
    if (!new_buf) return;
    std::memset(new_buf, 0, new_bytes);

    // Write header (private — buffer not yet published)
    arr_buf_set_size(new_buf, old_size, std::memory_order_relaxed);
    arr_buf_set_cap(new_buf, new_cap);

    // Copy existing elements (private)
    if (old_buf && old_size > 0)
    {
        std::memcpy(arr_buf_data(new_buf), arr_buf_data(old_buf),
                    static_cast<std::size_t>(old_size) * item_size);
    }

    // SINGLE COMMIT POINT: publish new buffer with release.
    // Old buffer NOT freed (bump allocator policy, G2).
    atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);
}

// Reserve next slot without committing size. Caller must fill the slot then
// call msm_array_commit_push. This split prevents a data race where the
// caller writes the element value AFTER push_back already committed size+1.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_array_prepare_push(std::int8_t* slot, std::uint32_t item_size)
{
    if (!slot || item_size == 0) return nullptr;

    // Writer-only reads
    std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
    std::uint32_t sz = buf ? arr_buf_size(buf, std::memory_order_relaxed) : 0;
    std::uint32_t cap = buf ? arr_buf_cap(buf) : 0;

    if (!buf || sz >= cap)
    {
        std::uint32_t new_cap = (cap == 0) ? 8 : cap * 2;
        msm_array_reserve(slot, new_cap, item_size);
        // Re-load buf after grow
        buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
        if (!buf) return nullptr;
    }

    // Compute element pointer; zero it out (private write before commit).
    std::int8_t* elem = arr_buf_data(buf) + static_cast<std::size_t>(sz) * item_size;
    std::memset(elem, 0, item_size);

    // Size is NOT incremented here. Caller fills the slot, then commits.
    return elem;
}

// Commit a previously-prepared push by publishing size+1 with release.
// The release on buf header's size field pairs with reader's acquire_load
// on the same field (post-acquire-load-of-ptr).
EXPORT void PROJECT_SHARED_CCA msm_array_commit_push(std::int8_t* slot)
{
    if (!slot) return;
    std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed); // writer-only
    if (!buf) return;
    std::uint32_t sz = arr_buf_size(buf, std::memory_order_relaxed);
    arr_buf_set_size(buf, sz + 1, std::memory_order_release);
}

// Convenience wrapper — single-threaded use only. For SWMR-safe pushes, use
// the prepare+commit API.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_array_push_back(std::int8_t* slot, std::uint32_t item_size)
{
    std::int8_t* elem = msm_array_prepare_push(slot, item_size);
    if (elem) msm_array_commit_push(slot);
    return elem;
}

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_array_get(std::int8_t* slot, std::uint32_t index, std::uint32_t item_size)
{
    if (!slot || item_size == 0) return nullptr;

    // Reader protocol: acquire_load(slot+8) → buf → read size from buf header.
    // The acquire-load on slot+8 pairs with writer's release_store, making
    // the entire buffer (header + data) visible.
    std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire
    if (!buf) return nullptr;

    std::uint32_t sz = arr_buf_size(buf); // acquire — pairs with commit_push
    if (index >= sz) return nullptr; // bounds check

    return arr_buf_data(buf) + static_cast<std::size_t>(index) * item_size;
}

EXPORT void PROJECT_SHARED_CCA msm_array_clear(std::int8_t* slot)
{
    if (!slot) return;
    std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed); // writer-only
    if (!buf) return;
    // Commit point: publish size=0 with release on buffer header
    arr_buf_set_size(buf, 0, std::memory_order_release);
}

// =============================================================================
// String C API — THE single implementation of string behavior.
// msm::string in string.hpp delegates to these functions.
// C# MsmString delegates to these functions via P/Invoke.
//
// Slot layout (24 bytes, for API uniformity with array/object):
//   [header:8 | data_ptr:8 | UNUSED:4 | UNUSED:4]
//   slot+16 and slot+20 are no longer used — len and cap now live inside the
//   data buffer header (Variant B pattern, same as object).
//
// Buffer layout (allocated separately, lives in the segment heap):
//   [len:4 | cap:4 | data:cap bytes including null terminator]
//   len = string length in bytes (NOT including null terminator)
//   cap = total bytes in data area (typically len+1 for tight CoW fit)
//
// Rationale (single commit point — G1 linearizability + closes REMARKS #7):
//   The previous design used two separate release_store on (ptr, len). For
//   arbitrary workloads, this admits the mixed observation (OLD len, NEW ptr)
//   in which the reader reads OLD-len bytes from the NEW buffer. For
//   monotonically growing strings this is safe (NEW buf has >= OLD-len bytes),
//   but for shrinking strings it produces out-of-bounds reads.
//
//   Variant B embeds len INSIDE the buffer. Every set is a single atomic
//   commit = release_store(data_ptr, new_buf). The (ptr, len) snapshot is
//   physically inseparable, eliminating all mixed states regardless of
//   workload.
//
// Old buffer at slot+8 is NOT freed (bump allocator policy, see G2).
// =============================================================================

namespace
{
    constexpr std::size_t STR_BUF_HEADER = 8; // [len:4 | cap:4]

    inline std::uint32_t str_buf_len(std::int8_t* buf, std::memory_order order = std::memory_order_acquire)
    {
        return atomic_load_u32(buf, order);
    }
    inline std::uint32_t str_buf_cap(std::int8_t* buf)
    {
        // Capacity is writer-only state for future allocation decisions; relaxed.
        return atomic_load_u32(buf + 4, std::memory_order_relaxed);
    }
    inline std::int8_t* str_buf_data(std::int8_t* buf)
    {
        return buf + STR_BUF_HEADER;
    }
    inline void str_buf_set_len(std::int8_t* buf, std::uint32_t v, std::memory_order order = std::memory_order_release)
    {
        atomic_store_u32(buf, v, order);
    }
    inline void str_buf_set_cap(std::int8_t* buf, std::uint32_t v)
    {
        atomic_store_u32(buf + 4, v, std::memory_order_relaxed);
    }
}

EXPORT void PROJECT_SHARED_CCA msm_string_set(std::int8_t* slot, const char* str, std::uint32_t len)
{
    if (!slot) return;
    if (!str) len = 0;

    if (len == 0)
    {
        // Empty string: publish nullptr. Reader sees null → returns "".
        // Old buffer NOT freed (bump allocator).
        atomic_store_ptr(slot + 8, nullptr, std::memory_order_release);
        return;
    }

    // CoW + Variant B (single commit point):
    //   1. malloc new buffer with header (private)
    //   2. write header (len + cap) and data (private)
    //   3. release_store(slot+8, new_buf) — SINGLE commit
    // Any reader observing the new buffer via acquire-load on slot+8 sees
    // a fully consistent (len, data) pair — both physically in the same buffer.
    std::uint32_t new_cap = len + 1; // tight fit, includes null terminator
    std::size_t buf_size = STR_BUF_HEADER + new_cap;
    auto* new_buf = static_cast<std::int8_t*>(std::malloc(buf_size));
    if (!new_buf) return;

    // Write header (private — buffer not yet published).
    str_buf_set_len(new_buf, len, std::memory_order_relaxed);
    str_buf_set_cap(new_buf, new_cap);

    // Write data (private).
    std::int8_t* data = str_buf_data(new_buf);
    std::memcpy(data, str, len);
    data[len] = '\0';

    // SINGLE COMMIT POINT: publish new buffer with release.
    // Reader's acquire-load on slot+8 pairs with this, making header + data visible.
    // Old buffer NOT freed (bump allocator policy, G2).
    atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);
}

EXPORT const char* PROJECT_SHARED_CCA msm_string_get(std::int8_t* slot)
{
    if (!slot) return "";

    // Reader protocol: acquire_load(slot+8) → buf → read len from buf header.
    // The acquire-load on slot+8 pairs with writer's release_store, making
    // the entire buffer (header + data) visible. No second atomic needed.
    std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire
    if (!buf) return "";

    std::uint32_t len = str_buf_len(buf); // acquire (could be relaxed, but stays acquire for clarity)
    if (len == 0) return "";

    // Return pointer to data area (skip 8-byte header).
    return reinterpret_cast<const char*>(str_buf_data(buf));
}

EXPORT std::uint32_t PROJECT_SHARED_CCA msm_string_length(std::int8_t* slot)
{
    if (!slot) return 0;
    std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire
    if (!buf) return 0;
    return str_buf_len(buf); // acquire
}

EXPORT void PROJECT_SHARED_CCA msm_string_clear(std::int8_t* slot)
{
    if (!slot) return;
    // Publish nullptr. Reader sees null → returns "".
    atomic_store_ptr(slot + 8, nullptr, std::memory_order_release);
}

// =============================================================================
// Record C API — field access via pointer + offset.
// Trivial but provides uniform C ABI for all language bindings.
// =============================================================================

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_record_get_field(std::int8_t* record_ptr, std::uint32_t offset)
{
    if (!record_ptr) return nullptr;
    return record_ptr + offset;
}

// =============================================================================
// Object C API — dynamic property bag.
//
// Slot layout (16 bytes, post slot_size shrink, post Variant B, post hash-names):
//   [header:8 | entries_ptr:8]
//
// Entries buffer layout (allocated separately, lives in the segment heap):
//   [count:4 | capacity:4 | entry[0]:24 | entry[1]:24 | ... | entry[N-1]:24]
//
// Entry layout (24B, post-hash-names — closes REMARKS #3):
//   [name_hash:8 | type_id:2 | unused:2 | size:4 | value_ptr:8]
//
//   - name_hash: FNV-1a 64-bit hash of the field name (collision-resistant)
//   - type_id:   one of msm_type enum values (2 bytes)
//   - size:      bytes occupied by the value (for size accounting)
//   - value_ptr: pointer to the malloc'd value buffer
//
// Why hash names instead of storing them:
//   - Lookup becomes 64-bit integer comparison (single instruction) instead
//     of memcmp + pointer dereference. ~2-3× faster in benchmarks.
//   - No per-entry name buffer allocation. Eliminates name-related memory
//     leak (was ~strlen(name)+1 bytes per ever-set field under bump-allocator
//     policy).
//   - Better cache locality: entry becomes self-contained, no pointer chasing
//     into separate name buffers.
//   - Trade-off: cannot reconstruct field name from a memory dump. For
//     debugging, callers should keep their own name table or use schema
//     metadata externally.
//
// Hash collision risk: FNV-1a 64-bit has ~1/2^64 collision probability for
// realistic field-name vocabularies (<1M unique names per object). For
// safety-critical applications, an optional debug-mode name table could be
// added (Future Work).
//
// Rationale (single commit point — G1 linearizability):
//   By placing count INSIDE the entries buffer, every CoW operation (remove,
//   grow) publishes a fully consistent (ptr, count) pair via a SINGLE
//   release_store on the entries_ptr. Reader does acquire_load(entries_ptr)
//   then reads count from the buffer header — no mixed-state observation
//   like (NEW count, OLD ptr) or (OLD count, NEW ptr) is possible because
//   count is physically part of the buffer.
//
//   For in-place add (no grow), count is updated in-place in the buffer
//   header with release_store; reader's acquire_load on count pairs with it.
// =============================================================================

namespace
{

    constexpr auto type_bool    = static_cast<std::uint16_t>(msm_type::boolean);
    constexpr auto type_int32   = static_cast<std::uint16_t>(msm_type::int32);
    constexpr auto type_float32 = static_cast<std::uint16_t>(msm_type::float32);
    constexpr auto type_int64   = static_cast<std::uint16_t>(msm_type::int64);
    constexpr auto type_float64 = static_cast<std::uint16_t>(msm_type::float64);
    constexpr auto type_string  = static_cast<std::uint16_t>(msm_type::string);
    constexpr auto type_array   = static_cast<std::uint16_t>(msm_type::array);
    constexpr auto type_object  = static_cast<std::uint16_t>(msm_type::object);
    constexpr std::size_t ENTRY_SIZE = 24;
    constexpr std::size_t OBJ_BUF_HEADER = 8; // [count:4 | capacity:4]

    // --- Entries buffer accessors ---
    // Buffer layout: [count:4 | capacity:4 | entry[0] | entry[1] | ...]

    inline std::uint32_t obj_buf_count(std::int8_t* buf, std::memory_order order = std::memory_order_acquire)
    {
        return atomic_load_u32(buf, order);
    }
    inline std::uint32_t obj_buf_capacity(std::int8_t* buf)
    {
        // Capacity is fixed at allocation time; writer-only consultations use relaxed.
        return atomic_load_u32(buf + 4, std::memory_order_relaxed);
    }
    inline std::int8_t* obj_buf_entries(std::int8_t* buf)
    {
        return buf + OBJ_BUF_HEADER;
    }
    inline void obj_buf_set_count(std::int8_t* buf, std::uint32_t v, std::memory_order order = std::memory_order_release)
    {
        atomic_store_u32(buf, v, order);
    }
    inline void obj_buf_set_capacity(std::int8_t* buf, std::uint32_t v)
    {
        atomic_store_u32(buf + 4, v, std::memory_order_relaxed);
    }

    // --- Entry accessors (post hash-names — closes REMARKS #3) ---
    // Layout: [name_hash:8 | type_id:2 | unused:2 | size:4 | value_ptr:8] = 24 bytes
    inline std::uint64_t entry_name_hash(std::int8_t* e) { std::uint64_t v; std::memcpy(&v, e, 8); return v; }
    inline std::uint16_t entry_type_id(std::int8_t* e) { std::uint16_t v; std::memcpy(&v, e + 8, 2); return v; }
    inline std::uint32_t entry_size(std::int8_t* e) { std::uint32_t v; std::memcpy(&v, e + 12, 4); return v; }
    inline std::int8_t* entry_value_ptr(std::int8_t* e) { std::int8_t* p; std::memcpy(&p, e + 16, 8); return p; }

    inline void entry_set_name_hash(std::int8_t* e, std::uint64_t v) { std::memcpy(e, &v, 8); }
    inline void entry_set_type_id(std::int8_t* e, std::uint16_t v) { std::memcpy(e + 8, &v, 2); }
    inline void entry_set_size(std::int8_t* e, std::uint32_t v) { std::memcpy(e + 12, &v, 4); }
    inline void entry_set_value_ptr(std::int8_t* e, std::int8_t* p) { std::memcpy(e + 16, &p, 8); }

    // FNV-1a 64-bit — same as msm_schema_hash_fnv1a but inline for hot path.
    // Computes hash of [name, name+len), without null terminator dependency.
    inline std::uint64_t hash_name(const char* name, std::size_t len)
    {
        std::uint64_t hash = 14695981039346656037ULL; // FNV offset basis
        for (std::size_t i = 0; i < len; ++i)
        {
            hash ^= static_cast<std::uint8_t>(name[i]);
            hash *= 1099511628211ULL; // FNV prime
        }
        return hash;
    }

    // Find entry by name hash. Returns pointer to entry or nullptr.
    // Reader path: acquire_load(entries_ptr) → acquire_load(count from buf header)
    // → iterate. Count is physically inside the buffer, so (ptr, count) snapshot
    // is always self-consistent. Each entry's name_hash is compared with a
    // single 64-bit integer compare — no memcmp, no pointer chasing.
    std::int8_t* object_find_entry_by_hash(std::int8_t* slot, std::uint64_t name_hash)
    {
        std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire — reader sees a consistent buffer
        if (!buf) return nullptr;

        std::uint32_t count = obj_buf_count(buf); // acquire — pairs with writer's commit on count
        if (count == 0) return nullptr;

        std::int8_t* entries = obj_buf_entries(buf);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::int8_t* e = entries + i * ENTRY_SIZE;
            if (entry_name_hash(e) == name_hash) return e;
        }
        return nullptr;
    }

    // Convenience overload: compute hash from name string then search.
    std::int8_t* object_find_entry(std::int8_t* slot, const char* name, std::size_t name_len)
    {
        return object_find_entry_by_hash(slot, hash_name(name, name_len));
    }

    // Ensure there is room for one more entry. If the buffer is null or full,
    // allocate a new (larger) buffer, copy existing entries, and publish the
    // new pointer with release. Does NOT update count — caller must call
    // object_commit_add after writing entry data.
    //
    // Returns pointer to the slot for the new entry (at position 'count'),
    // or nullptr on allocation failure.
    std::int8_t* object_add_entry(std::int8_t* slot)
    {
        // Writer-only reads (SWMR contract C1)
        std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
        std::uint32_t count = buf ? obj_buf_count(buf, std::memory_order_relaxed) : 0;
        std::uint32_t cap   = buf ? obj_buf_capacity(buf) : 0;

        if (!buf || count >= cap)
        {
            // GROW (or first allocation). Build new buffer privately, then publish.
            std::uint32_t new_cap = (cap == 0) ? 4 : cap * 2;
            std::size_t new_size = OBJ_BUF_HEADER + new_cap * ENTRY_SIZE;
            auto* new_buf = static_cast<std::int8_t*>(std::malloc(new_size));
            if (!new_buf) return nullptr;
            std::memset(new_buf, 0, new_size);

            // Header (private — buffer not yet published)
            obj_buf_set_count(new_buf, count, std::memory_order_relaxed);
            obj_buf_set_capacity(new_buf, new_cap);

            // Copy existing entries into new buffer (private)
            if (buf && count > 0)
            {
                std::memcpy(obj_buf_entries(new_buf), obj_buf_entries(buf), count * ENTRY_SIZE);
            }

            // SINGLE COMMIT POINT for grow: publish new buffer with release.
            // After this, any reader's acquire_load(entries_ptr) sees the new buf
            // which contains old count + old entries — a fully consistent snapshot.
            // NOTE: old buffer NOT freed (bump allocator policy, see G2).
            atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);

            buf = new_buf;
        }

        // Return pointer to where the new entry will be written.
        // Caller fills entry, then calls object_commit_add to publish count+1.
        return obj_buf_entries(buf) + count * ENTRY_SIZE;
    }

    // Commit point for add: bump count in current buffer header with release.
    // Pairs with reader's acquire_load(count from buf). The release ensures
    // that the entry data the caller just wrote is visible to any reader
    // that observes the new count.
    void object_commit_add(std::int8_t* slot)
    {
        std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed); // writer-only
        if (!buf) return; // unreachable if object_add_entry succeeded
        std::uint32_t cur = obj_buf_count(buf, std::memory_order_relaxed);
        obj_buf_set_count(buf, cur + 1, std::memory_order_release);
    }

    void object_deallocate_entry_value(std::int8_t* e)
    {
        std::int8_t* val = entry_value_ptr(e);
        if (!val) return;

        std::uint16_t type = entry_type_id(e);
        if (type == type_string)
        {
            // Free string's data buffer
            std::int8_t* str_data;
            std::memcpy(&str_data, val + 8, sizeof(str_data));
            if (str_data) std::free(str_data);
        }
        else if (type == type_object)
        {
            // Recursive: deallocate nested object entries.
            // The nested object's slot is `val` (16 bytes post-shrink).
            // Its entries buffer (with [count|cap|entries...] layout) is at val+8.
            // Post hash-names (REMARKS #3): no per-entry name buffers to free —
            // names are inline 8-byte hashes inside each entry.
            std::int8_t* nested_buf;
            std::memcpy(&nested_buf, val + 8, sizeof(nested_buf));
            if (nested_buf)
            {
                std::uint32_t nested_count = obj_buf_count(nested_buf, std::memory_order_relaxed);
                std::int8_t* nested_entries = obj_buf_entries(nested_buf);
                for (std::uint32_t i = 0; i < nested_count; ++i)
                {
                    std::int8_t* ne = nested_entries + i * ENTRY_SIZE;
                    object_deallocate_entry_value(ne);
                }
                std::free(nested_buf);
            }
        }

        std::free(val);
    }
}

// --- Object set functions ---

EXPORT int PROJECT_SHARED_CCA msm_object_set_int32(std::int8_t* slot, const char* name, std::int32_t value)
{
    if (!slot || !name) return -1;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));

    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (e)
    {
        // Exists — check type
        if (entry_type_id(e) != type_int32)
        {
            std::fprintf(stderr, "[msm_object] FATAL: type mismatch for '%s'\n", name);
            std::abort();
        }
        std::int8_t* val = entry_value_ptr(e);
        std::memcpy(val + 4, &value, 4);
        return 0;
    }

    // New entry: reserve slot, fill data, then commit (publish new count).
    e = object_add_entry(slot);
    if (!e) return -1;

    // Hash-name layout (REMARKS #3): store 8-byte hash, no malloc for name.
    entry_set_name_hash(e, hash_name(name, nlen));
    entry_set_type_id(e, type_int32);
    entry_set_size(e, 8);

    auto* val = static_cast<std::int8_t*>(std::malloc(8));
    std::memset(val, 0, 8);
    std::memcpy(val + 4, &value, 4);
    entry_set_value_ptr(e, val);

    object_commit_add(slot); // commit: count+1 in buf header with release
    return 0;
}

EXPORT int PROJECT_SHARED_CCA msm_object_set_int64(std::int8_t* slot, const char* name, std::int64_t value)
{
    if (!slot || !name) return -1;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));

    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (e)
    {
        if (entry_type_id(e) != type_int64)
        {
            std::fprintf(stderr, "[msm_object] FATAL: type mismatch for '%s'\n", name);
            std::abort();
        }
        std::int8_t* val = entry_value_ptr(e);
        std::memcpy(val + 8, &value, 8);
        return 0;
    }

    e = object_add_entry(slot);
    if (!e) return -1;

    // Hash-name layout (REMARKS #3): store 8-byte hash, no malloc for name.
    entry_set_name_hash(e, hash_name(name, nlen));
    entry_set_type_id(e, type_int64);
    entry_set_size(e, 16);

    auto* val = static_cast<std::int8_t*>(std::malloc(16));
    std::memset(val, 0, 16);
    std::memcpy(val + 8, &value, 8);
    entry_set_value_ptr(e, val);

    object_commit_add(slot);
    return 0;
}

EXPORT int PROJECT_SHARED_CCA msm_object_set_float64(std::int8_t* slot, const char* name, double value)
{
    if (!slot || !name) return -1;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));

    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (e)
    {
        if (entry_type_id(e) != type_float64)
        {
            std::fprintf(stderr, "[msm_object] FATAL: type mismatch for '%s'\n", name);
            std::abort();
        }
        std::int8_t* val = entry_value_ptr(e);
        std::memcpy(val + 8, &value, 8);
        return 0;
    }

    e = object_add_entry(slot);
    if (!e) return -1;

    // Hash-name layout (REMARKS #3): store 8-byte hash, no malloc for name.
    entry_set_name_hash(e, hash_name(name, nlen));
    entry_set_type_id(e, type_float64);
    entry_set_size(e, 16);

    auto* val = static_cast<std::int8_t*>(std::malloc(16));
    std::memset(val, 0, 16);
    std::memcpy(val + 8, &value, 8);
    entry_set_value_ptr(e, val);

    object_commit_add(slot);
    return 0;
}

EXPORT int PROJECT_SHARED_CCA msm_object_set_bool(std::int8_t* slot, const char* name, std::int32_t value)
{
    if (!slot || !name) return -1;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));

    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (e)
    {
        if (entry_type_id(e) != type_bool)
        {
            std::fprintf(stderr, "[msm_object] FATAL: type mismatch for '%s'\n", name);
            std::abort();
        }
        std::int8_t* val = entry_value_ptr(e);
        std::int32_t v = value ? 1 : 0;
        std::memcpy(val + 4, &v, 4);
        return 0;
    }

    e = object_add_entry(slot);
    if (!e) return -1;

    // Hash-name layout (REMARKS #3): store 8-byte hash, no malloc for name.
    entry_set_name_hash(e, hash_name(name, nlen));
    entry_set_type_id(e, type_bool);
    entry_set_size(e, 8);

    auto* val = static_cast<std::int8_t*>(std::malloc(8));
    std::memset(val, 0, 8);
    std::int32_t v = value ? 1 : 0;
    std::memcpy(val + 4, &v, 4);
    entry_set_value_ptr(e, val);

    object_commit_add(slot);
    return 0;
}

EXPORT int PROJECT_SHARED_CCA msm_object_set_string(std::int8_t* slot, const char* name, const char* str, std::uint32_t len)
{
    if (!slot || !name) return -1;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));

    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (e)
    {
        if (entry_type_id(e) != type_string)
        {
            std::fprintf(stderr, "[msm_object] FATAL: type mismatch for '%s'\n", name);
            std::abort();
        }
        // Reuse existing 24B string slot
        std::int8_t* val = entry_value_ptr(e);
        msm_string_set(val, str, len);
        return 0;
    }

    e = object_add_entry(slot);
    if (!e) return -1;

    // Hash-name layout (REMARKS #3): store 8-byte hash, no malloc for name.
    entry_set_name_hash(e, hash_name(name, nlen));
    entry_set_type_id(e, type_string);
    entry_set_size(e, 24);

    auto* val = static_cast<std::int8_t*>(std::malloc(24));
    std::memset(val, 0, 24);
    msm_string_set(val, str, len);
    entry_set_value_ptr(e, val);

    object_commit_add(slot);
    return 0;
}

// --- Object get functions ---

EXPORT std::int32_t PROJECT_SHARED_CCA msm_object_get_int32(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return 0;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e || entry_type_id(e) != type_int32) return 0;
    std::int32_t v;
    std::memcpy(&v, entry_value_ptr(e) + 4, 4);
    return v;
}

EXPORT std::int64_t PROJECT_SHARED_CCA msm_object_get_int64(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return 0;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e || entry_type_id(e) != type_int64) return 0;
    std::int64_t v;
    std::memcpy(&v, entry_value_ptr(e) + 8, 8);
    return v;
}

EXPORT double PROJECT_SHARED_CCA msm_object_get_float64(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return 0.0;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e || entry_type_id(e) != type_float64) return 0.0;
    double v;
    std::memcpy(&v, entry_value_ptr(e) + 8, 8);
    return v;
}

EXPORT std::int32_t PROJECT_SHARED_CCA msm_object_get_bool(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return 0;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e || entry_type_id(e) != type_bool) return 0;
    std::int32_t v;
    std::memcpy(&v, entry_value_ptr(e) + 4, 4);
    return v;
}

EXPORT const char* PROJECT_SHARED_CCA msm_object_get_string(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return "";
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e || entry_type_id(e) != type_string) return "";
    return msm_string_get(entry_value_ptr(e));
}

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_object_get_slot(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return nullptr;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e) return nullptr;
    return entry_value_ptr(e);
}

// --- Object remove / query ---

// Copy-on-Write remove. SINGLE commit point = release_store(entries_ptr).
//
// Correctness (variant B — count inside buffer header):
//   1. Allocate new_buf, write count_new INTO header (private).
//   2. Copy filtered entries (private — buf not yet published).
//   3. release_store(slot+8, new_buf) — single atomic publication.
//   4. Reader's acquire_load(slot+8) sees either OLD buf (with OLD count
//      embedded in its header) or NEW buf (with NEW count embedded). The
//      (ptr, count) pair is physically inseparable — no mixed observation
//      like (OLD ptr, NEW count) or (NEW ptr, OLD count) is possible
//      because count IS part of the buffer.
//   5. Old buf NOT freed (bump allocator, G2). Concurrent readers iterating
//      over the old buf continue to see a consistent pre-removal snapshot.
EXPORT int PROJECT_SHARED_CCA msm_object_remove(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return -1;
    std::uint64_t h = hash_name(name, std::strlen(name));

    // Writer-only reads (SWMR contract C1)
    std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
    if (!buf) return -1;
    std::uint32_t count = obj_buf_count(buf, std::memory_order_relaxed);
    if (count == 0) return -1;

    std::int8_t* entries = obj_buf_entries(buf);

    // Find the entry to remove by hash compare (single 64-bit cmp per entry).
    std::uint32_t remove_idx = count; // sentinel
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::int8_t* e = entries + i * ENTRY_SIZE;
        if (entry_name_hash(e) == h)
        {
            remove_idx = i;
            break;
        }
    }
    if (remove_idx == count) return -1;

    // NOTE: we do NOT free the removed entry's value here.
    // Concurrent readers may still be iterating over the OLD buffer (which we
    // do not free per bump-allocator policy, G2). The value pointer would be
    // dangling if freed; under stress this manifests as heap corruption.
    //
    // The removed entry's value buffer leaks until segment teardown. After
    // the name-hash refactor (REMARKS #3), there is NO name buffer to leak
    // — names are inline 8-byte hashes, requiring no separate allocation.

    std::uint32_t new_count = count - 1;

    if (new_count == 0)
    {
        // Object becomes empty. SINGLE commit: publish null entries_ptr.
        // Reader's acquire_load(slot+8) sees null → returns 0 fields.
        atomic_store_ptr(slot + 8, nullptr, std::memory_order_release);
    }
    else
    {
        // CoW: allocate new buffer, write count INTO header, copy filtered entries,
        // then publish ptr with release as the single commit point.
        //
        // Capacity policy: TIGHT FIT (new_cap = new_count). Each remove allocates
        // exactly the bytes needed, so the leak per remove is bounded by the
        // current object size (1× old buf), not by old_cap. For large-object
        // remove-heavy workloads (e.g., batch-remove from N=5000), tight-fit
        // gives O(N²/2) total leak instead of O(K×N) with keep-capacity policy.
        // The trade-off is that the next add() after remove triggers a grow,
        // adding ~50% overhead for small objects with frequent add/remove cycles.
        std::uint32_t new_cap = new_count;
        std::size_t new_size = OBJ_BUF_HEADER + new_cap * ENTRY_SIZE;
        auto* new_buf = static_cast<std::int8_t*>(std::malloc(new_size));
        if (!new_buf) return -1;

        // Header (private — buffer not yet published)
        obj_buf_set_count(new_buf, new_count, std::memory_order_relaxed);
        obj_buf_set_capacity(new_buf, new_cap);

        // Copy entries [0..remove_idx) and (remove_idx+1..count) into new buffer (private)
        std::int8_t* new_entries = obj_buf_entries(new_buf);
        std::size_t before_bytes = remove_idx * ENTRY_SIZE;
        if (before_bytes > 0)
        {
            std::memcpy(new_entries, entries, before_bytes);
        }

        std::size_t after_count = count - remove_idx - 1;
        if (after_count > 0)
        {
            std::memcpy(new_entries + before_bytes,
                        entries + (remove_idx + 1) * ENTRY_SIZE,
                        after_count * ENTRY_SIZE);
        }

        // SINGLE COMMIT POINT: publish new buffer with release.
        // Old buffer NOT freed (bump allocator policy).
        atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);
    }

    // Intentionally NOT freeing removed entry's name/value here — see comment
    // above. Bump-allocator policy applies to all object-related allocations.

    return 0;
}

EXPORT std::uint32_t PROJECT_SHARED_CCA msm_object_field_count(std::int8_t* slot)
{
    if (!slot) return 0;
    // Reader path: acquire_load(entries_ptr) → acquire_load(count from buf header).
    // Consistent (ptr, count) snapshot via the buffer-embedded count layout.
    std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire
    if (!buf) return 0;
    return obj_buf_count(buf); // acquire
}

EXPORT int PROJECT_SHARED_CCA msm_object_has_field(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return 0;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    return object_find_entry(slot, name, nlen) ? 1 : 0;
}

EXPORT std::uint16_t PROJECT_SHARED_CCA msm_object_get_type(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return 0;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));
    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (!e) return 0;
    return entry_type_id(e);
}

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_object_ensure_object(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return nullptr;
    std::uint16_t nlen = static_cast<std::uint16_t>(std::strlen(name));

    std::int8_t* e = object_find_entry(slot, name, nlen);
    if (e)
    {
        if (entry_type_id(e) != type_object)
        {
            std::fprintf(stderr, "[msm_object] FATAL: type mismatch for '%s' (expected OBJECT)\n", name);
            std::abort();
        }
        return entry_value_ptr(e);
    }

    // Create new nested object entry
    e = object_add_entry(slot);
    if (!e) return nullptr;

    // Hash-name layout (REMARKS #3): store 8-byte hash, no malloc for name.
    entry_set_name_hash(e, hash_name(name, nlen));
    entry_set_type_id(e, type_object);
    entry_set_size(e, 24);

    auto* val = static_cast<std::int8_t*>(std::malloc(24));
    std::memset(val, 0, 24);
    entry_set_value_ptr(e, val);

    object_commit_add(slot);
    return val;
}

} // extern "C"
