// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// WorkspaceChange — the typed list of events diff() emits when comparing
// two ModelState values. An IWorkspaceView implementation reacts to each
// change by mutating its target representation (XAML, in production; an
// in-memory recorder in tests).
//
// The 16 arms describe state transitions in the past tense — they are
// events the view observes, not commands applied to a reducer. Field
// types mirror the model's own types (e.g. names are `std::string`,
// colors are `WorkspaceModel::Color`) so the `WorkspaceModel` lib
// remains winrt-free and the same headers compile against both the
// production XAML view and the pure-C++ test mock.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Ids.h"
#include "PaneTree.h"
#include "TabContent.h"

namespace WorkspaceModel
{
    // ---------------------------------------------------------------------
    // Workspace-level changes
    // ---------------------------------------------------------------------

    struct WorkspaceAdded
    {
        WorkspaceId id{};
        std::string name{};
        std::optional<Color> color{};
        bool pinned{ false };
        // The arm carries the workspace's stable id only; the view resolves
        // the display/insertion position from that id through its own
        // id->XAML resolver at apply time. (Phase 1 had a `position`
        // display-index here, but "workspace display index == classic tab
        // index" is a Phase-1-only invariant that breaks once multi-
        // workspace / per-pane tabs land — see #44/#45.)
        //
        // name/color/pinned are the workspace's initial render metadata; a
        // later mutation to any of them while the workspace persists is
        // carried by WorkspaceMetadataUpdated.

        [[nodiscard]] friend bool operator==(const WorkspaceAdded&, const WorkspaceAdded&) noexcept = default;
    };

    struct WorkspaceRemoved
    {
        WorkspaceId id{};

        [[nodiscard]] friend bool operator==(const WorkspaceRemoved&, const WorkspaceRemoved&) noexcept = default;
    };

    struct ActiveWorkspaceChanged
    {
        // The newly-active workspace's stable id, or std::nullopt for the
        // empty-model case (no workspace active → no classic tab to select).
        // The view resolves `id` to the classic tab to select through its
        // own id->XAML resolver; it no longer carries a display index (that
        // index-as-identity coupling is removed — see #44/#45).
        std::optional<WorkspaceId> id{};

        [[nodiscard]] friend bool operator==(const ActiveWorkspaceChanged&, const ActiveWorkspaceChanged&) noexcept = default;
    };

    // A surviving workspace's display metadata (name / color / pin) changed.
    // The workspace analogue of TabDecorationUpdated: the view resolves `id`
    // to its sidebar row through its own id->XAML resolver and re-renders.
    struct WorkspaceMetadataUpdated
    {
        WorkspaceId id{};
        std::string name{};
        std::optional<Color> color{};
        bool pinned{ false };
        [[nodiscard]] friend bool operator==(const WorkspaceMetadataUpdated&, const WorkspaceMetadataUpdated&) noexcept = default;
    };

