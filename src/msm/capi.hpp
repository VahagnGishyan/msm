#pragma once

#include <cstdint>
#include "sharelib.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Array operations (for C# / FFI interop)
// ============================================================================

/// Push int32 into array at given slot pointer.
EXPORT void PROJECT_SHARED_CCA msm_array_push_int32(std::int8_t* array_slot, std::int32_t value);

/// Get int32 from array at index.
EXPORT std::int32_t PROJECT_SHARED_CCA msm_array_get_int32(std::int8_t* array_slot, std::uint32_t index);

// ============================================================================
// Scalar read/write (for C# / FFI interop)
// ============================================================================

/// Write int32 at slot (payload offset +4).
EXPORT void PROJECT_SHARED_CCA msm_write_int32(std::int8_t* slot_ptr, std::int32_t value);

/// Read int32 from slot (payload offset +4).
EXPORT std::int32_t PROJECT_SHARED_CCA msm_read_int32(std::int8_t* slot_ptr);

#ifdef __cplusplus
}
#endif
