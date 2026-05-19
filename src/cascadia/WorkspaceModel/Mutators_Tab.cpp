// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tab mutators: newTab, closeTab, closeTabsRight, closeOtherTabs,
// selectTab, setTabTitle, setTabColor, setTabPinned.
//
// closeTab is the centrepiece of the cascade-rule logic. Every other
// "close-style" mutator (closePane, moveTab's source side, closeTabsRight,
// closeOtherTabs) collapses to a leaf-rewrite of the same shape:
//
//   1. Find the leaf containing the tab.
//   2. Build a new leaf with the tab removed (or std::nullopt if removing
//      it would make the leaf empty).
//   3. Cascade up: if the leaf disappears, its parent split collapses to
//      the surviving sibling. If the workspace's root pane is gone, the
//      workspace is removed (with the usual MRU + activeWorkspaceId
//      fallback).

#include "pch.h"

#include "MutatorHelpers.h"
#include "Mutators.h"

namespace WorkspaceModel
{
    namespace
    {
        using namespace detail;

        // Apply a leaf-transform to the leaf with id `leafId` in the
        // workspace at index `wsIdx`. If the transform returns nullopt
        // (i.e. leaf disappears), the cascade is allowed to remove the
        // whole workspace if its root pane is gone.
        //
        // After applying the transform, this helper also fixes up:
        //   - The workspace's activePaneId if the active leaf was rewritten
        //     or removed (falls back to the first remaining leaf).
        //   - The model's activeWorkspaceId + MRU if the workspace was
        //     removed entirely (falls back to MRU-next, or nullopt).
        //
        // The caller is responsible for any further fixups (e.g. setting
        // a specific activePaneId/activeTabIdx after a move).
        void applyLeafTransformAndCascade(WorkspaceModelData& m,
                                          std::size_t wsIdx,
                                          PaneId leafId,
                                          const LeafTransform& transform)
        {
            auto& ws = m.workspaces[wsIdx];
            const auto wasActiveLeaf = (ws.activePaneId == leafId);

            bool found = false;
            auto newRoot = detail::transformLeaf(ws.root, leafId, transform, found);

            if (!found)
            {
                // Leaf wasn't located — leave the workspace untouched.
                return;
            }

            if (!newRoot.has_value())
            {
                // The entire workspace's root pane is gone. Remove the
                // workspace; MRU + active fall back accordingly.
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
                return;
            }

            // Workspace survives; install the rewritten root.
            ws.root = std::move(*newRoot);

            // If the active leaf was the one we rewrote (or removed via
            // cascade-collapse), repair activePaneId to point at the
            // first surviving leaf.
            bool activeStillPresent = false;
            if (ws.activePaneId.valid())
            {
                if (detail::findLeaf(ws.root, ws.activePaneId) != nullptr)
                {
                    activeStillPresent = true;
                }
            }
            if (!activeStillPresent || wasActiveLeaf)
            {
                if (wasActiveLeaf)
                {
                    // If the leaf still exists (e.g. closeTabsRight kept
                    // it), prefer keeping the active focus on that leaf.
                    if (auto* survived = detail::findLeaf(ws.root, leafId))
                    {
                        ws.activePaneId = survived->id;
                    }
                    else if (const auto* fallback = detail::firstLeaf(ws.root))
                    {
                        ws.activePaneId = fallback->id;
                    }
                }
                else if (const auto* fallback = detail::firstLeaf(ws.root))
                {
                    ws.activePaneId = fallback->id;
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    NewTabResult newTab(const ModelState& state,
                        WorkspaceId workspaceId,
                        PaneId leafId,
                        TabContent description,
                        std::string customTitle,
                        std::optional<Color> runtimeColor,
                        bool pinned)
    {
        auto m = detail::copyOf(state);

        auto* ws = detail::findWorkspace(m, workspaceId);
        if (!ws)
        {
            return NewTabResult{ state ? state : detail::finalize(std::move(m)), TabId{ 0 } };
        }
        if (!detail::findLeaf(ws->root, leafId))
        {
            return NewTabResult{ state ? state : detail::finalize(std::move(m)), TabId{ 0 } };
        }

        const auto tabId = TabId{ detail::allocId(m.idCounter) };
        TabRecord t;
        t.id = tabId;
        t.description = std::move(description);
        t.customTitle = std::move(customTitle);
        t.runtimeColor = runtimeColor;
        t.pinned = pinned;

        // Re-acquire ws (allocId didn't move workspaces, but be safe).
        auto* ws2 = detail::findWorkspace(m, workspaceId);

        bool found = false;
        auto newRoot = detail::transformLeaf(
            ws2->root,
            leafId,
            [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                LeafPane out = leaf;
                out.tabs.push_back(t);
                out.activeTabIdx = out.tabs.size() - 1; // new tab becomes active
                return out;
            },
            found);
        if (found && newRoot.has_value())
        {
            ws2->root = std::move(*newRoot);
            ws2->activePaneId = leafId;
        }

        return NewTabResult{ detail::finalize(std::move(m)), tabId };
    }

    // ---------------------------------------------------------------------
    ModelState closeTab(const ModelState& state, TabId id)
    {
        auto m = detail::copyOf(state);

        const auto loc = detail::findTabLocationByIndex(m, id);
        if (!loc.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto wsIdx = loc->workspaceIdx;
        const auto leafId = loc->leafId;
        const auto tabIdx = loc->indexInLeaf;

        applyLeafTransformAndCascade(
            m,
            wsIdx,
            leafId,
            [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                if (leaf.tabs.size() <= 1)
                {
                    // Removing the last tab kills the leaf.
                    return std::nullopt;
                }
                LeafPane out = leaf;
                out.tabs.erase(out.tabs.begin() + static_cast<std::ptrdiff_t>(tabIdx));
                // Shift activeTabIdx: if the closed tab was to the left
                // of (or at) activeTabIdx, decrement it; clamp to last.
                if (tabIdx < out.activeTabIdx)
                {
                    out.activeTabIdx -= 1;
                }
                else if (tabIdx == out.activeTabIdx)
                {
                    // Keep idx; clamp if past end.
                    if (out.activeTabIdx >= out.tabs.size())
                    {
                        out.activeTabIdx = out.tabs.size() - 1;
                    }
                }
                return out;
            });

        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState closeTabsRight(const ModelState& state, TabId id)
    {
        auto m = detail::copyOf(state);
        const auto loc = detail::findTabLocationByIndex(m, id);
        if (!loc.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto wsIdx = loc->workspaceIdx;
        const auto leafId = loc->leafId;
        const auto keepIdx = loc->indexInLeaf;

        applyLeafTransformAndCascade(
            m,
            wsIdx,
            leafId,
            [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                LeafPane out = leaf;
                if (keepIdx + 1 < out.tabs.size())
                {
                    out.tabs.erase(out.tabs.begin() + static_cast<std::ptrdiff_t>(keepIdx + 1),
                                   out.tabs.end());
                }
                if (out.activeTabIdx >= out.tabs.size())
                {
                    out.activeTabIdx = out.tabs.empty() ? 0 : out.tabs.size() - 1;
                }
                return out; // never empty (the named tab itself stays)
            });
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState closeOtherTabs(const ModelState& state, TabId id)
    {
        auto m = detail::copyOf(state);
        const auto loc = detail::findTabLocationByIndex(m, id);
        if (!loc.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto wsIdx = loc->workspaceIdx;
        const auto leafId = loc->leafId;
        const auto keepIdx = loc->indexInLeaf;

        applyLeafTransformAndCascade(
            m,
            wsIdx,
            leafId,
            [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                LeafPane out = leaf;
                auto kept = out.tabs[keepIdx];
                out.tabs.clear();
                out.tabs.push_back(std::move(kept));
                out.activeTabIdx = 0;
                return out;
            });
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState selectTab(const ModelState& state, TabId id)
    {
        auto m = detail::copyOf(state);
        const auto loc = detail::findTabLocationByIndex(m, id);
        if (!loc.has_value())
        {
            return state ? state : detail::finalize(std::move(m));
        }
        const auto wsIdx = loc->workspaceIdx;
        const auto leafId = loc->leafId;
        const auto tabIdx = loc->indexInLeaf;

        auto& ws = m.workspaces[wsIdx];
        bool found = false;
        auto newRoot = detail::transformLeaf(
            ws.root,
            leafId,
            [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                LeafPane out = leaf;
                out.activeTabIdx = tabIdx;
                return out;
            },
            found);
        if (found && newRoot.has_value())
        {
            ws.root = std::move(*newRoot);
            ws.activePaneId = leafId;
        }
        m.activeWorkspaceId = ws.id;
        detail::mruTouch(m.mru, ws.id);
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    namespace
    {
        // Helper for the simple "find tab by id, mutate the TabRecord
        // in place" pattern shared by setTabTitle / setTabColor /
        // setTabPinned.
        template<typename Fn>
        [[nodiscard]] ModelState updateTab(const ModelState& state, TabId id, Fn&& fn)
        {
            auto m = detail::copyOf(state);
            const auto loc = detail::findTabLocationByIndex(m, id);
            if (!loc.has_value())
            {
                return state ? state : detail::finalize(std::move(m));
            }
            const auto wsIdx = loc->workspaceIdx;
            const auto leafId = loc->leafId;
            const auto tabIdx = loc->indexInLeaf;

            auto& ws = m.workspaces[wsIdx];
            bool found = false;
            auto newRoot = detail::transformLeaf(
                ws.root,
                leafId,
                [&](const LeafPane& leaf) -> std::optional<LeafPane> {
                    LeafPane out = leaf;
                    fn(out.tabs[tabIdx]);
                    return out;
                },
                found);
            if (found && newRoot.has_value())
            {
                ws.root = std::move(*newRoot);
            }
            return detail::finalize(std::move(m));
        }
    }

    ModelState setTabTitle(const ModelState& state, TabId id, std::string customTitle)
    {
        return updateTab(state, id, [&](TabRecord& t) { t.customTitle = std::move(customTitle); });
    }

    ModelState setTabColor(const ModelState& state, TabId id, std::optional<Color> color)
    {
        return updateTab(state, id, [&](TabRecord& t) { t.runtimeColor = color; });
    }

    ModelState setTabPinned(const ModelState& state, TabId id, bool pinned)
    {
        return updateTab(state, id, [&](TabRecord& t) { t.pinned = pinned; });
    }
}
