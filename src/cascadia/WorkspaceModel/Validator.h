// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// validate(WorkspaceModelData) walks the model and returns the first
// invariant violation it finds, or std::nullopt when the model is
// well-formed.
//
// The validator is the single source of truth for what constitutes a legal
// model state. Every action must produce a state that satisfies it; the
// property fuzzer calls validate() after every random op.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <optional>

#include "WorkspaceState.h"

namespace WorkspaceModel
{
    enum class Violation
    {
        // Invariant 1: every leaf has at least one tab.
        LeafEmpty,

        // Invariant 2: every split has exactly two children, both non-null.
        SplitArityWrong,

        // Invariant 3: every workspace has exactly one root pane (with a
        // valid, non-zero PaneId).
        WorkspaceWithoutRoot,

        // Invariant 4: for every leaf, activeTabIdx is in [0, tabs.size()).
        ActiveTabIdxOutOfRange,

        // Invariant 5: each workspace's activePaneId is the id of some leaf
        // within that workspace's pane tree.
        ActivePaneIdInvalid,

        // Invariant 6: activeWorkspaceId is either std::nullopt (empty
        // model) or refers to a workspace that exists.
        ActiveWorkspaceIdInvalid,

        // Invariant 7: mru is a permutation of the workspace ids.
        MruNotPermutationOfWorkspaces,

        // Invariant 8: no two TabRecord.mount ContentIds collide across the
        // whole model.
        DuplicateContentIdMount,

        // Invariant 9: the active workspace's content is materialised. For
        // every leaf in the workspace named by activeWorkspaceId, that leaf's
        // active tab (tabs[activeTabIdx]) must carry a mount. This is the
        // model-side projection of the mount policy (option I): the currently
        // visible workspace's panes are always live. Inactive workspaces are
        // unconstrained — a tab there may be materialised (kept alive across a
        // switch-away) or not (never visited), and either is legal, which is
        // why this invariant is scoped to the active workspace only.
        ActiveContentNotMounted,

        // Invariant 10: in display order, every pinned workspace precedes every
        // unpinned workspace (the pinned block is a contiguous prefix). Pinned
        // workspaces float to the top of the sidebar; an unpinned workspace
        // sitting before a pinned one is unrepresentable. The setWorkspacePinned
        // action establishes this and detail::finalize() re-normalizes it after
        // every action (so e.g. a free-form reorderWorkspace can never leave a
        // pinned/unpinned interleave).
        PinnedNotContiguous,
    };

    [[nodiscard]] std::optional<Violation> validate(const WorkspaceModelData& m) noexcept;
}