    // The sidebar display order of the (surviving) workspaces changed. Carries
    // the FULL new display order as a list of stable WorkspaceIds — the view
    // re-sequences its sidebar rows to match, resolving each id to its row.
    //
    // Carrying the whole order (rather than a single move) keeps the arm
    // self-describing and order-agnostic: the view never has to reconstruct the
    // target order from a delta, and the model never has to commit to a
    // particular move encoding. It is emitted whenever the relative order of
    // the ids common to (prev, next) differs — e.g. pinning a workspace floats
    // it within the list. A pin therefore produces BOTH a
    // WorkspaceMetadataUpdated (the pin glyph/bool, in place) AND a
    // WorkspaceReordered (the new sequence); the two are orthogonal — one
    // re-renders a row, the other moves rows.
    struct WorkspaceReordered
    {
        std::vector<WorkspaceId> order{};
        [[nodiscard]] friend bool operator==(const WorkspaceReordered&, const WorkspaceReordered&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Pane-tree changes
    // ---------------------------------------------------------------------

    // The SplitPane that contains a newly-created leaf: its id plus the
    // orientation/ratio the view needs to drive the classic split. Bundling
    // these together (rather than carrying loose parent-axis / parent-ratio
    // fields on LeafPaneCreated) makes the "axis/ratio are meaningless unless
    // there is a parent" state unrepresentable.
    struct ParentSplit
    {
        PaneId id{};
        Axis axis{ Axis::Vertical };
        double ratio{ 0.5 };

        [[nodiscard]] friend bool operator==(const ParentSplit&, const ParentSplit&) noexcept = default;
    };

    // A brand-new leaf pane has appeared. `parent` is std::nullopt when this
    // leaf is the workspace's root; otherwise it carries the containing
    // SplitPane (id + axis + ratio) so the view can both decide which
    // container to insert under and drive the classic split without
    // re-resolving the parent node.
    struct LeafPaneCreated
    {
        PaneId id{};
        std::optional<ParentSplit> parent{};

        [[nodiscard]] friend bool operator==(const LeafPaneCreated&, const LeafPaneCreated&) noexcept = default;
    };

    // A brand-new SplitPane has appeared. `left` and `right` are the
    // PaneIds of the children at the moment of emission; diff emits Create
    // events for any newly-introduced children before this one (see the
    // emit-ordering comment at the top of Diff.cpp).
    struct SplitPaneCreated
    {
        PaneId id{};
        Axis axis{ Axis::Vertical };
        double ratio{ 0.5 };
        PaneId left{};
        PaneId right{};

        [[nodiscard]] friend bool operator==(const SplitPaneCreated&, const SplitPaneCreated&) noexcept = default;
    };

    // A split has become single-child; the surviving sibling replaces the
    // split in the parent (or becomes the workspace root). `removedSplit`
    // is the SplitPane id that goes away; `survivor` is the PaneId of the
    // child that lifts up to take its place.
    struct SplitPaneCollapsed
    {
        PaneId removedSplit{};
        PaneId survivor{};

        [[nodiscard]] friend bool operator==(const SplitPaneCollapsed&, const SplitPaneCollapsed&) noexcept = default;
    };

    struct SplitRatioChanged
    {
        PaneId id{};
        double ratio{ 0.5 };

        [[nodiscard]] friend bool operator==(const SplitRatioChanged&, const SplitRatioChanged&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Tab changes
    // ---------------------------------------------------------------------

    struct TabAdded
    {
        PaneId leafId{};
        std::size_t idx{ 0 };
        TabId id{};
        std::string customTitle{};
        std::optional<Color> runtimeColor{};
        bool pinned{ false };
        // The tab's persistable content spec — the view materialises the
        // classic Tab from this without resolving the record by id.
        TabContent description{};
        // True when leafId lives inside a SplitPane. The split's new
        // sibling leaf is materialised by apply(LeafPaneCreated); when this
        // is set the TabAdded arm must NOT open an additional classic tab.
        bool leafInsideSplit{ false };
        // The workspace that owns this tab, for the classic-tab registry
        // binding. Always valid for a diff-emitted TabAdded (every tab lives
        // in exactly one workspace); the view's valid() check is defensive.
        WorkspaceId owningWorkspace{};

        [[nodiscard]] friend bool operator==(const TabAdded&, const TabAdded&) noexcept = default;
    };

    struct TabRemoved
    {
        PaneId leafId{};
        TabId id{};

        [[nodiscard]] friend bool operator==(const TabRemoved&, const TabRemoved&) noexcept = default;
    };

    // Identity-keyed move. diff emits one of these when a TabId exists in
    // both prev and next but at a different (leafId, idx). The XAML view
    // uses this signal to preserve the live IPaneContent (e.g. TermControl
    // + swap chain panel) across reorder / cross-leaf move; emitting
    // TabRemoved + TabAdded would tear down and re-create the content,
    // causing flicker and ConPTY disconnect.
    struct TabMoved
    {
        TabId id{};
        PaneId srcLeafId{};
        PaneId dstLeafId{};
        std::size_t dstIdx{ 0 };

        [[nodiscard]] friend bool operator==(const TabMoved&, const TabMoved&) noexcept = default;
    };

    struct ActiveTabChanged
    {
        PaneId leafId{};
        std::size_t idx{ 0 };

        [[nodiscard]] friend bool operator==(const ActiveTabChanged&, const ActiveTabChanged&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Content changes
    // ---------------------------------------------------------------------

    // Bind a ContentId to a live IPaneContent. The view materialises the
    // content from `description` if it isn't already mounted under
    // `contentId`. ContentUnmounted is the inverse.
    struct ContentMounted
    {
        TabId tabId{};
        ContentId contentId{};
        TabContent description{};
        // The stable id of the workspace that owns the mounted tab. diff()
        // resolves it from the tab's leaf the same way the decoration path
        // does. The view records a workspace->contents reverse index from
        // this so apply(WorkspaceRemoved) can tear down — and thus drop the
        // ConPTY of — every content a whole-workspace close orphans (a
        // whole-workspace close emits WorkspaceRemoved, not per-tab
        // TabRemoved/ContentUnmounted, so without this the factory-built
        // content would leak until the window exits). Appended (not inserted)
        // so existing hand-built ContentMounted positions stay valid.
        WorkspaceId owningWorkspace{};

        [[nodiscard]] friend bool operator==(const ContentMounted& lhs, const ContentMounted& rhs) noexcept
        {
            return lhs.tabId == rhs.tabId &&
                   lhs.contentId == rhs.contentId &&
                   lhs.description == rhs.description &&
                   lhs.owningWorkspace == rhs.owningWorkspace;
        }
    };

    struct ContentUnmounted
    {
        TabId tabId{};
        ContentId contentId{};

        [[nodiscard]] friend bool operator==(const ContentUnmounted&, const ContentUnmounted&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // Tab decoration (no structural change)
    // ---------------------------------------------------------------------

    struct TabDecorationUpdated
    {
        TabId id{};
        std::string customTitle{};
        std::optional<Color> runtimeColor{};
        bool pinned{ false };
        // The stable id of the workspace that owns the decorated tab. The
        // view resolves this to the classic tab the decoration applies to
        // through its own id->XAML resolver; it no longer carries a display
        // index (that index-as-identity coupling is removed — see #44/#45).
        WorkspaceId workspaceId{};

        [[nodiscard]] friend bool operator==(const TabDecorationUpdated&, const TabDecorationUpdated&) noexcept = default;
    };

    // ---------------------------------------------------------------------
    // The variant
    // ---------------------------------------------------------------------

    using WorkspaceChange = std::variant<
        WorkspaceAdded,
        WorkspaceRemoved,
        ActiveWorkspaceChanged,
        WorkspaceMetadataUpdated,
        WorkspaceReordered,
        LeafPaneCreated,
        SplitPaneCreated,
        SplitPaneCollapsed,
        SplitRatioChanged,
        TabAdded,
        TabRemoved,
        TabMoved,
        ActiveTabChanged,
        ContentMounted,
        ContentUnmounted,
        TabDecorationUpdated>;
}
