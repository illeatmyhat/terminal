// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Move mutators: moveTab, moveTabAsSplit.
//
// Both are atomic: the returned state has the tab fully removed from its
// source leaf and inserted at the destination in a single shared_ptr
// model snapshot. The source side cascades using the same rules as
// closeTab (leaf empty → leaf removed → split collapses → workspace
// removed if its root disappears). The destination side is a pure
// insertion (moveTab) or a pure split wrap (moveTabAsSplit).

#include "pch.h"

#include "MutatorHelpers.h"
#include "Mutators.h"

namespace WorkspaceModel
{
    namespace
    {
        using namespace detail;

        // Remove the tab at `tabIdx` from the leaf with id `srcLeafId` in
        // the workspace at index `wsIdx`, cascading if the leaf empties.
        // Returns the removed TabRecord. If the cascade removed the
        // workspace, sets `workspaceRemoved` to true and the workspace at
        // wsIdx is gone from m.workspaces.
        TabRecord detachTab(WorkspaceModelData& m,
                            std::size_t wsIdx,
                            PaneId srcLeafId,
                            std::size_t tabIdx,
                            bool& workspaceRemoved)
        {
            TabRecord taken;
            auto& ws = m.workspaces[wsIdx];

            bool found = false;
            auto newRoot = detail::transformLeaf(
                ws.root,
                srcLeafId,
                [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                    taken = leaf.tabs[tabIdx];
                    if (leaf.tabs.size() <= 1)
                    {
                        return std::nullopt; // leaf disappears
                    }
                    LeafPane out = leaf;
                    out.tabs.erase(out.tabs.begin() + static_cast<std::ptrdiff_t>(tabIdx));
                    if (tabIdx < out.activeTabIdx)
                    {
                        out.activeTabIdx -= 1;
                    }
                    else if (tabIdx == out.activeTabIdx)
                    {
                        if (out.activeTabIdx >= out.tabs.size())
                        {
                            out.activeTabIdx = out.tabs.size() - 1;
                        }
                    }
                    return out;
                },
                found);

            // found should be true; we located the tab before calling.
            if (!newRoot.has_value())
            {
                // Workspace gone.
                const auto removedId = ws.id;
                m.workspaces.erase(m.workspaces.begin() + static_cast<std::ptrdiff_t>(wsIdx));
                detail::mruErase(m.mru, removedId);
                if (m.activeWorkspaceId.has_value() && *m.activeWorkspaceId == removedId)
                {
                    if (!m.mru.empty())
                    {
                        m.activeWorkspaceId = m.mru.front();
                    }
                    else
                    {
                        m.activeWorkspaceId.reset();
                    }
                }
                workspaceRemoved = true;
                return taken;
            }

            ws.root = std::move(*newRoot);

            // Repair activePaneId if the source leaf disappeared.
            if (ws.activePaneId.valid() &&
                detail::findLeaf(ws.root, ws.activePaneId) == nullptr)
            {
                if (const auto* fallback = detail::firstLeaf(ws.root))
                {
                    ws.activePaneId = fallback->id;
                }
            }
            return taken;
        }
    }

