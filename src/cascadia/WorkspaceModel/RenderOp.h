// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// RenderOp — the typed list of operations a reconciler emits when diffing
// two ModelState values. An IRenderSurface implementation applies each op
// to its target representation (XAML, in production; an in-memory recorder
// in tests).
//
// The 14 arms are locked by issue #9 (Q10) and the Slice 3 spec in
// issue #12. Field types intentionally mirror the model's own types (e.g.
// names are `std::string`, colors are `WorkspaceModel::Color`) so the
// `WorkspaceModel` lib remains winrt-free and the same headers compile
// against both the production XAML surface and the pure-C++ test mock.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

#include "Ids.h"
#include "PaneTree.h"
#include "TabContent.h"

namespace WorkspaceModel
{
    // ---------------------------------------------------------------------
    // Workspace-level ops
    // ---------------------------------------------------------------------

    struct AddWorkspace
    {
        WorkspaceId id{};
        std::string name{};
        std::optional<Color> color{};
        // Display position in the sidebar (index into workspaces vector at
        // the moment of emission). The renderer typically inserts at this
        // index.
        std::size_t position{ 0 };

        [[nodiscard]] friend bool operator==(const AddWorkspace&, const AddWorkspace&) noexcept = default;
    };

    struct RemoveWorkspace
    {
        WorkspaceId id{};

        [[nodiscard]] friend bool operator==(const RemoveWorkspace&, const RemoveWorkspace&) noexcept = default;
    };

    struct SetActiveWorkspace
    {
        std::optional<WorkspaceId> id{};

        [[nodiscard]] friend bool operator==(const SetActiveWorkspace&, const SetActiveWorkspace&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Pane-tree ops
    // ---------------------------------------------------------------------

    // Materialise a brand-new leaf pane. `parent` is std::nullopt when this
    // leaf is the workspace's root; otherwise it is the SplitPane id that
    // contains the leaf. The renderer can use `parent` to decide which
    // existing container to insert the new XAML element under.
    struct CreateLeafPane
    {
        PaneId id{};
        std::optional<PaneId> parent{};

        [[nodiscard]] friend bool operator==(const CreateLeafPane&, const CreateLeafPane&) noexcept = default;
    };

    // Materialise a brand-new SplitPane. `left` and `right` are the
    // PaneIds of the children at the moment of emission; the reconciler
    // emits Create ops for any newly-introduced children before this op
    // (see the emit ordering comment at the top of Reconciler.cpp).
    struct CreateSplitPane
    {
        PaneId id{};
        Axis axis{ Axis::Vertical };
        double ratio{ 0.5 };
        PaneId left{};
        PaneId right{};

        [[nodiscard]] friend bool operator==(const CreateSplitPane&, const CreateSplitPane&) noexcept = default;
    };

    // A split has become single-child; the surviving sibling replaces the
    // split in the parent (or becomes the workspace root). `removedSplit`
    // is the SplitPane id that goes away; `survivor` is the PaneId of the
    // child that lifts up to take its place.
    struct CollapseSplitPane
    {
        PaneId removedSplit{};
        PaneId survivor{};

        [[nodiscard]] friend bool operator==(const CollapseSplitPane&, const CollapseSplitPane&) noexcept = default;
    };

    struct SetSplitRatio
    {
        PaneId id{};
        double ratio{ 0.5 };

        [[nodiscard]] friend bool operator==(const SetSplitRatio&, const SetSplitRatio&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Tab ops
    // ---------------------------------------------------------------------

    struct AddTab
    {
        PaneId leafId{};
        std::size_t idx{ 0 };
        TabId id{};
        std::string customTitle{};
        std::optional<Color> runtimeColor{};
        bool pinned{ false };

        [[nodiscard]] friend bool operator==(const AddTab&, const AddTab&) noexcept = default;
    };

    struct RemoveTab
    {
        PaneId leafId{};
        TabId id{};

        [[nodiscard]] friend bool operator==(const RemoveTab&, const RemoveTab&) noexcept = default;
    };

    // Identity-keyed move. The reconciler emits one of these when a TabId
    // exists in both prev and next but at a different (leafId, idx).
    // The XAML surface uses this signal to preserve the live IPaneContent
    // (e.g. TermControl + swap chain panel) across reorder/cross-leaf
    // move; emitting RemoveTab + AddTab would tear down and re-create the
    // content, causing flicker and ConPTY disconnect.
    struct MoveTab
    {
        TabId id{};
        PaneId srcLeafId{};
        PaneId dstLeafId{};
        std::size_t dstIdx{ 0 };

        [[nodiscard]] friend bool operator==(const MoveTab&, const MoveTab&) noexcept = default;
    };

    struct SetActiveTab
    {
        PaneId leafId{};
        std::size_t idx{ 0 };

        [[nodiscard]] friend bool operator==(const SetActiveTab&, const SetActiveTab&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Content ops
    // ---------------------------------------------------------------------

    // Bind a ContentId to a live IPaneContent. The renderer materialises
    // the content from `description` if it isn't already mounted under
    // `contentId`. UnmountContent is the inverse.
    struct MountContent
    {
        TabId tabId{};
        ContentId contentId{};
        TabContent description{};

        [[nodiscard]] friend bool operator==(const MountContent& lhs, const MountContent& rhs) noexcept
        {
            return lhs.tabId == rhs.tabId &&
                   lhs.contentId == rhs.contentId &&
                   lhs.description == rhs.description;
        }
    };

    struct UnmountContent
    {
        TabId tabId{};
        ContentId contentId{};

        [[nodiscard]] friend bool operator==(const UnmountContent&, const UnmountContent&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Tab decoration (no structural change)
    // ---------------------------------------------------------------------

    struct UpdateTabDecoration
    {
        TabId id{};
        std::string customTitle{};
        std::optional<Color> runtimeColor{};
        bool pinned{ false };

        [[nodiscard]] friend bool operator==(const UpdateTabDecoration&, const UpdateTabDecoration&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // The variant
    // ---------------------------------------------------------------------

    using RenderOp = std::variant<
        AddWorkspace,
        RemoveWorkspace,
        SetActiveWorkspace,
        CreateLeafPane,
        CreateSplitPane,
        CollapseSplitPane,
        SetSplitRatio,
        AddTab,
        RemoveTab,
        MoveTab,
        SetActiveTab,
        MountContent,
        UnmountContent,
        UpdateTabDecoration>;
}
