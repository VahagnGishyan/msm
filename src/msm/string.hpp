#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════════
// C API declarations (implemented in msm-allocator DLL).
// Single source of truth for string operations.
// Both msm::string (C++) and MsmString (C#) delegate to these.
// ═══════════════════════════════════════════════════════════════════════════════

extern "C"
{
    void         msm_string_set(std::int8_t* slot, const char* str, std::uint32_t len);
    const char*  msm_string_get(std::int8_t* slot);
    std::uint32_t msm_string_length(std::int8_t* slot);
    void         msm_string_clear(std::int8_t* slot);
}

namespace msm
{
    //////////////////////////////////////////////////////////////////////
    /// string — UTF-8 string stored in shared memory, delegates to C API.
    ///
    /// Slot layout (16 bytes, stored inline in parent record):
    ///   [header: 8B] [data_ptr: 8B]
    ///
    /// Variant B (since 2026-05-26): length and capacity are stored INSIDE
    /// the buffer pointed to by data_ptr, not in the slot. Buffer layout:
    ///   [len: 4B | cap: 4B | data: cap bytes (null-terminated)]
    ///
    /// All mutation (set, clear) goes through msm_string_* C API in
    /// msm-allocator DLL — single implementation shared by all language
    /// bindings (C++, C#, etc.). The 8 bytes formerly used for len+cap in
    /// the slot are now reclaimed (reduced slot from 24 to 16 bytes).
    //////////////////////////////////////////////////////////////////////

    struct string
    {
        using native_type = string;
        static constexpr std::size_t slot_size = 16; // header(8) + data_ptr(8). len+cap moved to buffer header.

        std::int8_t* ptr_; // points to the 24-byte slot in parent record/segment

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
