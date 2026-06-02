#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// C API declarations (implemented in the msm-allocator library). Both
// msm::string here and the C# MsmString binding delegate to these.

extern "C"
{
    void         msm_string_set(std::int8_t* slot, const char* str, std::uint32_t len);
    const char*  msm_string_get(std::int8_t* slot);
    std::uint32_t msm_string_length(std::int8_t* slot);
    void         msm_string_clear(std::int8_t* slot);
}

namespace msm
{
    /// string — a UTF-8 string in shared memory that delegates to the
    /// msm_string_* C API, so C++ and the other bindings share one path.
    ///
    /// The slot, stored inline in the parent record, is 16 bytes:
    ///   slot:   [header:8 | data_ptr:8]
    /// Length and capacity live in the buffer, not the slot:
    ///   buffer: [len:4 | cap:4 | data: cap bytes, null-terminated]

    struct string
    {
        using native_type = string;
        static constexpr std::size_t slot_size = 16; // header(8) + data_ptr(8); len and cap live in the buffer

        std::int8_t* ptr_; // points to the 16-byte slot in the parent record/segment

        /// Deallocate the data buffer (called by allocator on msm_free).
        static void deallocate(std::int8_t* slot)
        {
            if (!slot) return;

            // Read data_ptr from slot
            std::int8_t* data;
            std::memcpy(&data, slot + 8, sizeof(data));
            if (!data) return;

            std::free(data);

            // Null out pointer to prevent double-free
            std::int8_t* null_ptr = nullptr;
            std::memcpy(slot + 8, &null_ptr, sizeof(null_ptr));
        }

        // --- Public API (delegates to C API) ---

        /// Set string from null-terminated C string.
        void operator=(const char* str)
        {
            std::uint32_t len = str ? static_cast<std::uint32_t>(std::strlen(str)) : 0;
            msm_string_set(ptr_, str, len);
        }

        /// Get null-terminated C string.
        const char* c_str() const
        {
            return msm_string_get(ptr_);
        }

        /// Get string length in bytes.
        std::uint32_t length() const
        {
            return msm_string_length(ptr_);
        }

        /// Check if empty.
        bool empty() const
        {
            return msm_string_length(ptr_) == 0;
        }

        /// Clear string (does not free buffer).
        void clear()
        {
            msm_string_clear(ptr_);
        }

        /// Implicit conversion to const char* for convenience.
        operator const char*() const
        {
            return msm_string_get(ptr_);
        }
    };

} // namespace msm
