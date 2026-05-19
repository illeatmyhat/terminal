// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Implementations of the const-member queries declared on
// WorkspaceModelData. The template query (`findFirstTabOfKind<SpecT>`)
// is defined inline in WorkspaceState.h because templates have to be
// in headers.

#include "pch.h"

#include "Cascade.h"
#include "WorkspaceState.h"

namespace WorkspaceModel
{
    namespace
    {
        // Find the SplitPane node whose direct child has the given id.
        // Walks the subtree; returns nullptr if not found.
        [[nodiscard]] const SplitPane* findParentSplit(const PaneNode& node, PaneId target) noexcept
        {
            if (std::holds_alternative<LeafPane>(node))
            {
                return nullptr;
            }
            const auto& split = std::get<SplitPane>(node);
            // Is `target` the direct child?
            auto idOf = [](const PaneNode& n) -> PaneId {
                if (const auto* leaf = std::get_if<LeafPane>(&n))
                {
                    return leaf->id;
                }
                return std::get<SplitPane>(n).id;
            };
            if (split.left && idOf(*split.left) == target)
            {
                return &split;
            }
            if (split.right && idOf(*split.right) == target)
            {
                return &split;
            }
            if (split.left)
            {
                if (auto* r = findParentSplit(*split.left, target))
                {
                    return r;
                }
            }
            if (split.right)
            {
                if (auto* r = findParentSplit(*split.right, target))
                {
                    return r;
                }
            }
            return nullptr;
        }

        // Find a pane (leaf or split) anywhere in the subtree.
        [[nodiscard]] const PaneNode* findPaneInSubtree(const PaneNode& node, PaneId target) noexcept
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                return (leaf->id == target) ? &node : nullptr;
            }
            const auto& split = std::get<SplitPane>(node);
            if (split.id == target)
            {
                return &node;
            }
            if (split.left)
            {
                if (auto* r = findPaneInSubtree(*split.left, target))
                {
                    return r;
                }
            }
            if (split.right)
            {
                if (auto* r = findPaneInSubtree(*split.right, target))
                {
                    return r;
                }
            }
            return nullptr;
        }
    }

    // -------------------------------------------------------------------
    const std::vector<WorkspaceState>& WorkspaceModelData::workspaces_view() const noexcept
    {
        return workspaces;
    }

    const WorkspaceState* WorkspaceModelData::workspace(WorkspaceId id) const noexcept
    {
        for (const auto& ws : workspaces)
        {
            if (ws.id == id)
            {
                return &ws;
            }
        }
        return nullptr;
    }

    std::optional<WorkspaceId> WorkspaceModelData::activeWorkspaceId_view() const noexcept
    {
        return activeWorkspaceId;
    }

    const std::deque<WorkspaceId>& WorkspaceModelData::mru_view() const noexcept
    {
        return mru;
    }

    const PaneNode* WorkspaceModelData::pane(PaneId id) const noexcept
    {
        for (const auto& ws : workspaces)
        {
            if (auto* p = findPaneInSubtree(ws.root, id))
            {
                return p;
            }
        }
        return nullptr;
    }

    const SplitPane* WorkspaceModelData::parentOf(PaneId id) const noexcept
    {
        for (const auto& ws : workspaces)
        {
            if (auto* p = findParentSplit(ws.root, id))
            {
                return p;
            }
        }
        return nullptr;
    }

    std::vector<const LeafPane*> WorkspaceModelData::leaves(WorkspaceId id) const
    {
        std::vector<const LeafPane*> out;
        for (const auto& ws : workspaces)
        {
            if (ws.id == id)
            {
                detail::collectLeaves(ws.root, out);
                return out;
            }
        }
        return out; // empty for unknown workspace
    }

    const TabRecord* WorkspaceModelData::tab(TabId id) const noexcept
    {
        for (const auto& ws : workspaces)
        {
            auto loc = detail::findTabInSubtree(ws.root, id);
            if (loc.leaf != nullptr)
            {
                return &loc.leaf->tabs[loc.indexInLeaf];
            }
        }
        return nullptr;
    }

    double WorkspaceModelData::sidebarWidth_view() const noexcept
    {
        return sidebarWidth;
    }

    // Public-but-internal helper used by the inline-defined
    // findFirstTabOfKind template. We delegate to detail::collectLeaves;
    // declaring our own thunk avoids leaking the entire Cascade.h
    // include into WorkspaceState.h.
    void WorkspaceModelData::detail_collectLeavesPublic(const PaneNode& node,
                                                        std::vector<const LeafPane*>& out)
    {
        detail::collectLeaves(node, out);
    }
}
