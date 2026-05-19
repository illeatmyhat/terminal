// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// validate(WorkspaceModelData) walks the model and returns the first
// invariant violation it finds, or std::nullopt when the model is
// well-formed.
//
// The validator is the single source of truth for what constitutes a legal
// model state. Every mutator must produce a state that satisfies it; the
// property fuzzer (later slice) calls validate() after every random op.
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
    };

    [[nodiscard]] std::optional<Violation> validate(const WorkspaceModelData& m) noexcept;
}
