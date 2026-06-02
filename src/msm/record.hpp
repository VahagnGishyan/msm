#pragma once

#include <cstddef>
#include <cstdint>
#include "fixed_string.hpp"
#include "member.hpp"
#include "types.hpp"

// Record C API (implemented in the msm-allocator library)
extern "C"
{
    std::int8_t* msm_record_get_field(std::int8_t* record_ptr, std::uint32_t offset);
}

namespace msm
{
    namespace detail
    {
        /// Compute offset of field at index I within a parameter pack of named fields.
        template<std::size_t I, typename... Fields>
        struct offset_at;

        template<std::size_t I, typename First, typename... Rest>
        struct offset_at<I, First, Rest...>
        {
            static constexpr std::size_t value =
                (I == 0) ? 0 : First::type::slot_size + offset_at<I - 1, Rest...>::value;
        };

        template<std::size_t I>
        struct offset_at<I>
        {
            static constexpr std::size_t value = 0;
        };

        /// Find index of field with given name in parameter pack.
        template<fixed_string Name, std::size_t I, typename... Fields>
        struct find_field_index;

        template<fixed_string Name, std::size_t I, typename First, typename... Rest>
        struct find_field_index<Name, I, First, Rest...>
        {
            static constexpr std::size_t value =
                (Name == First::name) ? I : find_field_index<Name, I + 1, Rest...>::value;
        };

        template<fixed_string Name, std::size_t I>
        struct find_field_index<Name, I>
        {
            static constexpr std::size_t value = static_cast<std::size_t>(-1); // not found
        };

        /// Get the type of field at index I.
        template<std::size_t I, typename... Fields>
        struct type_at;

        template<std::size_t I, typename First, typename... Rest>
        struct type_at<I, First, Rest...>
        {
            using type = typename type_at<I - 1, Rest...>::type;
        };

        template<typename First, typename... Rest>
        struct type_at<0, First, Rest...>
        {
            using type = typename First::type;
        };

        /// Compute offset of field at index I (sum of slot_sizes of fields 0..I-1).
        template<std::size_t I, std::size_t Acc, typename... Fields>
        struct compute_offset;

        template<std::size_t I, std::size_t Acc, typename First, typename... Rest>
        struct compute_offset<I, Acc, First, Rest...>
        {
            static constexpr std::size_t value =
                (I == 0) ? Acc : compute_offset<I - 1, Acc + First::type::slot_size, Rest...>::value;
        };

        template<std::size_t I, std::size_t Acc>
        struct compute_offset<I, Acc>
        {
            static constexpr std::size_t value = Acc;
        };

        /// Check if a name matches any field in the pack (for uniqueness validation).
        template<fixed_string Name, typename... Fs>
        struct count_name;

        template<fixed_string Name>
        struct count_name<Name>
        {
            static constexpr std::size_t value = 0;
        };

        template<fixed_string Name, typename First, typename... Rest>
        struct count_name<Name, First, Rest...>
        {
            static constexpr std::size_t value =
                (Name == First::name ? 1 : 0) + count_name<Name, Rest...>::value;
        };

        /// Check that ALL field names in the pack are unique (each appears exactly once).
        template<typename... Fs>
        struct all_names_unique;

        template<>
        struct all_names_unique<>
        {
            static constexpr bool value = true;
        };

        template<typename First, typename... Rest>
        struct all_names_unique<First, Rest...>
        {
            static constexpr bool value =
                (count_name<First::name, First, Rest...>::value == 1) &&
                all_names_unique<Rest...>::value;
        };

    } // namespace detail

    /// Compile-time record — like a tuple with named fields.
    /// All field offsets are computed at compile time. Access is O(1).
    /// Field names must be unique (enforced at compile time).
    template<typename... Fields>
    struct record
    {
        static_assert(detail::all_names_unique<Fields...>::value,
            "msm::record: duplicate field names detected. All field names must be unique.");

        using native_type = record<Fields...>; // self-referential for array compatibility
        static constexpr std::size_t header_size = 8; // [is_const:1|type_id:1|field_count:2|reserved:4]
        static constexpr std::size_t slot_size = header_size + (Fields::type::slot_size + ... + 0);
        static constexpr std::size_t field_count = sizeof...(Fields);
        std::int8_t* ptr_;

        /// Get offset of field at index I (includes header).
        template<std::size_t I>
        static constexpr std::size_t field_offset()
        {
            return header_size + detail::compute_offset<I, 0, Fields...>::value;
        }

        /// Get type of field at index I (for use by array::push_back).
        template<std::size_t I>
        using field_type = typename detail::type_at<I, Fields...>::type;

        /// Deallocate nested data for all fields that have dynamic allocations.
        /// Called by allocator on msm_free.
        static void deallocate(std::int8_t* ptr)
        {
            if (!ptr) return;
            deallocate_fields<header_size, Fields...>(ptr);
        }

        /// Access field by compile-time name. Returns a type wrapper pointing to the field's memory.
        /// Delegates to msm_record_get_field C API for uniform cross-language behavior.
        template<fixed_string Name>
        auto get()
        {
            constexpr std::size_t idx = detail::find_field_index<Name, 0, Fields...>::value;
            static_assert(idx != static_cast<std::size_t>(-1), "Field name not found in record");

            constexpr std::size_t offset = header_size + detail::compute_offset<idx, 0, Fields...>::value;
            using field_type = typename detail::type_at<idx, Fields...>::type;

            return field_type{ msm_record_get_field(ptr_, static_cast<std::uint32_t>(offset)) };
        }

        /// Const access.
        template<fixed_string Name>
        auto get() const
        {
            constexpr std::size_t idx = detail::find_field_index<Name, 0, Fields...>::value;
            static_assert(idx != static_cast<std::size_t>(-1), "Field name not found in record");

            constexpr std::size_t offset = header_size + detail::compute_offset<idx, 0, Fields...>::value;
            using field_type = typename detail::type_at<idx, Fields...>::type;

            return field_type{ msm_record_get_field(const_cast<std::int8_t*>(ptr_), static_cast<std::uint32_t>(offset)) };
        }

    private:
        template<std::size_t Offset, typename First, typename... Rest>
        static void deallocate_fields(std::int8_t* ptr)
        {
            First::type::deallocate(ptr + Offset);
            if constexpr (sizeof...(Rest) > 0)
            {
                deallocate_fields<Offset + First::type::slot_size, Rest...>(ptr);
            }
        }

        template<std::size_t Offset>
        static void deallocate_fields(std::int8_t*) {}
    };

} // namespace msm
