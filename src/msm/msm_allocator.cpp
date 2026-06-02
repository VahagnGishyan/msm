#include "msm_allocator.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <string>

// Atomic access to fields embedded in raw byte slots, using std::atomic_ref
// (C++20) so the slots don't need std::atomic<T> layout. Commit-point fields
// (size, length, count, data_ptr) are written with release and read with
// acquire; writer-private fields (capacity) stay relaxed.
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

// Contract checks. MSM_REQUIRE fires in both debug and release builds (it
// guards critical invariants); MSM_DEBUG_ASSERT is debug-only for hot paths.

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
        // Schema mismatch (contract C2): silent layout drift is the worst
        // failure mode for cross-language shared memory, so fail fast.
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

// Array C API.
//
//   slot:   [header:8 | data_ptr:8]          (16 bytes)
//   buffer: [size:4 | cap:4 | elements...]   (separate heap allocation)
//
// Keeping size and cap in the buffer header makes the (ptr, size) snapshot
// inseparable: grow/CoW publishes a consistent state with a single release
// store on data_ptr, and push_back bumps size in place with a release store
// that the reader's acquire load pairs with.

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

    // Writer-only reads (contract C1)
    std::int8_t* old_buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
    std::uint32_t old_cap = old_buf ? arr_buf_cap(old_buf) : 0;
    if (new_cap <= old_cap) return;
    std::uint32_t old_size = old_buf ? arr_buf_size(old_buf, std::memory_order_relaxed) : 0;

    std::size_t new_bytes = ARR_BUF_HEADER + static_cast<std::size_t>(new_cap) * item_size;
    auto* new_buf = static_cast<std::int8_t*>(std::malloc(new_bytes));
    if (!new_buf) return;
    std::memset(new_buf, 0, new_bytes);

    // Fill the new buffer privately, before it is reachable by readers.
    arr_buf_set_size(new_buf, old_size, std::memory_order_relaxed);
    arr_buf_set_cap(new_buf, new_cap);
    if (old_buf && old_size > 0)
    {
        std::memcpy(arr_buf_data(new_buf), arr_buf_data(old_buf),
                    static_cast<std::size_t>(old_size) * item_size);
    }

    // Commit: publish with a release store. Old buffer kept (G2).
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

// Publish size+1 with a release store on the buffer's size field; the reader's
// acquire load of that field (after acquiring the buffer) pairs with it.
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

    // Acquire-load the buffer pointer (pairs with the writer's release store,
    // exposing the whole buffer), then read size from its header.
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

// String C API. msm::string (C++) and the C# binding both delegate here.
//
//   slot:   [header:8 | data_ptr:8]   (only data_ptr is read/written)
//   buffer: [len:4 | cap:4 | data: cap bytes, NUL-terminated]
//
// len excludes the NUL; cap is the data-area size (len+1 for a tight CoW fit).
// Embedding len in the buffer gives one commit point (contract G1): every set
// is a single release store of data_ptr, so the (ptr, len) pair is inseparable.
// Storing them separately would let a reader pair an old len with a new buffer
// — harmless while growing, an out-of-bounds read while shrinking.
// Old buffers are not freed (bump-allocator policy, contract G2).

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
        // Empty: publish nullptr; readers map null to "". Old buffer kept (G2).
        atomic_store_ptr(slot + 8, nullptr, std::memory_order_release);
        return;
    }

    // CoW: build the new buffer privately (header + data), then publish it with
    // a single release store. A reader that acquires the new pointer sees a
    // consistent (len, data) pair, since both live in the same buffer.
    std::uint32_t new_cap = len + 1; // tight fit, includes null terminator
    std::size_t buf_size = STR_BUF_HEADER + new_cap;
    auto* new_buf = static_cast<std::int8_t*>(std::malloc(buf_size));
    if (!new_buf) return;

    str_buf_set_len(new_buf, len, std::memory_order_relaxed);
    str_buf_set_cap(new_buf, new_cap);

    std::int8_t* data = str_buf_data(new_buf);
    std::memcpy(data, str, len);
    data[len] = '\0';

    // Commit: publish with a release store. Old buffer kept (G2).
    atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);
}

