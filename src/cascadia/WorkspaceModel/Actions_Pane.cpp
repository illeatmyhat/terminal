// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Pane actions: splitPane, closePane, resizePane, focusPane.
//
// splitPane is the identity-preservation centrepiece. The original leaf's
// PaneId is unchanged after the action: a new SplitPane wraps the
// original leaf (kept by shared_ptr<const PaneNode>) plus a freshly minted
// sibling leaf. Tests assert that PaneId before == PaneId after.

#include "pch.h"

#include "WorkspaceActionHelpers.h"
#include "WorkspaceActions.h"

namespace WorkspaceModel
{
    namespace
    {
        using namespace detail;

        // Clamp a finite double into [lo, hi]. NaN maps to lo.
        [[nodiscard]] double clampRatio(double r) noexcept
        {
            if (!(r == r))
            {
                return 0.5; // NaN: default to even split
            }
            if (r < 0.0)
            {
                return 0.0;
            }
            if (r > 1.0)
            {
                return 1.0;
            }
            return r;
        }

        // Walks the subtree looking for a SplitPane with id `target`.
        // Returns std::nullopt if it doesn't exist, otherwise a rebuilt
        // copy of the subtree with that split's ratio updated to
        // `newRatio`.
        [[nodiscard]] std::optional<PaneNode> updateSplitRatio(const PaneNode& node,
                                                               PaneId target,
                                                               double newRatio)
        {
            if (std::holds_alternative<LeafPane>(node))
            {
                return std::nullopt;
            }
            const auto& split = std::get<SplitPane>(node);
            if (split.id == target)
            {
                SplitPane out = split;
                out.ratio = clampRatio(newRatio);
                return PaneNode{ out };
            }
            // Recurse into children. We rebuild the split for whichever
            // child contained the target.
            if (split.left)
            {
                if (auto r = updateSplitRatio(*split.left, target, newRatio))
                {
                    SplitPane out;
                    out.id = split.id;
                    out.axis = split.axis;
                    out.ratio = split.ratio;
                    out.left = std::make_shared<const PaneNode>(*r);
                    out.right = split.right;
                    return PaneNode{ out };
                }
            }
            if (split.right)
            {
                if (auto r = updateSplitRatio(*split.right, target, newRatio))
                {
                    SplitPane out;
                    out.id = split.id;
                    out.axis = split.axis;
                    out.ratio = split.ratio;
                    out.left = split.left;
                    out.right = std::make_shared<const PaneNode>(*r);
                    return PaneNode{ out };
                }
            }
            return std::nullopt;
        }
    }

    // ---------------------------------------------------------------------
    SplitPaneResult splitPane(const ModelState& state,
                              PaneId leafId,
                              Axis axis,
                              double ratio,
                              TabContent newTabDescription,
                              std::string newTabCustomTitle,
                              std::optional<Color> newTabColor,
                              bool newTabPinned)
    {
        auto m = detail::copyOf(state);

        const auto wsIdxOpt = detail::findWorkspaceIndexForLeaf(m, leafId);
        if (!wsIdxOpt.has_value())
        {
            return SplitPaneResult{ state ? state : detail::finalize(std::move(m)),
                                    PaneId{ 0 },
                                    TabId{ 0 } };
        }
        const auto wsIdx = *wsIdxOpt;

        // Mint the new ids.
        const auto newSplitId = PaneId{ detail::allocId(m.idCounter) };
        const auto newSiblingPaneId = PaneId{ detail::allocId(m.idCounter) };
        const auto newTabId = TabId{ detail::allocId(m.idCounter) };

        // The new sibling leaf holds one new tab.
        TabRecord t;
        t.id = newTabId;
        t.description = std::move(newTabDescription);
        t.customTitle = std::move(newTabCustomTitle);
        t.runtimeColor = newTabColor;
        t.pinned = newTabPinned;

        LeafPane sibling;
        sibling.id = newSiblingPaneId;
        sibling.tabs.push_back(std::move(t));
        sibling.activeTabIdx = 0;

        // Build the new SplitPane. The ORIGINAL leaf is preserved with
        // its PaneId intact and becomes the SplitPane's `left` child;
        // the new sibling leaf goes on the right.
        auto& ws = m.workspaces[wsIdx];
        const auto* originalLeaf = detail::findLeaf(ws.root, leafId);
        if (!originalLeaf)
        {
            // Shouldn't happen — we just confirmed it exists — but be safe.
            return SplitPaneResult{ state ? state : detail::finalize(std::move(m)),
                                    PaneId{ 0 },
                                    TabId{ 0 } };
        }

        SplitPane newSplit;
        newSplit.id = newSplitId;
        newSplit.axis = axis;
        newSplit.ratio = clampRatio(ratio);
        newSplit.left = std::make_shared<const PaneNode>(PaneNode{ *originalLeaf });
        newSplit.right = std::make_shared<const PaneNode>(PaneNode{ sibling });

        // Replace the original leaf with the new SplitPane subtree.
        bool found = false;
        ws.root = detail::replaceLeafWithSubtree(ws.root, leafId, PaneNode{ newSplit }, found);

        // Active pane focuses the new sibling leaf (matches typical
        // "split here, focus the new one" UX).
        ws.activePaneId = newSiblingPaneId;

        return SplitPaneResult{ detail::finalize(std::move(m)), newSiblingPaneId, newTabId };
    }

    // ---------------------------------------------------------------------
    ModelState closePane(const ModelState& state, PaneId leafId)
    {
        auto m = detail::copyOf(state);

        const auto wsIdxOpt = detail::findWorkspaceIndexForLeaf(m, leafId);
        if (!wsIdxOpt.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto wsIdx = *wsIdxOpt;

        // closePane is "close every tab in this leaf, then cascade" —
        // equivalent to the leaf-disappears branch of closeTab. We use
        // the same transform machinery: return std::nullopt to signal
        // "this leaf is gone."
        const auto wasActiveLeaf = (m.workspaces[wsIdx].activePaneId == leafId);

        bool found = false;
        auto newRoot = detail::transformLeaf(
            m.workspaces[wsIdx].root,
            leafId,
            [](const LeafPane&) -> std::optional<LeafPane> { return std::nullopt; },
            found);

        if (!found)
        {
            return state ? state : detail::finalize(std::move(m));
        }

        if (!newRoot.has_value())
        {
            // Workspace gone.
            const auto removedId = m.workspaces[wsIdx].id;
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
            return detail::finalize(std::move(m));
        }

        m.workspaces[wsIdx].root = std::move(*newRoot);
        if (wasActiveLeaf)
        {
            if (const auto* fallback = detail::firstLeaf(m.workspaces[wsIdx].root))
            {
                m.workspaces[wsIdx].activePaneId = fallback->id;
            }
        }
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState resizePane(const ModelState& state, PaneId splitId, double ratio)
    {
        auto m = detail::copyOf(state);

        for (auto& ws : m.workspaces)
        {
            auto updated = updateSplitRatio(ws.root, splitId, ratio);
            if (updated.has_value())
            {
                ws.root = std::move(*updated);
                return detail::finalize(std::move(m));
            }
        }
        return state ? state : detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState focusPane(const ModelState& state, PaneId leafId)
    {
        auto m = detail::copyOf(state);
        const auto wsIdxOpt = detail::findWorkspaceIndexForLeaf(m, leafId);
        if (!wsIdxOpt.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto wsIdx = *wsIdxOpt;
        auto& ws = m.workspaces[wsIdx];
        ws.activePaneId = leafId;
        m.activeWorkspaceId = ws.id;
        detail::mruTouch(m.mru, ws.id);
        return detail::finalize(std::move(m));
    }
}
