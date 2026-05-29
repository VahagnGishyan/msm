#pragma once

#include <cstddef>
#include <algorithm>

namespace msm
{
    /// C++20 NTTP-compatible fixed string for compile-time field names.
    /// Usage: fixed_string<"hello"> produces a compile-time string usable as template parameter.
    template<std::size_t N>
    struct fixed_string
    {
        char data[N]{};
        static constexpr std::size_t size = N - 1; // exclude null terminator

        constexpr fixed_string() = default;

        constexpr fixed_string(const char (&str)[N])
        {
            std::copy_n(str, N, data);
        }

        constexpr bool operator==(const fixed_string& other) const
        {
            for (std::size_t i = 0; i < N; ++i)
            {
                if (data[i] != other.data[i]) return false;
            }
            return true;
        }

        template<std::size_t M>
        constexpr bool operator==(const fixed_string<M>&) const
        {
            return false; // different sizes are never equal
        }

        constexpr auto operator<=>(const fixed_string& other) const = default;
    };

    // Deduction guide
    template<std::size_t N>
    fixed_string(const char (&)[N]) -> fixed_string<N>;

} // namespace msm
