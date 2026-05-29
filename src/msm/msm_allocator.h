#ifndef MSM_ALLOCATOR_H
#define MSM_ALLOCATOR_H

#include <cstdint>
#include "sharelib.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/// Destructor callback type. Called by msm_free before freeing the root block.
/// Use to clean up nested allocations (array data blocks, etc.).
typedef void (*msm_destructor_fn)(std::int8_t* ptr);

/// Allocate a named memory region of given size (bytes).
/// Returns pointer to allocated memory, or nullptr on failure (out of memory).
/// Terminates if name already exists (programmer error).
/// destructor: optional callback invoked by msm_free before freeing. Pass NULL if not needed.
/// Thread-safe. noexcept.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_alloc(const char* name, std::uint64_t size, msm_destructor_fn destructor);

/// Get pointer to an existing named memory region.
/// Returns pointer, or nullptr if name not found.
/// Thread-safe. noexcept.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_get(const char* name);

/// Free a named memory region.
/// Calls the destructor (if registered) before freeing.
/// Does nothing if name not found.
/// Thread-safe. noexcept.
EXPORT void PROJECT_SHARED_CCA msm_free(const char* name);

// ═══════════════════════════════════════════════════════════════════════════════
// Schema hash — contract C2 enforcement (silent cross-language corruption guard).
//
// Two parties (e.g., C++ writer and C# reader) attaching to a named segment
// MUST agree on the byte layout of stored data. The schema_hash is a writer-
// declared 64-bit fingerprint of the agreed schema (FNV-1a is provided as a
// helper but any 64-bit hash works as long as both sides compute identically).
//
// Usage:
//   uint64_t h = msm_schema_hash_fnv1a("Graph{nodes:int32, edges:array<Edge>}");
//   ptr = msm_alloc_with_schema("graph", size, h, dtor);
//   // later (possibly different process / language):
//   ptr = msm_get_with_schema("graph", h);  // aborts if hash mismatch
//
// Mismatch handling: msm_get_with_schema calls std::abort() with a fprintf
// diagnostic showing both hashes. This is a fail-fast contract violation
// detection, NOT recoverable error handling — silent layout drift is the
// single most dangerous failure mode for cross-language shared memory.
// ═══════════════════════════════════════════════════════════════════════════════

/// FNV-1a 64-bit hash of a null-terminated string. Recommended helper for
/// computing schema_hash arguments, but any 64-bit hash works as long as
/// both producer and consumer use the same function.
EXPORT std::uint64_t PROJECT_SHARED_CCA msm_schema_hash_fnv1a(const char* schema_str);

/// Allocate a named region with associated schema hash. Identical to
/// msm_alloc otherwise. The hash is stored in the in-process registry and
/// can be queried later via msm_get_schema_hash or verified at attach time
/// via msm_get_with_schema.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_alloc_with_schema(
    const char* name, std::uint64_t size, std::uint64_t schema_hash, msm_destructor_fn destructor);

/// Get pointer to existing named region, verifying that its stored schema
/// hash matches the supplied schema_hash. If mismatch — std::abort() with
/// diagnostic. If name not found — returns nullptr. If region was allocated
/// without a schema (schema_hash=0 implicit), passing schema_hash=0 succeeds
/// while any non-zero value aborts.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_get_with_schema(const char* name, std::uint64_t schema_hash);

/// Returns the stored schema hash for a named region, or 0 if not found
/// or no schema was attached. For inspection / debugging.
EXPORT std::uint64_t PROJECT_SHARED_CCA msm_get_schema_hash(const char* name);

// ═══════════════════════════════════════════════════════════════════════════════
// Array C API — operates on a 24-byte array slot: [header:8][data_ptr:8][size:4][capacity:4]
// item_size = size of each element in bytes (e.g., 8 for int32, 16 for float64)
// These functions implement the same logic as msm::array<T> in C++ but via C ABI.
// ═══════════════════════════════════════════════════════════════════════════════

/// Get current element count.
EXPORT std::uint32_t PROJECT_SHARED_CCA msm_array_size(std::int8_t* slot);

/// Get current capacity.
EXPORT std::uint32_t PROJECT_SHARED_CCA msm_array_capacity(std::int8_t* slot);

/// Reserve capacity. Allocates new block, copies existing data, frees old.
/// Does nothing if new_cap <= current capacity.
EXPORT void PROJECT_SHARED_CCA msm_array_reserve(std::int8_t* slot, std::uint32_t new_cap, std::uint32_t item_size);

/// Push back: ensures capacity, returns pointer to the new slot (zero-initialized).
/// Increments size BEFORE returning — convenience for single-threaded use only.
/// For SWMR-safe pushes, use msm_array_prepare_push + msm_array_commit_push.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_array_push_back(std::int8_t* slot, std::uint32_t item_size);

/// SWMR-safe push (step 1 of 2). Reserves next slot without committing size.
/// Returns pointer to zero-initialized slot. Caller must fill the slot then
/// call msm_array_commit_push to publish size.
/// Concurrent readers do not observe the new slot until commit.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_array_prepare_push(std::int8_t* slot, std::uint32_t item_size);

/// SWMR-safe push (step 2 of 2). Publishes size+1 with release ordering.
/// Must be called after msm_array_prepare_push once the slot has been filled.
EXPORT void PROJECT_SHARED_CCA msm_array_commit_push(std::int8_t* slot);

/// Get pointer to element at index. No bounds checking.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_array_get(std::int8_t* slot, std::uint32_t index, std::uint32_t item_size);

