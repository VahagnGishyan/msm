#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════════════
// C API declarations (implemented in msm-allocator DLL).
// Single source of truth for object operations.
// Both msm::object (C++) and MsmObject (C#) delegate to these.
// ═══════════════════════════════════════════════════════════════════════════════

extern "C"
{
    int           msm_object_set_int32  (std::int8_t* slot, const char* name, std::int32_t value);
    int           msm_object_set_int64  (std::int8_t* slot, const char* name, std::int64_t value);
    int           msm_object_set_float64(std::int8_t* slot, const char* name, double value);
    int           msm_object_set_bool   (std::int8_t* slot, const char* name, std::int32_t value);
    int           msm_object_set_string (std::int8_t* slot, const char* name, const char* str, std::uint32_t len);

    std::int32_t  msm_object_get_int32  (std::int8_t* slot, const char* name);
    std::int64_t  msm_object_get_int64  (std::int8_t* slot, const char* name);
    double        msm_object_get_float64(std::int8_t* slot, const char* name);
    std::int32_t  msm_object_get_bool   (std::int8_t* slot, const char* name);
    const char*   msm_object_get_string (std::int8_t* slot, const char* name);

    std::int8_t*  msm_object_get_slot   (std::int8_t* slot, const char* name);
    std::uint16_t msm_object_get_type   (std::int8_t* slot, const char* name);

    int           msm_object_remove     (std::int8_t* slot, const char* name);
    std::uint32_t msm_object_field_count(std::int8_t* slot);
    int           msm_object_has_field  (std::int8_t* slot, const char* name);
    std::int8_t*  msm_object_ensure_object(std::int8_t* slot, const char* name);
}

namespace msm
{
    struct object;

    //////////////////////////////////////////////////////////////////////
    /// object_proxy — returned by operator[], enables JsonCpp-style syntax.
    /// Lightweight (two pointers), lives on stack only.
    ///
    /// Write: obj["hp"] = 100;        (calls msm_object_set_int32)
    /// Read:  int hp = obj["hp"];     (calls msm_object_get_int32 via conversion)
    /// Type mismatch on overwrite = abort (programmer error).
    //////////////////////////////////////////////////////////////////////

    struct object_proxy
    {
        std::int8_t* obj_ptr_;
        const char*  name_;

        // --- Assignment operators (write) ---

        void operator=(std::int32_t v)   { msm_object_set_int32(obj_ptr_, name_, v); }
        void operator=(std::int64_t v)   { msm_object_set_int64(obj_ptr_, name_, v); }
        void operator=(double v)         { msm_object_set_float64(obj_ptr_, name_, v); }
        void operator=(bool v)           { msm_object_set_bool(obj_ptr_, name_, v ? 1 : 0); }
        void operator=(const char* v)
        {
            std::uint32_t len = 0;
            if (v) { const char* p = v; while (*p) { ++p; ++len; } }
            msm_object_set_string(obj_ptr_, name_, v, len);
        }

        // --- Conversion operators (read) ---

        operator std::int32_t() const { return msm_object_get_int32(obj_ptr_, name_); }
        operator std::int64_t() const { return msm_object_get_int64(obj_ptr_, name_); }
        operator double()       const { return msm_object_get_float64(obj_ptr_, name_); }
        operator bool()         const { return msm_object_get_bool(obj_ptr_, name_) != 0; }
        operator const char*()  const { return msm_object_get_string(obj_ptr_, name_); }

        // --- Explicit getters (when conversion is ambiguous) ---

        std::int32_t as_int32()   const { return msm_object_get_int32(obj_ptr_, name_); }
        std::int64_t as_int64()   const { return msm_object_get_int64(obj_ptr_, name_); }
        double       as_float64() const { return msm_object_get_float64(obj_ptr_, name_); }
        bool         as_bool()    const { return msm_object_get_bool(obj_ptr_, name_) != 0; }
        const char*  as_string()  const { return msm_object_get_string(obj_ptr_, name_); }

        // --- Type query ---

        std::uint16_t type_id() const { return msm_object_get_type(obj_ptr_, name_); }
        bool exists() const { return msm_object_has_field(obj_ptr_, name_) != 0; }

        // --- Nested object access (chaining) ---
        // obj["player"]["pos"]["x"] = 10.0;

        object_proxy operator[](const char* nested_name)
        {
            std::int8_t* nested_slot = msm_object_ensure_object(obj_ptr_, name_);
            return object_proxy{ nested_slot, nested_name };
        }
    };

    //////////////////////////////////////////////////////////////////////
    /// object — dynamic property bag, delegates to C API.
    ///
    /// Slot layout (16 bytes, stored inline in parent record/segment):
    ///   [header: 8B] [entries_ptr: 8B]
    ///
    /// Variant B (since 2026-05-26): count and capacity are stored INSIDE
    /// the entries buffer header, not in the slot. Buffer layout:
    ///   [count: 4B | cap: 4B | entries: count × 24 bytes]
    ///
    /// All mutation goes through msm_object_* C API in msm-allocator DLL.
    /// Slot reduced from 24 to 16 bytes.
    //////////////////////////////////////////////////////////////////////

    struct object
    {
        using native_type = object;
        static constexpr std::size_t slot_size = 16; // header(8) + entries_ptr(8). count+cap moved to buffer header.

        std::int8_t* ptr_;

        /// Deallocate all entries (called by allocator on msm_free).
        /// Variant B layout (since 2026-05-26):
        ///   slot:    [header:8 | entries_ptr:8]
        ///   buf:     [count:4 | cap:4 | entries[count] × 24]
        /// Reads count from buf+0 (not slot+16 — that's no longer used).
        static void deallocate(std::int8_t* slot)
        {
            if (!slot) return;

            std::int8_t* buf;
            std::memcpy(&buf, slot + 8, sizeof(buf));
            if (!buf) return;

            // Read count from buffer header (Variant B layout)
            std::uint32_t count;
            std::memcpy(&count, buf, 4);

            // Entries start AFTER the 8-byte buffer header [count:4 | cap:4]
            std::int8_t* entries = buf + 8;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::int8_t* e = entries + i * 24;

                // Post hash-names (REMARKS #3): no name buffer to free.
                // Entry layout: [name_hash:8 | type_id:2 | unused:2 | size:4 | value_ptr:8]
                std::uint16_t type;
                std::memcpy(&type, e + 8, 2);  // type_id offset changed from +10 to +8
                std::int8_t* val;
                std::memcpy(&val, e + 16, 8);
                if (val)
                {
                    // If string, free its internal buffer (Variant B buffer too)
                    if (type == 0x06) // msm_type::string
                    {
                        std::int8_t* str_data;
                        std::memcpy(&str_data, val + 8, sizeof(str_data));
                        if (str_data) std::free(str_data);
                    }
                    std::free(val);
                }
            }

            // Free the entries buffer itself (single malloc — header + entries together)
            std::free(buf);

            // Clear slot
            std::int8_t* null_ptr = nullptr;
            std::memcpy(slot + 8, &null_ptr, sizeof(null_ptr));
        }

        // --- operator[] — JsonCpp style ---

        object_proxy operator[](const char* name)
        {
            return object_proxy{ ptr_, name };
        }

        const object_proxy operator[](const char* name) const
        {
            return object_proxy{ ptr_, name };
        }

        // --- Remove / Query ---

        bool remove(const char* name)
        {
            return msm_object_remove(ptr_, name) == 0;
        }

        std::uint32_t field_count() const
        {
            return msm_object_field_count(ptr_);
        }

        bool has(const char* name) const
        {
            return msm_object_has_field(ptr_, name) != 0;
        }
    };

} // namespace msm
