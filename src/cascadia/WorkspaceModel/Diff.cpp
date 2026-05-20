// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// diff(prev, next) — the pure diff function.
//
// EMIT ORDERING
// =============
// The output is partitioned into three phases so a view can apply changes
// sequentially without worrying about ordering hazards (e.g. setting the
// active tab on a pane before that pane has been created):
//
//   Phase 1 (additive):
//     WorkspaceAdded, LeafPaneCreated, SplitPaneCreated, TabAdded,
//     ContentMounted
//   Phase 2 (intra-existing mutation):
//     TabMoved, SplitRatioChanged, TabDecorationUpdated, ActiveTabChanged,
//     ActiveWorkspaceChanged
//   Phase 3 (subtractive):
//     ContentUnmounted, TabRemoved, SplitPaneCollapsed, WorkspaceRemoved
//
// Within Phase 1 we emit workspace events before pane events before tab
// events before mount events, and we emit Created events for ancestors
// before descendants when both are new in the same workspace — so a view
// can blindly process changes in order.
//
// Within Phase 3 we emit content unmounts before tab removes before
// split collapses before workspace removes — the inverse of Phase 1.
//
// IDENTITY-KEYED MATCHING
// ========================
// Tabs, panes, and workspaces are compared by their strong-typed id
// across (prev, next):
//
//   - WorkspaceId in both → diff its contents.
//   - WorkspaceId in next only → WorkspaceAdded.
//   - WorkspaceId in prev only → WorkspaceRemoved (omits per-leaf
//                                 TabRemoved/SplitPaneCollapsed; the
//                                 view drops the whole workspace).
//
//   - PaneId in both as same kind (leaf/leaf or split/split) → diff
//                                 internals (ratio, tabs, etc.).
//   - PaneId in next only → LeafPaneCreated or SplitPaneCreated.
//   - PaneId in prev only as a SplitPane → SplitPaneCollapsed{ removedSplit, survivor }.
//   - PaneId in prev only as a LeafPane within a surviving workspace
//                                  → its tabs are emitted as TabRemoved;
//                                    the parent split's collapse is what
//                                    structurally drops the leaf.
//
//   - TabId in both at same (leafId, idx) → maybe TabDecorationUpdated if
//                                            customTitle/runtimeColor/pinned
//                                            changed.
//   - TabId in both at different (leafId, idx) → TabMoved. (Critical for
//                                                 preserving live XAML
//                                                 state across reorder /
//                                                 cross-leaf / cross-
//                                                 workspace move.)
//   - TabId in next only → TabAdded.
//   - TabId in prev only (and its workspace survives) → TabRemoved.
//
// SPLIT PRESERVATION UNDER WRAP
// ==============================
// When the `splitPane` action wraps a leaf, the original leaf's PaneId is
// preserved. diff observes:
//   prev: leafL exists at root.
//   next: leafL exists nested under new splitS (also a new sibling
//         leafR introduced).
// It emits:
//   SplitPaneCreated(splitS, …)   — new split id, new ratio/axis.
//   LeafPaneCreated(leafR, …)     — new sibling leaf.
//   TabAdded for each tab in leafR.
//   ContentMounted for the new sibling's tabs (if they have mounts).
// It does NOT emit any Created/Removed/Added events for leafL or its
// tabs; they are preserved across the wrap.

#include "pch.h"

#include "Diff.h"

