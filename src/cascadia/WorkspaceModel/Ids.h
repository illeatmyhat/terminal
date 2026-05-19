// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Strong-typed monotonic IDs for the workspace data model.
//
// Each ID is a thin struct wrapping a uint64_t. The compiler will not let a
// WorkspaceId be passed where a PaneId is expected, even though both
// underlying types are uint64_t. The model treats id==0 as "no id assigned
// yet" / sentinel; the first id allocated by the monotonic counter is 1.
//
// No winrt::* or Windows.h dependency — this header is pure C++ and must
// compile against just the C++ standard library.

#pragma once

#include <cstdint>
#include <functional>

namespace WorkspaceModel
{
#define WORKSPACEMODEL_DEFINE_ID(Name)                                                          \
    struct Name                                                                                 \
    {                                                                                           \
        std::uint64_t v{ 0 };                                                                   \
        constexpr Name() noexcept = default;                                                    \
        constexpr explicit Name(std::uint64_t value) noexcept :                                 \
            v(value) {}                                                                         \
        [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }                  \
        [[nodiscard]] friend constexpr bool operator==(Name lhs, Name rhs) noexcept             \
        {                                                                                       \
            return lhs.v == rhs.v;                                                              \
        }                                                                                       \
        [[nodiscard]] friend constexpr bool operator!=(Name lhs, Name rhs) noexcept             \
        {                                                                                       \
            return lhs.v != rhs.v;                                                              \
        }                                                                                       \
        [[nodiscard]] friend constexpr bool operator<(Name lhs, Name rhs) noexcept              \
        {                                                                                       \
            return lhs.v < rhs.v;                                                               \
        }                                                                                       \
    };

    WORKSPACEMODEL_DEFINE_ID(WorkspaceId)
    WORKSPACEMODEL_DEFINE_ID(PaneId)
    WORKSPACEMODEL_DEFINE_ID(TabId)
    WORKSPACEMODEL_DEFINE_ID(ContentId)

#undef WORKSPACEMODEL_DEFINE_ID
}

namespace std
{
#define WORKSPACEMODEL_DEFINE_ID_HASH(Name)                                                     \
    template<>                                                                                  \
    struct hash<::WorkspaceModel::Name>                                                         \
    {                                                                                           \
        [[nodiscard]] std::size_t operator()(::WorkspaceModel::Name id) const noexcept          \
        {                                                                                       \
            return std::hash<std::uint64_t>{}(id.v);                                            \
        }                                                                                       \
    };

    WORKSPACEMODEL_DEFINE_ID_HASH(WorkspaceId)
    WORKSPACEMODEL_DEFINE_ID_HASH(PaneId)
    WORKSPACEMODEL_DEFINE_ID_HASH(TabId)
    WORKSPACEMODEL_DEFINE_ID_HASH(ContentId)

#undef WORKSPACEMODEL_DEFINE_ID_HASH
}
