#pragma once

#include "fixed_string.hpp"

namespace msm
{
    /// Named field wrapper — associates a compile-time name with an MSM type.
    /// Used as template parameter for record<>.
    template<fixed_string Name, typename Type>
    struct member
    {
        using type = Type;
        static constexpr auto name = Name;
    };

} // namespace msm
