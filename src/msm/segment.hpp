#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Allocator C API (from the msm-allocator library)
extern "C"
{
    typedef void (*msm_destructor_fn)(std::int8_t* ptr);
    std::int8_t* msm_alloc(const char* name, std::uint64_t size, msm_destructor_fn destructor);
    std::int8_t* msm_get(const char* name);
    void         msm_free(const char* name);
}

namespace msm
{
    /// segment — typed access to named memory regions owned by the allocator.
    /// create<T> allocates T::slot_size bytes, open<T> retrieves an existing
    /// region, and close frees it. These wrappers throw on failure; the
    /// underlying C API returns null instead.

    struct segment
    {
        /// Create a new named region. Allocates T::slot_size bytes via allocator.
        /// Returns a typed view (T) pointing to the allocated memory.
        /// Throws std::runtime_error if allocation fails.
        template<typename T>
        static T create(const char* name)
        {
            constexpr std::uint64_t size = T::slot_size;

            std::int8_t* base = msm_alloc(name, size, &T::deallocate);
            if (!base)
            {
                throw std::runtime_error(
                    std::string("msm::segment::create failed for '") + name + "'");
            }

            return T{ base };
        }

        /// Get an existing named region. Returns typed view.
        /// Throws std::runtime_error if name not found.
        template<typename T>
        static T open(const char* name)
        {
            std::int8_t* base = msm_get(name);
            if (!base)
            {
                throw std::runtime_error(
                    std::string("msm::segment::open failed for '") + name + "'");
            }

            return T{ base };
        }

        /// Free a named region via allocator.
        static void close(const char* name)
        {
            msm_free(name);
        }

        /// Get raw base pointer (nullptr if not found).
        static std::int8_t* get_base(const char* name)
        {
            return msm_get(name);
        }
    };

} // namespace msm
