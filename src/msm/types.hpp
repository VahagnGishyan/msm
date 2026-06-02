#pragma once

#include <cstdint>
#include <cstring>

namespace msm
{
    /// MSM type wrappers — thin typed views over raw shared-memory bytes. Each
    /// knows its slot_size and exposes operator= / operator T().
    ///
    /// Primitive slot (8 bytes): [is_const:1 | type:1 | reserved:2 | payload:4],
    /// value in payload (bytes 4..7). Scalar slot (16 bytes): [header:8 | value:8],
    /// value in bytes 8..15.

    struct boolean
    {
        using native_type = bool;
        static constexpr std::size_t slot_size = 8;
        std::int8_t* ptr_;

        static void deallocate(std::int8_t*) {} // no nested allocations

        void operator=(bool v)
        {
            std::int32_t val = v ? 1 : 0;
            std::memcpy(ptr_ + 4, &val, 4);
        }

        operator bool() const
        {
            std::int32_t val;
            std::memcpy(&val, ptr_ + 4, 4);
            return val != 0;
        }
    };

    struct int32
    {
        using native_type = std::int32_t;
        static constexpr std::size_t slot_size = 8;
        std::int8_t* ptr_;

        static void deallocate(std::int8_t*) {}

        void operator=(std::int32_t v)
        {
            std::memcpy(ptr_ + 4, &v, 4);
        }

        operator std::int32_t() const
        {
            std::int32_t val;
            std::memcpy(&val, ptr_ + 4, 4);
            return val;
        }
    };

    struct float32
    {
        using native_type = float;
        static constexpr std::size_t slot_size = 8;
        std::int8_t* ptr_;

        static void deallocate(std::int8_t*) {}

        void operator=(float v)
        {
            std::memcpy(ptr_ + 4, &v, 4);
        }

        operator float() const
        {
            float val;
            std::memcpy(&val, ptr_ + 4, 4);
            return val;
        }
    };

    struct int64
    {
        using native_type = std::int64_t;
        static constexpr std::size_t slot_size = 16;
        std::int8_t* ptr_;

        static void deallocate(std::int8_t*) {}

        void operator=(std::int64_t v)
        {
            std::memcpy(ptr_ + 8, &v, 8);
        }

        operator std::int64_t() const
        {
            std::int64_t val;
            std::memcpy(&val, ptr_ + 8, 8);
            return val;
        }
    };

    struct float64
    {
        using native_type = double;
        static constexpr std::size_t slot_size = 16;
        std::int8_t* ptr_;

        static void deallocate(std::int8_t*) {}

        void operator=(double v)
        {
            std::memcpy(ptr_ + 8, &v, 8);
        }

        operator double() const
        {
            double val;
            std::memcpy(&val, ptr_ + 8, 8);
            return val;
        }
    };

} // namespace msm
