// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The 25 atomic mutators of the workspace data model.
//
// Each mutator takes a `ModelState` (= `shared_ptr<const WorkspaceModelData>`)
// and returns a new `ModelState`. Mutators are pure: structural sharing of
// the underlying pane tree means returning a new state is cheap.
//
// Every mutator must produce a returned state that satisfies `validate()`.
// In debug builds the mutators assert this internally; the test suite
// asserts it externally for every case.
//
// Cascade rules (re-stated from issue #11 and the PRD #9):
//   - closeTab emptying a leaf → leaf removed → if the leaf's parent split
//     becomes single-child, the split collapses to the surviving sibling →
//     if the workspace's root pane is gone, the workspace is removed →
//     activeWorkspaceId falls back to MRU next or std::nullopt.
//   - closeWorkspace removing the last workspace leaves workspaces.empty()
//     and activeWorkspaceId == std::nullopt.
//   - splitPane wraps the original leaf in a new SplitPane (kept by
//     shared_ptr); the leaf's PaneId is preserved across the mutation.
//   - moveTab: source leaf cascade matches closeTab rules; destination leaf
//     gets the tab inserted atomically; both activeTabIdx fields are
//     updated consistently in the same returned state.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "Ids.h"
#include "PaneTree.h"
#include "TabContent.h"
#include "WorkspaceState.h"

namespace WorkspaceModel
{
    // The application's handle on a model state. Mutators take and return
    // values of this type; structural sharing keeps the per-mutation cost
    // proportional to the depth of the affected leaf, not the model size.
    using ModelState = std::shared_ptr<const WorkspaceModelData>;

    // ---------------------------------------------------------------------
    // Workspace lifecycle (10 ops)
    // ---------------------------------------------------------------------

    struct NewWorkspaceResult
    {
        ModelState state;
        WorkspaceId id;
    };

    // Append a new workspace. Requires `initialTab` so the new workspace
    // satisfies invariant 1 (a leaf has at least one tab) from the
    // moment it's created. The new workspace becomes the active workspace
    // and moves to the front of the MRU.
    [[nodiscard]] NewWorkspaceResult newWorkspace(const ModelState& state,
                                                  std::string name,
                                                  TabContent initialTab,
                                                  std::string initialTabTitle = {},
                                                  std::optional<Color> initialTabColor = std::nullopt,
                                                  bool initialTabPinned = false);

    [[nodiscard]] ModelState closeWorkspace(const ModelState& state, WorkspaceId id);
    [[nodiscard]] ModelState closeOtherWorkspaces(const ModelState& state, WorkspaceId keep);
    [[nodiscard]] ModelState closeAllWorkspaces(const ModelState& state);
    [[nodiscard]] ModelState switchToWorkspace(const ModelState& state, WorkspaceId id);
    [[nodiscard]] ModelState renameWorkspace(const ModelState& state, WorkspaceId id, std::string name);
    [[nodiscard]] ModelState setWorkspaceColor(const ModelState& state,
                                               WorkspaceId id,
                                               std::optional<Color> color);
    [[nodiscard]] ModelState setWorkspaceDescription(const ModelState& state,
                                                     WorkspaceId id,
                                                     std::string description);
    [[nodiscard]] ModelState setWorkspacePinned(const ModelState& state, WorkspaceId id, bool pinned);

    // Reorder the workspace with id `id` to index `dstIdx` in the
    // workspaces vector. Out-of-range indices are clamped to the valid
    // range [0, workspaces.size()).
    [[nodiscard]] ModelState reorderWorkspace(const ModelState& state, WorkspaceId id, std::size_t dstIdx);

    // ---------------------------------------------------------------------
    // Tab (8 ops)
    // ---------------------------------------------------------------------

    struct NewTabResult
    {
        ModelState state;
        TabId id;
    };

    // Add a new tab to a specific leaf in a specific workspace. Carries
    // every persistable field (description + customTitle + runtimeColor +
    // pinned) so a future cross-window IPC tear-out can reconstruct the
    // tab faithfully. The new tab becomes the leaf's active tab.
    //
    // If the destination leaf doesn't exist, returns the input state
    // unchanged with TabId{0}. Callers shouldn't rely on that — they
    // should know the leaf exists.
    [[nodiscard]] NewTabResult newTab(const ModelState& state,
                                      WorkspaceId workspaceId,
                                      PaneId leafId,
                                      TabContent description,
                                      std::string customTitle = {},
                                      std::optional<Color> runtimeColor = std::nullopt,
                                      bool pinned = false);

    [[nodiscard]] ModelState closeTab(const ModelState& state, TabId id);

    // Close every tab to the right of (i.e. after) the tab with id `id`
    // in its leaf. The named tab itself stays.
    [[nodiscard]] ModelState closeTabsRight(const ModelState& state, TabId id);

