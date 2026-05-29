#include "capi.hpp"
#include "types.hpp"
#include "array.hpp"

#include <cstring>

extern "C"
{

// ============================================================================
// Array operations
// ============================================================================

EXPORT void PROJECT_SHARED_CCA msm_array_push_int32(std::int8_t* array_slot, std::int32_t value)
{
    if (!array_slot) return;
    msm::array<msm::int32> arr{ array_slot };
    arr.push_back(value);
}

EXPORT std::int32_t PROJECT_SHARED_CCA msm_array_get_int32(std::int8_t* array_slot, std::uint32_t index)
{
    if (!array_slot) return 0;
    msm::array<msm::int32> arr{ array_slot };
    msm::int32 item = arr[index];
    return static_cast<std::int32_t>(item);
}

// ============================================================================
// Scalar read/write
// ============================================================================

EXPORT void PROJECT_SHARED_CCA msm_write_int32(std::int8_t* slot_ptr, std::int32_t value)
{
    if (!slot_ptr) return;
    std::memcpy(slot_ptr + 4, &value, 4);
}

EXPORT std::int32_t PROJECT_SHARED_CCA msm_read_int32(std::int8_t* slot_ptr)
{
    if (!slot_ptr) return 0;
    std::int32_t value;
    std::memcpy(&value, slot_ptr + 4, 4);
    return value;
}

} // extern "C"
