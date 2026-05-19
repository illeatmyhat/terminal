// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The immutable pane tree. A workspace's root pane is a PaneNode, which is
// either a LeafPane (a real container of tabs) or a SplitPane (a binary
// container of two child PaneNodes).
//
// SplitPane children are held by shared_ptr<const PaneNode> so structural
// updates can share substructure cheaply: mutating a deep leaf only needs
// to rebuild the path from the root to that leaf.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Ids.h"
#include "TabContent.h"

namespace WorkspaceModel
{
    // A plain RGBA color. We avoid winrt::Windows::UI::Color to keep this
    // header pure C++.
    struct Color
    {
        std::uint8_t r{ 0 };
        std::uint8_t g{ 0 };
        std::uint8_t b{ 0 };
        std::uint8_t a{ 0xFF };

        [[nodiscard]] friend bool operator==(const Color&,
                                             const Color&) noexcept = default;
    };

    // A single tab inside a leaf pane.
    //
    // `description` captures everything needed to recreate the tab from disk.
    // `mount` is a runtime-only handle that points at a live IPaneContent
    // entry in the (separate) ContentRegistry; it is std::nullopt while the
    // tab has not been materialised yet.
    struct TabRecord
    {
        TabId id{};
        TabContent description{};
        std::optional<ContentId> mount{};
        std::string customTitle{};
        std::optional<Color> runtimeColor{};
        bool pinned{ false };

        [[nodiscard]] friend bool operator==(const TabRecord&,
                                             const TabRecord&) noexcept = default;
    };

    // Forward declaration so SplitPane can hold shared_ptr<const PaneNode>.
    struct LeafPane;
    struct SplitPane;
    using PaneNode = std::variant<LeafPane, SplitPane>;

    // A leaf in the pane tree. Owns one or more tabs and a notion of which
    // tab is currently active.
    struct LeafPane
    {
        PaneId id{};
        std::vector<TabRecord> tabs{};
        std::size_t activeTabIdx{ 0 };

        [[nodiscard]] friend bool operator==(const LeafPane&,
                                             const LeafPane&) noexcept = default;
    };

    // The orientation a split is rendered in.
    // Horizontal = a horizontal splitter between two vertically-stacked
    // children (i.e. the splitter is a horizontal line).
    // Vertical   = a vertical splitter between two horizontally-stacked
    // children (i.e. the splitter is a vertical line).
    enum class Axis : std::uint8_t
    {
        Horizontal,
        Vertical,
    };

    // A binary split node. Both children are shared_ptr<const PaneNode> so
    // that structural sharing across model versions is cheap.
    //
    // `ratio` is in [0.0, 1.0] and represents how much of the available
    // extent is given to `left`. The renderer is free to clamp to a minimum
    // pixel size.
    struct SplitPane
    {
        PaneId id{};
        Axis axis{ Axis::Vertical };
        double ratio{ 0.5 };
        std::shared_ptr<const PaneNode> left{};
        std::shared_ptr<const PaneNode> right{};

        [[nodiscard]] friend bool operator==(const SplitPane& lhs,
                                             const SplitPane& rhs) noexcept
        {
            if (lhs.id != rhs.id || lhs.axis != rhs.axis || lhs.ratio != rhs.ratio)
            {
                return false;
            }
            const auto leftEq = (lhs.left == rhs.left) ||
                                (lhs.left && rhs.left && *lhs.left == *rhs.left);
            const auto rightEq = (lhs.right == rhs.right) ||
                                 (lhs.right && rhs.right && *lhs.right == *rhs.right);
            return leftEq && rightEq;
        }
    };
}