    // ---------------------------------------------------------------------
    ModelState moveTab(const ModelState& state,
                       TabId tabId,
                       PaneId dstLeafId,
                       std::size_t dstIdx)
    {
        auto m = detail::copyOf(state);

        const auto srcLoc = detail::findTabLocationByIndex(m, tabId);
        if (!srcLoc.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        // Validate destination exists.
        const auto dstWsIdxOpt = detail::findWorkspaceIndexForLeaf(m, dstLeafId);
        if (!dstWsIdxOpt.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }

        const auto srcWsIdx = srcLoc->workspaceIdx;
        const auto srcLeafId = srcLoc->leafId;
        const auto srcTabIdx = srcLoc->indexInLeaf;

        // Special case: same-leaf reorder. Doing this through the
        // detach/reinsert path would cascade the leaf away if it's a
        // single-tab leaf; handle it directly.
        if (srcLeafId == dstLeafId)
        {
            auto& ws = m.workspaces[srcWsIdx];
            bool found = false;
            auto newRoot = detail::transformLeaf(
                ws.root,
                srcLeafId,
                [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                    LeafPane out = leaf;
                    if (srcTabIdx >= out.tabs.size())
                    {
                        return out; // shouldn't happen
                    }
                    auto t = out.tabs[srcTabIdx];
                    const auto wasActiveIdx = out.activeTabIdx;
                    const auto activeTabId = (wasActiveIdx < out.tabs.size())
                                                 ? out.tabs[wasActiveIdx].id
                                                 : TabId{};
                    out.tabs.erase(out.tabs.begin() + static_cast<std::ptrdiff_t>(srcTabIdx));

                    // Clamp & adjust dst to the new vector length.
                    auto clampedDst = std::min(dstIdx, out.tabs.size());
                    out.tabs.insert(out.tabs.begin() + static_cast<std::ptrdiff_t>(clampedDst),
                                    std::move(t));

                    // Restore active by id if it still exists; else clamp.
                    if (activeTabId.valid())
                    {
                        for (std::size_t i = 0; i < out.tabs.size(); ++i)
                        {
                            if (out.tabs[i].id == activeTabId)
                            {
                                out.activeTabIdx = i;
                                break;
                            }
                        }
                    }
                    if (out.activeTabIdx >= out.tabs.size())
                    {
                        out.activeTabIdx = out.tabs.empty() ? 0 : out.tabs.size() - 1;
                    }
                    return out;
                },
                found);
            if (found && newRoot.has_value())
            {
                ws.root = std::move(*newRoot);
            }
            return detail::finalize(std::move(m));
        }

        // Cross-leaf (possibly cross-workspace) move.
        bool workspaceRemoved = false;
        TabRecord taken = detachTab(m, srcWsIdx, srcLeafId, srcTabIdx, workspaceRemoved);

        // After detach, the dst workspace index might have shifted if
        // workspaceRemoved && srcWsIdx < dstWsIdxOpt.
        std::size_t dstWsIdx = *dstWsIdxOpt;
        if (workspaceRemoved && srcWsIdx < dstWsIdx)
        {
            dstWsIdx -= 1;
        }

        // Insert into the destination.
        auto& dstWs = m.workspaces[dstWsIdx];
        bool dstFound = false;
        auto dstRoot = detail::transformLeaf(
            dstWs.root,
            dstLeafId,
            [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                LeafPane out = leaf;
                const auto clamped = std::min(dstIdx, out.tabs.size());
                out.tabs.insert(out.tabs.begin() + static_cast<std::ptrdiff_t>(clamped),
                                taken);
                // If dst's activeTabIdx was at or past the insertion
                // point, the insertion shifts the active tab's effective
                // id to the right. We preserve the active tab by id.
                const auto wasActiveIdx = leaf.activeTabIdx;
                const auto activeTabId = (wasActiveIdx < leaf.tabs.size())
                                             ? leaf.tabs[wasActiveIdx].id
                                             : TabId{};
                if (activeTabId.valid())
                {
                    for (std::size_t i = 0; i < out.tabs.size(); ++i)
                    {
                        if (out.tabs[i].id == activeTabId)
                        {
                            out.activeTabIdx = i;
                            break;
                        }
                    }
                }
                if (out.activeTabIdx >= out.tabs.size())
                {
                    out.activeTabIdx = out.tabs.empty() ? 0 : out.tabs.size() - 1;
                }
                return out;
            },
            dstFound);
        if (dstFound && dstRoot.has_value())
        {
            dstWs.root = std::move(*dstRoot);
        }

        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState moveTabAsSplit(const ModelState& state,
                              TabId tabId,
                              PaneId dstLeafId,
                              Edge edge)
    {
        auto m = detail::copyOf(state);

        const auto srcLoc = detail::findTabLocationByIndex(m, tabId);
        if (!srcLoc.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto dstWsIdxOpt = detail::findWorkspaceIndexForLeaf(m, dstLeafId);
        if (!dstWsIdxOpt.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }

        const auto srcWsIdx = srcLoc->workspaceIdx;
        const auto srcLeafId = srcLoc->leafId;
        const auto srcTabIdx = srcLoc->indexInLeaf;

        // If src and dst are the same single-tab leaf, this is a no-op
        // (it would amount to splitting a leaf containing only the tab
        // we're "moving" — the result is identical to the input). Skip.
        if (srcLeafId == dstLeafId)
        {
            // We could implement a "promote tab into a sibling pane"
            // semantic here, but the spec frames moveTabAsSplit as
            // taking from one pane and splitting into another. Same
            // pane is a degenerate input — return the input unchanged.
            return state ? state : detail::finalize(std::move(m));
        }

        // Detach the tab from the source. May remove the source
        // workspace; we must therefore re-resolve the destination
        // workspace index afterwards.
        bool workspaceRemoved = false;
        TabRecord taken = detachTab(m, srcWsIdx, srcLeafId, srcTabIdx, workspaceRemoved);

        std::size_t dstWsIdx = *dstWsIdxOpt;
        if (workspaceRemoved && srcWsIdx < dstWsIdx)
        {
            dstWsIdx -= 1;
        }

        // Mint ids for the new SplitPane wrapper + new sibling leaf.
        const auto newSplitId = PaneId{ detail::allocId(m.idCounter) };
        const auto newLeafId = PaneId{ detail::allocId(m.idCounter) };

        // Determine axis and side.
        Axis axis = Axis::Vertical;
        bool newOnRight = true;
        switch (edge)
        {
        case Edge::Left:
            axis = Axis::Vertical;
            newOnRight = false;
            break;
        case Edge::Right:
            axis = Axis::Vertical;
            newOnRight = true;
            break;
        case Edge::Top:
            axis = Axis::Horizontal;
            newOnRight = false;
            break;
        case Edge::Bottom:
            axis = Axis::Horizontal;
            newOnRight = true;
            break;
        }

        // Build the new sibling leaf with the taken tab.
        LeafPane newSibling;
        newSibling.id = newLeafId;
        newSibling.tabs.push_back(std::move(taken));
        newSibling.activeTabIdx = 0;

        // Wrap the destination leaf in a new SplitPane.
        auto& dstWs = m.workspaces[dstWsIdx];
        const auto* dstLeaf = detail::findLeaf(dstWs.root, dstLeafId);
        if (!dstLeaf)
        {
            // Destination disappeared (e.g. due to detach cascade if dst
            // was somehow tied to src — shouldn't happen given the
            // same-leaf early-out above, but be safe).
            return state ? state : detail::finalize(std::move(m));
        }

        SplitPane newSplit;
        newSplit.id = newSplitId;
        newSplit.axis = axis;
        newSplit.ratio = 0.5;
        if (newOnRight)
        {
            newSplit.left = std::make_shared<const PaneNode>(PaneNode{ *dstLeaf });
            newSplit.right = std::make_shared<const PaneNode>(PaneNode{ newSibling });
        }
        else
        {
            newSplit.left = std::make_shared<const PaneNode>(PaneNode{ newSibling });
            newSplit.right = std::make_shared<const PaneNode>(PaneNode{ *dstLeaf });
        }

        bool found = false;
        dstWs.root = detail::replaceLeafWithSubtree(dstWs.root, dstLeafId, PaneNode{ newSplit }, found);
        dstWs.activePaneId = newLeafId;

        return detail::finalize(std::move(m));
    }
}