    // Close every tab in the leaf containing `id` except `id` itself.
    [[nodiscard]] ModelState closeOtherTabs(const ModelState& state, TabId id);

    // Set the active tab to the one with id `id`. The containing leaf
    // also becomes the workspace's active pane and the workspace becomes
    // the model's active workspace (moved to front of MRU).
    [[nodiscard]] ModelState selectTab(const ModelState& state, TabId id);

    [[nodiscard]] ModelState setTabTitle(const ModelState& state, TabId id, std::string customTitle);
    [[nodiscard]] ModelState setTabColor(const ModelState& state, TabId id, std::optional<Color> color);
    [[nodiscard]] ModelState setTabPinned(const ModelState& state, TabId id, bool pinned);

    // ---------------------------------------------------------------------
    // Pane (4 ops)
    // ---------------------------------------------------------------------

    struct SplitPaneResult
    {
        ModelState state;
        PaneId newPaneId;
        TabId newTabId;
    };

    // Split a leaf into a SplitPane wrapping (original leaf) + (new leaf
    // containing one new tab). The new SplitPane uses `axis` and `ratio`,
    // and is placed in the tree exactly where the original leaf was. The
    // original leaf's PaneId is preserved.
    //
    // The new sibling leaf is placed on the right/bottom by default. To
    // place it on the left/top instead, use ratio < 0.5 and an axis as
    // appropriate to the rendering — but since which side is "new" is a
    // UI concern, this slice fixes new = right.
    //
    // If `leafId` doesn't exist, returns the input state with
    // newPaneId == PaneId{0} and newTabId == TabId{0}.
    [[nodiscard]] SplitPaneResult splitPane(const ModelState& state,
                                            PaneId leafId,
                                            Axis axis,
                                            double ratio,
                                            TabContent newTabDescription,
                                            std::string newTabCustomTitle = {},
                                            std::optional<Color> newTabColor = std::nullopt,
                                            bool newTabPinned = false);

    // Close every tab in the leaf with id `leafId` — equivalent to
    // closing the leaf and cascading. Same cascade rules as closeTab.
    [[nodiscard]] ModelState closePane(const ModelState& state, PaneId leafId);

    // Set the split ratio of the SplitPane with id `splitId`. The ratio
    // is clamped to [0.0, 1.0]. If `splitId` does not name a SplitPane,
    // returns the input state.
    [[nodiscard]] ModelState resizePane(const ModelState& state, PaneId splitId, double ratio);

    // Mark the leaf with id `leafId` as the active pane in its workspace.
    // Side-effect: the containing workspace becomes the active workspace
    // and is moved to the front of the MRU.
    [[nodiscard]] ModelState focusPane(const ModelState& state, PaneId leafId);

    // ---------------------------------------------------------------------
    // Move (2 ops)
    // ---------------------------------------------------------------------

    // Detach the tab with id `tabId` from its source leaf (cascading if
    // the source leaf becomes empty) and insert it at `dstIdx` in the
    // leaf with id `dstLeafId`. `dstLeafId` may live in any workspace.
    //
    // `dstIdx` is clamped to [0, destLeaf.tabs.size()] (inclusive end so
    // "append" works). If src and dst are the same leaf, this is a
    // reorder within that leaf.
    //
    // If `tabId` is not found or `dstLeafId` is not a leaf in the model,
    // returns the input state unchanged.
    [[nodiscard]] ModelState moveTab(const ModelState& state,
                                     TabId tabId,
                                     PaneId dstLeafId,
                                     std::size_t dstIdx);

    enum class Edge : std::uint8_t
    {
        Left,
        Right,
        Top,
        Bottom,
    };

    // Atomic: detach the tab with id `tabId` from its source leaf
    // (cascading if it becomes empty) and create a new SplitPane wrapping
    // (dst leaf) + (new leaf containing just the taken tab) at the
    // requested edge of the destination leaf. The dst leaf's PaneId is
    // preserved; the taken tab's TabId is preserved.
    //
    // Edge determines axis and which side the new leaf appears on:
    //   - Left:   axis=Vertical,   new leaf on the LEFT  of dst
    //   - Right:  axis=Vertical,   new leaf on the RIGHT of dst
    //   - Top:    axis=Horizontal, new leaf on the TOP   of dst
    //   - Bottom: axis=Horizontal, new leaf on the BOTTOM of dst
    //
    // If `tabId` or `dstLeafId` is invalid, returns the input state.
    [[nodiscard]] ModelState moveTabAsSplit(const ModelState& state,
                                            TabId tabId,
                                            PaneId dstLeafId,
                                            Edge edge);

    // ---------------------------------------------------------------------
    // UI prefs (1 op)
    // ---------------------------------------------------------------------

    // Negative or non-finite values are clamped to a minimum of 0.
    [[nodiscard]] ModelState setSidebarWidth(const ModelState& state, double width);
}