/// Clear: set size to 0 (does not free memory).
EXPORT void PROJECT_SHARED_CCA msm_array_clear(std::int8_t* slot);

// ═══════════════════════════════════════════════════════════════════════════════
// String C API — operates on a 24-byte string slot: [header:8][data_ptr:8][length:4][capacity:4]
// UTF-8 encoded, null-terminated. Single implementation shared by C++/C#.
// ═══════════════════════════════════════════════════════════════════════════════

/// Set string value. Copies `len` bytes from `str` into the slot's data buffer.
/// Allocates/grows buffer as needed. Null-terminates the stored data.
EXPORT void PROJECT_SHARED_CCA msm_string_set(std::int8_t* slot, const char* str, std::uint32_t len);

/// Get string value. Returns pointer to null-terminated UTF-8 data.
/// Returns "" (empty string literal) if slot is empty or null.
EXPORT const char* PROJECT_SHARED_CCA msm_string_get(std::int8_t* slot);

/// Get string length in bytes (not including null terminator).
EXPORT std::uint32_t PROJECT_SHARED_CCA msm_string_length(std::int8_t* slot);

/// Clear string: set length to 0 (does not free buffer).
EXPORT void PROJECT_SHARED_CCA msm_string_clear(std::int8_t* slot);

// ═══════════════════════════════════════════════════════════════════════════════
// Record C API — field access via pointer + offset.
// Provides uniform C ABI for all language bindings.
// ═══════════════════════════════════════════════════════════════════════════════

/// Get pointer to a field within a record at the given byte offset.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_record_get_field(std::int8_t* record_ptr, std::uint32_t offset);

// ═══════════════════════════════════════════════════════════════════════════════
// Object C API — dynamic property bag with named fields.
// Slot layout: [header:8][entries_ptr:8][count:4][capacity:4] = 24 bytes
// Entry layout (24B each): [name_ptr:8][name_len:2][type_id:2][size:4][value_ptr:8]
// Values are heap-allocated. Type is immutable once set (mismatch = abort).
// ═══════════════════════════════════════════════════════════════════════════════

/// Set int32 field. Creates or overwrites (same type only).
EXPORT int PROJECT_SHARED_CCA msm_object_set_int32(std::int8_t* slot, const char* name, std::int32_t value);

/// Set int64 field.
EXPORT int PROJECT_SHARED_CCA msm_object_set_int64(std::int8_t* slot, const char* name, std::int64_t value);

/// Set float64 field.
EXPORT int PROJECT_SHARED_CCA msm_object_set_float64(std::int8_t* slot, const char* name, double value);

/// Set bool field.
EXPORT int PROJECT_SHARED_CCA msm_object_set_bool(std::int8_t* slot, const char* name, std::int32_t value);

/// Set string field.
EXPORT int PROJECT_SHARED_CCA msm_object_set_string(std::int8_t* slot, const char* name, const char* str, std::uint32_t len);

/// Get int32 field value. Returns 0 if not found.
EXPORT std::int32_t PROJECT_SHARED_CCA msm_object_get_int32(std::int8_t* slot, const char* name);

/// Get int64 field value. Returns 0 if not found.
EXPORT std::int64_t PROJECT_SHARED_CCA msm_object_get_int64(std::int8_t* slot, const char* name);

/// Get float64 field value. Returns 0.0 if not found.
EXPORT double PROJECT_SHARED_CCA msm_object_get_float64(std::int8_t* slot, const char* name);

/// Get bool field value. Returns 0 if not found.
EXPORT std::int32_t PROJECT_SHARED_CCA msm_object_get_bool(std::int8_t* slot, const char* name);

/// Get string field value. Returns "" if not found.
EXPORT const char* PROJECT_SHARED_CCA msm_object_get_string(std::int8_t* slot, const char* name);

/// Get raw value pointer for a field. Returns nullptr if not found.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_object_get_slot(std::int8_t* slot, const char* name);

/// Remove field by name. Returns 0 on success, -1 if not found.
EXPORT int PROJECT_SHARED_CCA msm_object_remove(std::int8_t* slot, const char* name);

/// Get number of fields.
EXPORT std::uint32_t PROJECT_SHARED_CCA msm_object_field_count(std::int8_t* slot);

/// Check if field exists. Returns 1 if yes, 0 if no.
EXPORT int PROJECT_SHARED_CCA msm_object_has_field(std::int8_t* slot, const char* name);

/// Get type_id of a field. Returns 0 if not found.
EXPORT std::uint16_t PROJECT_SHARED_CCA msm_object_get_type(std::int8_t* slot, const char* name);

/// Ensure a nested object field exists. Creates if not present (type=OBJECT, 24B value).
/// Returns pointer to the nested object's 24-byte slot. Aborts on type mismatch.
EXPORT std::int8_t* PROJECT_SHARED_CCA msm_object_ensure_object(std::int8_t* slot, const char* name);

#ifdef __cplusplus
}
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// MSM Type IDs — used in object entries and slot headers.
// C++ only (enum class). For C interop, cast to uint16_t.
// ═══════════════════════════════════════════════════════════════════════════════

enum class msm_type : std::uint16_t
{
    boolean  = 0x01,
    int32    = 0x02,
    float32  = 0x03,
    int64    = 0x04,
    float64  = 0x05,
    string   = 0x06,
    array    = 0x07,
    object   = 0x08,
    record   = 0x09,
};

#endif // MSM_ALLOCATOR_H
