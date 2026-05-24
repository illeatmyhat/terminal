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

        // True iff every leaf in the subtree has its active tab materialised
        // (tabs[activeTabIdx].mount set). Used by invariant 9 to check that
        // the active workspace's content is live. Malformed leaves (empty /
        // out-of-range activeTabIdx) are reported by earlier invariants, so
        // here they are treated as "not unmounted" to avoid masking the more
        // specific violation.
        [[nodiscard]] bool activeTabsMaterialised(const PaneNode& node) noexcept
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                if (leaf->tabs.empty() || leaf->activeTabIdx >= leaf->tabs.size())
                {
                    return true; // structural problem already caught upstream
                }
                return leaf->tabs[leaf->activeTabIdx].mount.has_value();
            }
            const auto& split = std::get<SplitPane>(node);
            if (split.left && !activeTabsMaterialised(*split.left))
            {
                return false;
            }
            if (split.right && !activeTabsMaterialised(*split.right))
            {
                return false;
            }
            return true;
        }

        // The workspace named by activeWorkspaceId, or nullptr if the model
        // is empty / the id is dangling (invariant 6 reports the latter).
        [[nodiscard]] const WorkspaceState* findActiveWorkspace(const WorkspaceModelData& m) noexcept
        {
            if (!m.activeWorkspaceId.has_value())
            {
                return nullptr;
            }
            for (const auto& ws : m.workspaces)
            {
                if (ws.id == *m.activeWorkspaceId)
                {
                    return &ws;
                }
            }
            return nullptr;
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

        // Invariant 10: in display order, every pinned workspace precedes
        // every unpinned one (the pinned block is a contiguous prefix). Once we
        // have seen an unpinned workspace, no later workspace may be pinned.
        {
            bool sawUnpinned = false;
            for (const auto& ws : m.workspaces)
            {
                if (ws.pinned)
                {
                    if (sawUnpinned)
                    {
                        return Violation::PinnedNotContiguous;
                    }
                }
                else
                {
                    sawUnpinned = true;
                }
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

        // Invariant 9: the active workspace's content is materialised — every
        // leaf's active tab in the active workspace carries a mount. (The
        // mount policy establishes this at the end of every action; here we
        // assert it as a model-wide invariant. Inactive workspaces are
        // intentionally unconstrained.)
        if (m.activeWorkspaceId.has_value())
        {
            if (const auto* ws = findActiveWorkspace(m))
            {
                if (!activeTabsMaterialised(ws->root))
                {
                    return Violation::ActiveContentNotMounted;
                }
            }
        }

        return std::nullopt;
    }
}
