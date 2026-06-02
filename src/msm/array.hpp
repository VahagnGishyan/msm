#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

// ═══════════════════════════════════════════════════════════════════════════════
// C API declarations (implemented in msm-allocator DLL).
// Single source of truth for array operations.
// Both msm::array<T> (C++) and MsmArray (C#) delegate to these.
// ═══════════════════════════════════════════════════════════════════════════════

extern "C"
{
    std::uint32_t msm_array_size(std::int8_t* slot);
    std::uint32_t msm_array_capacity(std::int8_t* slot);
    void          msm_array_reserve(std::int8_t* slot, std::uint32_t new_cap, std::uint32_t item_size);
    std::int8_t*  msm_array_push_back(std::int8_t* slot, std::uint32_t item_size);
    std::int8_t*  msm_array_get(std::int8_t* slot, std::uint32_t index, std::uint32_t item_size);
    void          msm_array_clear(std::int8_t* slot);
}

namespace msm
{
    /// array<ItemType> — a dynamic array that delegates to the msm_array_* C
    /// API, so C++ and the other language bindings share one implementation.
    ///
    /// The slot, stored inline in the parent record, is 16 bytes:
    ///   slot:   [header:8 | data_ptr:8]
    /// Size and capacity live in the heap buffer's header, not the slot:
    ///   buffer: [size:4 | cap:4 | elements: size × item_size bytes]

    template<typename ItemType>
    struct array
    {
        using native_type = array<ItemType>;
        static constexpr std::size_t slot_size = 16; // header(8) + data_ptr(8); size and cap live in the buffer
        static constexpr std::uint32_t item_size = static_cast<std::uint32_t>(ItemType::slot_size);

        std::int8_t* ptr_; // points to the 16-byte slot in parent record

        /// Deallocate nested data (called by allocator on msm_free).
        /// Variant B layout (since 2026-05-26):
        ///   slot: [header:8 | data_ptr:8]
        ///   buf:  [size:4 | cap:4 | elements: cap × item_size]
        /// Reads size from buffer header (buf+0), elements start at buf+8.
        static void deallocate(std::int8_t* slot)
        {
            if (!slot) return;

            // Read buffer pointer from slot
            std::int8_t* buf;
            std::memcpy(&buf, slot + 8, sizeof(buf));
            if (!buf) return;

            // If items have nested allocations, deallocate each.
            if constexpr (requires { ItemType::deallocate(nullptr); })
            {
                std::uint32_t sz;
                std::memcpy(&sz, buf, 4); // size at buf+0
                std::int8_t* elements = buf + 8;
                for (std::uint32_t i = 0; i < sz; ++i)
                {
                    ItemType::deallocate(elements + i * item_size);
                }
            }

            // Free the whole buffer (header + elements were one malloc).
            std::free(buf);

            // Null out pointer to prevent double-free
            std::int8_t* null_ptr = nullptr;
            std::memcpy(slot + 8, &null_ptr, sizeof(null_ptr));
        }

        // --- Public API (delegates to C API) ---

        std::size_t size() const { return msm_array_size(ptr_); }
        std::size_t capacity() const { return msm_array_capacity(ptr_); }
        bool empty() const { return msm_array_size(ptr_) == 0; }

        /// Access element at index i.
        ItemType operator[](std::size_t i)
        {
            return ItemType{ msm_array_get(ptr_, static_cast<std::uint32_t>(i), item_size) };
        }

        ItemType operator[](std::size_t i) const
        {
            return ItemType{ msm_array_get(ptr_, static_cast<std::uint32_t>(i), item_size) };
        }

        /// Push a value (for primitive/scalar types). Grows if needed.
        template<typename V>
        void push_back(V value)
        {
            std::int8_t* slot = msm_array_push_back(ptr_, item_size);
            ItemType item{ slot };
            item = value;
        }

        /// Emplace back — grows if needed, returns typed view to new slot.
        /// Use for record types: auto edge = edges.emplace_back();
        ItemType emplace_back()
        {
            std::int8_t* slot = msm_array_push_back(ptr_, item_size);
            return ItemType{ slot };
        }

        /// Push back with field values (for record types).
        /// Usage: edges.emplace_back(0, 1, 10);  // writes fields in order
        template<typename... Args>
        void emplace_back(Args... args)
            requires (sizeof...(Args) > 1)
        {
            auto item = emplace_back();
            write_fields(item, std::index_sequence_for<Args...>{}, args...);
        }

    private:
        template<typename Rec, typename... Args, std::size_t... I>
        static void write_fields(Rec& rec, std::index_sequence<I...>, Args... args)
        {
            ((write_field_at<I>(rec, args)), ...);
        }

        template<std::size_t I, typename Rec, typename V>
        static void write_field_at(Rec& rec, V value)
        {
            constexpr std::size_t offset = detail_offset<I>();
            typename detail_type_at<I>::type field{ rec.ptr_ + offset };
            field = static_cast<typename detail_type_at<I>::type::native_type>(value);
        }

        template<std::size_t I>
        static constexpr std::size_t detail_offset()
        {
            return ItemType::template field_offset<I>();
        }

        template<std::size_t I>
        struct detail_type_at
        {
            using type = typename ItemType::template field_type<I>;
        };

    public:

        /// Reserve capacity — delegates to C API.
        void reserve(std::uint32_t new_cap)
        {
            msm_array_reserve(ptr_, new_cap, item_size);
        }

        /// Clear all elements (does not free memory) — delegates to C API.
        void clear()
        {
            msm_array_clear(ptr_);
        }

        /// Free the data block (manual cleanup without the allocator). Header
        /// and elements were a single malloc, so one free releases everything.
        void destroy()
        {
            std::int8_t* buf;
            std::memcpy(&buf, ptr_ + 8, sizeof(buf));
            if (buf)
            {
                std::free(buf);
                std::int8_t* null_ptr = nullptr;
                std::memcpy(ptr_ + 8, &null_ptr, sizeof(null_ptr));
            }
            // size and cap lived inside buf, so freeing it is enough; slot+8
            // is now null, which makes msm_array_size report 0.
        }

        // --- Iterator ---

        struct iterator
        {
            std::int8_t* base_;
            std::size_t index_;

            auto operator*() const
            {
                return ItemType{ base_ + index_ * item_size };
            }
            iterator& operator++() { ++index_; return *this; }
            bool operator!=(const iterator& o) const { return index_ != o.index_; }
        };

        iterator begin() const
        {
            // ptr_+8 holds the buffer pointer; elements start after its 8-byte header.
            std::int8_t* buf;
            std::memcpy(&buf, ptr_ + 8, sizeof(buf));
            return iterator{ buf ? buf + 8 : nullptr, 0 };
        }

        iterator end() const
        {
            std::int8_t* buf;
            std::memcpy(&buf, ptr_ + 8, sizeof(buf));
            return iterator{ buf ? buf + 8 : nullptr, size() };
        }
    };

} // namespace msm