#include "Cascade.h"
#include "IWorkspaceView.h"

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace WorkspaceModel
{
    namespace
    {
        // -----------------------------------------------------------------
        // Identity tables built once per state.
        // -----------------------------------------------------------------

        struct PaneInfo
        {
            // If this PaneId belongs to a SplitPane, splitNode != nullptr.
            // If it belongs to a LeafPane, leafNode != nullptr.
            const SplitPane* splitNode{ nullptr };
            const LeafPane* leafNode{ nullptr };
            // Workspace this pane lives in.
            WorkspaceId workspaceId{};
            // The PaneId of the parent SplitPane, or nullopt for the
            // workspace root.
            std::optional<PaneId> parent{};
        };

        struct TabInfo
        {
            const TabRecord* record{ nullptr };
            // Leaf containing this tab.
            PaneId leafId{};
            // Index inside the leaf.
            std::size_t indexInLeaf{ 0 };
        };

        // Recursive helper: collect every PaneId in a subtree into `paneTable`
        // and every TabId into `tabTable`.
        void indexSubtree(const PaneNode& node,
                          WorkspaceId workspaceId,
                          std::optional<PaneId> parent,
                          std::unordered_map<PaneId, PaneInfo>& paneTable,
                          std::unordered_map<TabId, TabInfo>& tabTable)
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                PaneInfo info;
                info.leafNode = leaf;
                info.workspaceId = workspaceId;
                info.parent = parent;
                paneTable[leaf->id] = info;

                for (std::size_t i = 0; i < leaf->tabs.size(); ++i)
                {
                    TabInfo ti;
                    ti.record = &leaf->tabs[i];
                    ti.leafId = leaf->id;
                    ti.indexInLeaf = i;
                    tabTable[leaf->tabs[i].id] = ti;
                }
                return;
            }
            const auto& split = std::get<SplitPane>(node);
            PaneInfo info;
            info.splitNode = &split;
            info.workspaceId = workspaceId;
            info.parent = parent;
            paneTable[split.id] = info;

            if (split.left)
            {
                indexSubtree(*split.left, workspaceId, split.id, paneTable, tabTable);
            }
            if (split.right)
            {
                indexSubtree(*split.right, workspaceId, split.id, paneTable, tabTable);
            }
        }

        struct StateIndex
        {
            // workspaceId → WorkspaceState*
            std::unordered_map<WorkspaceId, const WorkspaceState*> workspaceById{};
            // PaneId → info (workspace, parent, kind)
            std::unordered_map<PaneId, PaneInfo> panes{};
            // TabId → info (leaf, index, record)
            std::unordered_map<TabId, TabInfo> tabs{};
        };

        [[nodiscard]] StateIndex buildIndex(const WorkspaceModelData* m)
        {
            StateIndex idx;
            if (!m)
            {
                return idx;
            }
            idx.workspaceById.reserve(m->workspaces.size());
            for (std::size_t i = 0; i < m->workspaces.size(); ++i)
            {
                const auto& ws = m->workspaces[i];
                idx.workspaceById[ws.id] = &ws;
                indexSubtree(ws.root, ws.id, std::nullopt, idx.panes, idx.tabs);
            }
            return idx;
        }

        // True if the PaneInfo describes a SplitPane.
        [[nodiscard]] bool isSplit(const PaneInfo& p) noexcept
        {
            return p.splitNode != nullptr;
        }

        // True if the PaneInfo describes a LeafPane.
        [[nodiscard]] bool isLeaf(const PaneInfo& p) noexcept
        {
            return p.leafNode != nullptr;
        }

        // Emit Created events for every pane in `subtree` that is NOT
        // present in prevIndex.panes. Walks parent-before-child order.
        // Also emits TabAdded for every new tab encountered and
        // ContentMounted for any already-mounted tab in the new subtree.
        void emitCreatesForNewSubtree(const PaneNode& subtree,
                                      const StateIndex& prevIndex,
                                      std::optional<ParentSplit> parentInNext,
                                      std::vector<WorkspaceChange>& out)
        {
            if (const auto* leaf = std::get_if<LeafPane>(&subtree))
            {
                // Only emit LeafPaneCreated if the leaf id is new in next.
                if (prevIndex.panes.find(leaf->id) == prevIndex.panes.end())
                {
                    out.push_back(LeafPaneCreated{ leaf->id, parentInNext });
                    // All of this leaf's tabs are necessarily new (no
                    // prev-side leaf to host them). We still gate per-tab
                    // because a tab could in theory have been moved here
                    // from elsewhere — but that case is handled later in
                    // the TabMoved pass, so we skip those here.
                    // We intentionally don't emit TabAdded from this helper;
                    // tab emissions are handled separately so TabMoved
                    // detection runs first.
                }
                return;
            }
            const auto& split = std::get<SplitPane>(subtree);
            if (prevIndex.panes.find(split.id) == prevIndex.panes.end())
            {
                PaneId leftId{ 0 };
                PaneId rightId{ 0 };
                if (split.left)
                {
                    if (const auto* l = std::get_if<LeafPane>(split.left.get()))
                    {
                        leftId = l->id;
                    }
                    else
                    {
                        leftId = std::get<SplitPane>(*split.left).id;
                    }
                }
                if (split.right)
                {
                    if (const auto* r = std::get_if<LeafPane>(split.right.get()))
                    {
                        rightId = r->id;
                    }
                    else
                    {
                        rightId = std::get<SplitPane>(*split.right).id;
                    }
                }
                out.push_back(SplitPaneCreated{ split.id, split.axis, split.ratio, leftId, rightId });
            }
            const ParentSplit childParent{ split.id, split.axis, split.ratio };
            if (split.left)
            {
                emitCreatesForNewSubtree(*split.left, prevIndex, childParent, out);
            }
            if (split.right)
            {
                emitCreatesForNewSubtree(*split.right, prevIndex, childParent, out);
            }
        }

        // For each SplitPane that exists in both states with a different
        // ratio, emit SplitRatioChanged.
        void emitSplitRatioChanges(const StateIndex& prevIndex,
                                   const StateIndex& nextIndex,
                                   std::vector<WorkspaceChange>& out)
        {
            for (const auto& [pid, info] : nextIndex.panes)
            {
                if (!isSplit(info))
                {
                    continue;
                }
                auto it = prevIndex.panes.find(pid);
                if (it == prevIndex.panes.end() || !isSplit(it->second))
                {
                    continue;
                }
                if (it->second.splitNode->ratio != info.splitNode->ratio)
                {
                    out.push_back(SplitRatioChanged{ pid, info.splitNode->ratio });
                }
            }
        }

        // Detect every SplitPaneCollapsed. A split collapses when its
        // PaneId is in prev but absent in next, AND one of its prev
        // children survives in next within the same workspace.
        // Workspaces removed entirely are skipped (the WorkspaceRemoved
        // op handles them).
        void emitCollapses(const WorkspaceModelData* prev,
                           const StateIndex& prevIndex,
                           const StateIndex& nextIndex,
                           std::vector<WorkspaceChange>& out)
        {
            if (!prev)
            {
                return;
            }
            for (const auto& [pid, info] : prevIndex.panes)
            {
                if (!isSplit(info))
                {
                    continue;
                }
                if (nextIndex.panes.find(pid) != nextIndex.panes.end())
                {
                    continue; // split still exists in next
                }
                // Skip if the containing workspace is gone in next; the
                // WorkspaceRemoved op tears the entire tree down.
                if (nextIndex.workspaceById.find(info.workspaceId) ==
                    nextIndex.workspaceById.end())
                {
                    continue;
                }

                // Which child survived? Walk the prev split's children
                // and find one whose PaneId is still in nextIndex.panes.
                auto idOf = [](const PaneNode& n) -> PaneId {
                    if (const auto* leaf = std::get_if<LeafPane>(&n))
                    {
                        return leaf->id;
                    }
                    return std::get<SplitPane>(n).id;
                };
                PaneId survivor{ 0 };
                if (info.splitNode->left)
                {
                    const auto lid = idOf(*info.splitNode->left);
                    if (nextIndex.panes.find(lid) != nextIndex.panes.end())
                    {
                        survivor = lid;
                    }
                }
                if (!survivor.valid() && info.splitNode->right)
                {
                    const auto rid = idOf(*info.splitNode->right);
                    if (nextIndex.panes.find(rid) != nextIndex.panes.end())
                    {
                        survivor = rid;
                    }
                }
                // If neither child survived, the whole subtree
                // disappeared. In that case the collapse op is still
                // emitted with survivor=PaneId{0} — but in the current
                // model that situation only arises when the workspace
                // is gone, which we filtered out above. We still guard:
                if (!survivor.valid())
                {
                    // Unusual but legal: emit with PaneId{0}. The
                    // renderer can treat it as "drop this split entirely
                    // and don't graft anything in its place".
                    out.push_back(SplitPaneCollapsed{ pid, PaneId{ 0 } });
                    continue;
                }
                out.push_back(SplitPaneCollapsed{ pid, survivor });
            }
        }

        // Emit TabAdded for every TabId that is new in next AND whose leaf
        // exists in next. Skips tabs that are also present in prev (those
        // are TabMoved or unchanged).
        void emitTabAdds(const WorkspaceModelData* next,
                         const StateIndex& prevIndex,
                         const StateIndex& nextIndex,
                         std::vector<WorkspaceChange>& out)
        {
            if (!next)
            {
                return;
            }
            for (const auto& ws : next->workspaces)
            {
                std::vector<const LeafPane*> leaves;
                detail::collectLeaves(ws.root, leaves);
                for (const auto* leaf : leaves)
                {
                    // A leaf nested under a SplitPane carries a parent in the
                    // next-state index; the view treats that tab's classic
                    // materialisation as already driven by LeafPaneCreated.
                    bool leafInsideSplit = false;
                    if (const auto paneIt = nextIndex.panes.find(leaf->id);
                        paneIt != nextIndex.panes.end())
                    {
                        leafInsideSplit = paneIt->second.parent.has_value();
                    }
                    for (std::size_t i = 0; i < leaf->tabs.size(); ++i)
                    {
                        const auto& t = leaf->tabs[i];
                        if (prevIndex.tabs.find(t.id) != prevIndex.tabs.end())
                        {
                            continue; // existing tab, handled elsewhere
                        }
                        out.push_back(TabAdded{
                            leaf->id,
                            i,
                            t.id,
                            t.customTitle,
                            t.runtimeColor,
                            t.pinned,
                            t.description,
                            leafInsideSplit,
                            ws.id });
                    }
                }
            }
        }

        // Emit TabRemoved for every TabId that was in prev but is gone in
        // next AND whose workspace is still present in next. (If the
        // workspace itself was removed, WorkspaceRemoved replaces these.)
        void emitTabRemoves(const StateIndex& prevIndex,
                            const StateIndex& nextIndex,
                            std::vector<WorkspaceChange>& out)
        {
            for (const auto& [tid, info] : prevIndex.tabs)
            {
                if (nextIndex.tabs.find(tid) != nextIndex.tabs.end())
                {
                    continue; // still around
                }
                // Find what workspace this tab was in via its leaf.
                auto paneIt = prevIndex.panes.find(info.leafId);
                if (paneIt == prevIndex.panes.end())
                {
                    continue;
                }
                if (nextIndex.workspaceById.find(paneIt->second.workspaceId) ==
                    nextIndex.workspaceById.end())
                {
                    continue; // workspace gone — WorkspaceRemoved handles
                }
                out.push_back(TabRemoved{ info.leafId, tid });
            }
        }

        // Emit TabMoved for every TabId present in both states whose
        // (leafId, indexInLeaf) differs.
        void emitTabMoves(const StateIndex& prevIndex,
                          const StateIndex& nextIndex,
                          std::vector<WorkspaceChange>& out)
        {
            for (const auto& [tid, nextInfo] : nextIndex.tabs)
            {
                auto prevIt = prevIndex.tabs.find(tid);
                if (prevIt == prevIndex.tabs.end())
                {
                    continue; // new tab → TabAdded handled elsewhere
                }
                const auto& prevInfo = prevIt->second;
                if (prevInfo.leafId == nextInfo.leafId &&
                    prevInfo.indexInLeaf == nextInfo.indexInLeaf)
                {
                    continue; // not moved
                }
                out.push_back(TabMoved{
                    tid,
                    prevInfo.leafId,
                    nextInfo.leafId,
                    nextInfo.indexInLeaf });
            }
        }

        // Emit TabDecorationUpdated for every TabId present in both states
        // where customTitle / runtimeColor / pinned changed and the
        // tab's location is unchanged. (When the location changed we
        // emit TabMoved; if decoration also changed, a later
        // TabDecorationUpdated is still emitted so the renderer can apply
        // both — the order Move-then-Update is fine because XAML state is
        // preserved across TabMoved.)
        void emitDecorationUpdates(const StateIndex& prevIndex,
                                   const StateIndex& nextIndex,
                                   std::vector<WorkspaceChange>& out)
        {
            for (const auto& [tid, nextInfo] : nextIndex.tabs)
            {
                auto prevIt = prevIndex.tabs.find(tid);
                if (prevIt == prevIndex.tabs.end())
                {
                    continue;
                }
                const auto* a = prevIt->second.record;
                const auto* b = nextInfo.record;
                if (!a || !b)
                {
                    continue;
                }
                if (a->customTitle != b->customTitle ||
                    a->runtimeColor != b->runtimeColor ||
                    a->pinned != b->pinned)
                {
                    // Carry the owning workspace's stable id so the view can
                    // route the decoration through its own id->XAML resolver;
                    // no positional display-index projection happens here.
                    WorkspaceId workspaceId{};
                    if (const auto leafIt = nextIndex.panes.find(nextInfo.leafId);
                        leafIt != nextIndex.panes.end())
                    {
                        workspaceId = leafIt->second.workspaceId;
                    }
                    out.push_back(TabDecorationUpdated{
                        tid,
                        b->customTitle,
                        b->runtimeColor,
                        b->pinned,
                        workspaceId });
                }
            }
        }

        // Mount / unmount diffs. A change in ContentId is treated as
        // Unmount(old) + Mount(new); a nullopt → ContentId is Mount; a
        // ContentId → nullopt is Unmount.
        void emitMountOps(const StateIndex& prevIndex,
                          const StateIndex& nextIndex,
                          std::vector<WorkspaceChange>& outMounts,
                          std::vector<WorkspaceChange>& outUnmounts)
        {
            // Mounts: any tab in next whose mount differs from its
            // previous value (or the tab itself is new).
            for (const auto& [tid, nextInfo] : nextIndex.tabs)
            {
                if (!nextInfo.record)
                {
                    continue;
                }
                const auto& nb = *nextInfo.record;
                std::optional<ContentId> prevMount{};
                auto prevIt = prevIndex.tabs.find(tid);
                if (prevIt != prevIndex.tabs.end() && prevIt->second.record)
                {
                    prevMount = prevIt->second.record->mount;
                }
                if (nb.mount.has_value() && prevMount != nb.mount)
                {
                    outMounts.push_back(ContentMounted{ tid, *nb.mount, nb.description });
                }
                if (prevMount.has_value() && prevMount != nb.mount)
                {
                    // Mount changed (rare) or was set and is now unset.
                    outUnmounts.push_back(ContentUnmounted{ tid, *prevMount });
                }
            }
            // Unmounts: tabs gone from next that still had a mount in prev,
            // when the workspace survives in next.
            for (const auto& [tid, prevInfo] : prevIndex.tabs)
            {
                if (nextIndex.tabs.find(tid) != nextIndex.tabs.end())
                {
                    continue;
                }
                if (!prevInfo.record || !prevInfo.record->mount.has_value())
                {
                    continue;
                }
                auto paneIt = prevIndex.panes.find(prevInfo.leafId);
                if (paneIt == prevIndex.panes.end())
                {
                    continue;
                }
                if (nextIndex.workspaceById.find(paneIt->second.workspaceId) ==
                    nextIndex.workspaceById.end())
                {
                    // workspace gone; whole-workspace teardown handles it
                    continue;
                }
                outUnmounts.push_back(ContentUnmounted{ tid, *prevInfo.record->mount });
            }
        }

        // ActiveTabChanged: per-leaf activeTabIdx changed AND the leaf exists
        // in both states. New leaves carry their initial active tab idx
        // only when it's non-zero (the renderer's default for a freshly
        // materialised leaf is activeTabIdx == 0; emitting a redundant
        // ActiveTabChanged op for the zero case would just be noise).
        void emitActiveTabChanges(const WorkspaceModelData* next,
                                  const StateIndex& prevIndex,
                                  std::vector<WorkspaceChange>& out)
        {
            if (!next)
            {
                return;
            }
            for (const auto& ws : next->workspaces)
            {
                std::vector<const LeafPane*> leaves;
                detail::collectLeaves(ws.root, leaves);
                for (const auto* leaf : leaves)
                {
                    auto prevIt = prevIndex.panes.find(leaf->id);
                    if (prevIt == prevIndex.panes.end() || !isLeaf(prevIt->second))
                    {
                        // Brand-new leaf. Emit ActiveTabChanged only if the
                        // initial active idx isn't the default 0.
                        if (!leaf->tabs.empty() && leaf->activeTabIdx != 0)
                        {
                            out.push_back(ActiveTabChanged{ leaf->id, leaf->activeTabIdx });
                        }
                        continue;
                    }
                    if (prevIt->second.leafNode->activeTabIdx != leaf->activeTabIdx)
                    {
                        out.push_back(ActiveTabChanged{ leaf->id, leaf->activeTabIdx });
                    }
                }
            }
        }
    } // namespace

    // ---------------------------------------------------------------------
    std::vector<WorkspaceChange> diff(const ModelState& prevState,
                                      const ModelState& nextState)
    {
        const WorkspaceModelData* prev = prevState ? prevState.get() : nullptr;
        const WorkspaceModelData* next = nextState ? nextState.get() : nullptr;

        const StateIndex prevIndex = buildIndex(prev);
        const StateIndex nextIndex = buildIndex(next);

        // Three phase buckets so we can sequence them at the end.
        std::vector<WorkspaceChange> additive;
        std::vector<WorkspaceChange> mutation;
        std::vector<WorkspaceChange> subtractive;

        // -------- Phase 1: additive --------

        // WorkspaceAdded for every workspace in next not in prev.
        if (next)
        {
            for (std::size_t i = 0; i < next->workspaces.size(); ++i)
            {
                const auto& ws = next->workspaces[i];
                if (prevIndex.workspaceById.find(ws.id) == prevIndex.workspaceById.end())
                {
                    // Carry the stable id only; the view resolves the
                    // display/insertion position from the id.
                    additive.push_back(WorkspaceAdded{ ws.id, ws.name, ws.color });
                }
            }
        }

        // LeafPaneCreated / SplitPaneCreated for every new pane in surviving
        // (or new) workspaces.
        if (next)
        {
            for (const auto& ws : next->workspaces)
            {
                emitCreatesForNewSubtree(ws.root, prevIndex, std::nullopt, additive);
            }
        }

        // TabAdded for every new tab.
        emitTabAdds(next, prevIndex, nextIndex, additive);

        // ContentMounted: collected jointly with unmounts below to keep
        // logic in one place, but mount entries go in `additive`.
        std::vector<WorkspaceChange> mountsBucket;
        std::vector<WorkspaceChange> unmountsBucket;
        emitMountOps(prevIndex, nextIndex, mountsBucket, unmountsBucket);
        for (auto& op : mountsBucket)
        {
            additive.push_back(std::move(op));
        }

        // -------- Phase 2: mutations of existing entities --------

        emitTabMoves(prevIndex, nextIndex, mutation);
        emitSplitRatioChanges(prevIndex, nextIndex, mutation);
        emitDecorationUpdates(prevIndex, nextIndex, mutation);
        emitActiveTabChanges(next, prevIndex, mutation);

        // ActiveWorkspaceChanged — emit when value changed (including
        // nullopt → something or something → nullopt).
        {
            std::optional<WorkspaceId> prevActive = prev ? prev->activeWorkspaceId : std::nullopt;
            std::optional<WorkspaceId> nextActive = next ? next->activeWorkspaceId : std::nullopt;
            if (prevActive != nextActive)
            {
                // Carry the stable id only; the view resolves it to the
                // classic tab to select through its own id->XAML resolver.
                mutation.push_back(ActiveWorkspaceChanged{ nextActive });
            }
        }

        // -------- Phase 3: subtractive --------

        // Unmounts first so the view can detach live content before
        // the tab structurally disappears.
        for (auto& change : unmountsBucket)
        {
            subtractive.push_back(std::move(change));
        }

        // TabRemoved for tabs gone from surviving workspaces.
        emitTabRemoves(prevIndex, nextIndex, subtractive);

        // SplitPaneCollapsed for splits that disappeared while their
        // workspace survived.
        emitCollapses(prev, prevIndex, nextIndex, subtractive);

        // WorkspaceRemoved for every workspace in prev not in next.
        if (prev)
        {
            for (const auto& ws : prev->workspaces)
            {
                if (nextIndex.workspaceById.find(ws.id) == nextIndex.workspaceById.end())
                {
                    subtractive.push_back(WorkspaceRemoved{ ws.id });
                }
            }
        }

        // Concatenate the three phases.
        std::vector<WorkspaceChange> out;
        out.reserve(additive.size() + mutation.size() + subtractive.size());
        for (auto& op : additive)
        {
            out.push_back(std::move(op));
        }
        for (auto& op : mutation)
        {
            out.push_back(std::move(op));
        }
        for (auto& op : subtractive)
        {
            out.push_back(std::move(op));
        }
        return out;
    }

    // ---------------------------------------------------------------------
    void applyChanges(IWorkspaceView& view, std::span<const WorkspaceChange> changes)
    {
        for (const auto& change : changes)
        {
            std::visit(
                [&view](const auto& arm) { view.apply(arm); },
                change);
        }
    }
}
