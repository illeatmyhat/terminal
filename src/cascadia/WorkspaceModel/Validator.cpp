// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "Validator.h"

#include <unordered_set>
#include <variant>

namespace WorkspaceModel
{
    namespace
    {
        // Walks a pane subtree and reports the first leaf/split structural
        // problem it sees. Returns std::nullopt when the subtree is sound.
        [[nodiscard]] std::optional<Violation> checkPaneStructure(const PaneNode& node) noexcept
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                if (leaf->tabs.empty())
                {
                    return Violation::LeafEmpty;
                }
                if (leaf->activeTabIdx >= leaf->tabs.size())
                {
                    return Violation::ActiveTabIdxOutOfRange;
                }
                return std::nullopt;
            }

            const auto& split = std::get<SplitPane>(node);
            if (!split.left || !split.right)
            {
                return Violation::SplitArityWrong;
            }
            if (auto v = checkPaneStructure(*split.left))
            {
                return v;
            }
            if (auto v = checkPaneStructure(*split.right))
            {
                return v;
            }
            return std::nullopt;
        }

        // True iff the subtree contains a leaf with the given PaneId.
        [[nodiscard]] bool subtreeContainsLeafWithId(const PaneNode& node, PaneId target) noexcept
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                return leaf->id == target;
            }
            const auto& split = std::get<SplitPane>(node);
            return (split.left && subtreeContainsLeafWithId(*split.left, target)) ||
                   (split.right && subtreeContainsLeafWithId(*split.right, target));
        }

        // True iff the root pane has a non-zero PaneId (invariant 3).
        [[nodiscard]] bool rootHasValidId(const PaneNode& node) noexcept
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                return leaf->id.valid();
            }
            return std::get<SplitPane>(node).id.valid();
        }

        // Append every TabRecord.mount in the subtree to `out`.
        void collectMounts(const PaneNode& node, std::vector<ContentId>& out)
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                for (const auto& tab : leaf->tabs)
                {
                    if (tab.mount.has_value())
                    {
                        out.push_back(*tab.mount);
                    }
                }
                return;
            }
            const auto& split = std::get<SplitPane>(node);
            if (split.left)
            {
                collectMounts(*split.left, out);
            }
            if (split.right)
            {
                collectMounts(*split.right, out);
            }
        }
    }

    std::optional<Violation> validate(const WorkspaceModelData& m) noexcept
    {
        // Walk every workspace's pane tree and check the per-workspace
        // invariants (1, 2, 3, 4, 5) before the model-wide invariants
        // (6, 7, 8).
        for (const auto& ws : m.workspaces)
        {
            if (!rootHasValidId(ws.root))
            {
                return Violation::WorkspaceWithoutRoot;
            }
            if (auto v = checkPaneStructure(ws.root))
            {
                return v;
            }
            if (!ws.activePaneId.valid() || !subtreeContainsLeafWithId(ws.root, ws.activePaneId))
            {
                return Violation::ActivePaneIdInvalid;
            }
        }

        // Invariant 6: activeWorkspaceId is std::nullopt OR points at an
        // extant workspace.
        if (m.activeWorkspaceId.has_value())
        {
            const auto wanted = *m.activeWorkspaceId;
            bool found = false;
            for (const auto& ws : m.workspaces)
            {
                if (ws.id == wanted)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return Violation::ActiveWorkspaceIdInvalid;
            }
        }

        // Invariant 7: MRU is a permutation of the workspace ids.
        {
            if (m.mru.size() != m.workspaces.size())
            {
                return Violation::MruNotPermutationOfWorkspaces;
            }
            std::unordered_set<WorkspaceId> workspaceIds;
            workspaceIds.reserve(m.workspaces.size());
            for (const auto& ws : m.workspaces)
            {
                if (!workspaceIds.insert(ws.id).second)
                {
                    // Duplicate workspace ids would also break MRU
                    // permutation semantics; report it under the same
                    // violation.
                    return Violation::MruNotPermutationOfWorkspaces;
                }
            }
            std::unordered_set<WorkspaceId> mruIds;
            mruIds.reserve(m.mru.size());
            for (const auto& id : m.mru)
            {
                if (!mruIds.insert(id).second)
                {
                    return Violation::MruNotPermutationOfWorkspaces;
                }
                if (workspaceIds.find(id) == workspaceIds.end())
                {
                    return Violation::MruNotPermutationOfWorkspaces;
                }
            }
        }

        // Invariant 8: every set TabRecord.mount is unique across the model.
        {
            std::vector<ContentId> mounts;
            for (const auto& ws : m.workspaces)
            {
                collectMounts(ws.root, mounts);
            }
            std::unordered_set<ContentId> seen;
            seen.reserve(mounts.size());
            for (const auto& cid : mounts)
            {
                if (!seen.insert(cid).second)
                {
                    return Violation::DuplicateContentIdMount;
                }
            }
        }

        return std::nullopt;
    }
}