EXPORT const char* PROJECT_SHARED_CCA msm_string_get(std::int8_t* slot)
{
    if (!slot) return "";

    // Acquire-load the buffer (pairs with the writer's release store), then read
    // len from its header — no second atomic needed.
    std::int8_t* buf = atomic_load_ptr(slot + 8); // acquire
    if (!buf) return "";

    std::uint32_t len = str_buf_len(buf); // acquire (relaxed would suffice)
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

// Record C API: field access by pointer + offset. Trivial, but it gives every
// language binding a uniform C ABI.

EXPORT std::int8_t* PROJECT_SHARED_CCA msm_record_get_field(std::int8_t* record_ptr, std::uint32_t offset)
{
    if (!record_ptr) return nullptr;
    return record_ptr + offset;
}

// Object C API: a dynamic property bag.
//
//   slot:   [header:8 | entries_ptr:8]                        (16 bytes)
//   buffer: [count:4 | capacity:4 | entry[0] | entry[1] ...]  (separate alloc)
//   entry:  [name_hash:8 | type_id:2 | unused:2 | size:4 | value_ptr:8]  (24B)
//
// Fields are keyed by the FNV-1a 64-bit hash of their name rather than by the
// name itself: lookup is a single 64-bit compare instead of memcmp plus a
// pointer chase, entries are self-contained (better locality), and there is no
// per-entry name buffer to allocate or leak. The cost is that a field name
// can't be recovered from a raw memory dump — callers that need that keep their
// own name table. Collisions are a ~2^-64 risk for realistic vocabularies.
//
// count lives in the buffer header, so every CoW (grow, remove) publishes a
// consistent (ptr, count) pair with one release store on entries_ptr (contract
// G1); a reader never pairs a new count with an old buffer or vice versa. An
// in-place add bumps count with a release store the reader's acquire load
// pairs with.

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

    // --- entries buffer accessors ---

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

    // --- entry field accessors (layout as above) ---
    inline std::uint64_t entry_name_hash(std::int8_t* e) { std::uint64_t v; std::memcpy(&v, e, 8); return v; }
    inline std::uint16_t entry_type_id(std::int8_t* e) { std::uint16_t v; std::memcpy(&v, e + 8, 2); return v; }
    inline std::uint32_t entry_size(std::int8_t* e) { std::uint32_t v; std::memcpy(&v, e + 12, 4); return v; }
    inline std::int8_t* entry_value_ptr(std::int8_t* e) { std::int8_t* p; std::memcpy(&p, e + 16, 8); return p; }

    inline void entry_set_name_hash(std::int8_t* e, std::uint64_t v) { std::memcpy(e, &v, 8); }
    inline void entry_set_type_id(std::int8_t* e, std::uint16_t v) { std::memcpy(e + 8, &v, 2); }
    inline void entry_set_size(std::int8_t* e, std::uint32_t v) { std::memcpy(e + 12, &v, 4); }
    inline void entry_set_value_ptr(std::int8_t* e, std::int8_t* p) { std::memcpy(e + 16, &p, 8); }

    // FNV-1a 64-bit over [name, name+len); inlined here for the hot path.
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

    // Look up an entry by name hash. Acquire-load the buffer, then its count,
    // then scan; count lives in the buffer, so the (ptr, count) view is consistent.
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
        // Writer-only reads (contract C1)
        std::int8_t* buf = atomic_load_ptr(slot + 8, std::memory_order_relaxed);
        std::uint32_t count = buf ? obj_buf_count(buf, std::memory_order_relaxed) : 0;
        std::uint32_t cap   = buf ? obj_buf_capacity(buf) : 0;

        if (!buf || count >= cap)
        {
            // Grow (or first allocation): build a bigger buffer privately, then publish.
            std::uint32_t new_cap = (cap == 0) ? 4 : cap * 2;
            std::size_t new_size = OBJ_BUF_HEADER + new_cap * ENTRY_SIZE;
            auto* new_buf = static_cast<std::int8_t*>(std::malloc(new_size));
            if (!new_buf) return nullptr;
            std::memset(new_buf, 0, new_size);

            obj_buf_set_count(new_buf, count, std::memory_order_relaxed);
            obj_buf_set_capacity(new_buf, new_cap);

            if (buf && count > 0)
            {
                std::memcpy(obj_buf_entries(new_buf), obj_buf_entries(buf), count * ENTRY_SIZE);
            }

            // Commit the grow with a release store. Old buffer kept (G2).
            atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);

            buf = new_buf;
        }

        // Hand back the slot for the new entry; the caller fills it, then commits.
        return obj_buf_entries(buf) + count * ENTRY_SIZE;
    }

    // Commit an add: bump count with a release store, which makes the entry data
    // the caller just wrote visible to any reader that sees the new count.
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
            // Recurse into a nested object: its entries buffer is at val+8.
            // Names are inline hashes, so there are no name buffers to free.
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

// Copy-on-Write remove: build a filtered buffer privately, then publish it with
// one release store on entries_ptr. Because count lives in the buffer header, a
// reader's acquire load of entries_ptr yields either the old buffer (old count)
// or the new one (new count) — the (ptr, count) pair can't be torn. The old
// buffer is kept (G2), so readers still mid-scan see a consistent pre-removal
// snapshot.
EXPORT int PROJECT_SHARED_CCA msm_object_remove(std::int8_t* slot, const char* name)
{
    if (!slot || !name) return -1;
    std::uint64_t h = hash_name(name, std::strlen(name));

    // Writer-only reads (contract C1)
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

    // The removed entry's value buffer is intentionally not freed: readers may
    // still be scanning the old buffer (kept per G2), and freeing would leave
    // them with a dangling value pointer. It leaks until segment teardown. The
    // name needs no cleanup — it's an inline 8-byte hash, not a separate alloc.

    std::uint32_t new_count = count - 1;

    if (new_count == 0)
    {
        // Object becomes empty: publish a null entries_ptr. A reader's acquire
        // load sees null and reports 0 fields.
        atomic_store_ptr(slot + 8, nullptr, std::memory_order_release);
    }
    else
    {
        // Allocate a tightly-sized buffer (capacity == new_count), copy the
        // surviving entries into it privately, then publish below. Tight-fit
        // keeps the per-remove leak proportional to the current object size
        // rather than to the old capacity; the cost is that the next add() has
        // to grow again.
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

        // Commit: publish the new buffer with release. Old buffer kept (G2).
        atomic_store_ptr(slot + 8, new_buf, std::memory_order_release);
    }

    return 0;
}

EXPORT std::uint32_t PROJECT_SHARED_CCA msm_object_field_count(std::int8_t* slot)
{
    if (!slot) return 0;
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
