// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tiny private helpers shared across all the Actions_*.cpp files.
//
// This header is intentionally internal — it sits inside the
// WorkspaceModel namespace alongside the public actions but only the
// implementation files include it. Callers of the library reach for
// WorkspaceActions.h (or the helpers in Cascade.h if they're doing
// structural rebuilds) instead.

#pragma once

#include "WorkspaceActions.h"
#include "Validator.h"
#include "Cascade.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <variant>

namespace WorkspaceModel::detail
{
    // Deep-mutable copy of the state's underlying data. Actions build a
    // new WorkspaceModelData on top of this copy and wrap it back into a
    // shared_ptr with `finalize()`. A null input is treated as an empty
    // model.
    [[nodiscard]] inline WorkspaceModelData copyOf(const ModelState& state)
    {
        if (!state)
        {
            return WorkspaceModelData{};
        }
        return *state;
    }

    // -------------------------------------------------------------------
    // Mount policy (Phase 2 Slice 1, #44/#45 follow-up)
    // -------------------------------------------------------------------
    //
    // CONTRACT (option I — "lifetime / materialised mount"):
    //   A TabRecord.mount is a *lifetime* ContentId. It is allocated lazily
    //   the first time a tab needs to be live — i.e. when it is the active
    //   tab of its leaf inside the workspace that is currently active — and
    //   is then NEVER cleared while the tab survives. (Removing the tab is
    //   the only thing that drops it.) Once allocated, the ContentId is the
    //   same value for the rest of the tab's life: a switch away from a
    //   workspace and back, or a tab switch within a leaf and back, never
    //   reallocates it, so the view's EnsureMounted resolves the SAME live
    //   IPaneContent (ConPTY / scrollback survive the detach).
    //
    //   This is the minimal-machinery option that satisfies the survival
    //   requirement with no new TabRecord field: a switch is non-structural
    //   and changes no mount, so it diffs to a single ActiveWorkspaceChanged
    //   while the registry keeps every previously-materialised content alive.
    //
    // applyMountPolicy is the ONLY writer of TabRecord.mount. It runs at the
    // end of every action (via finalize) so the model-wide invariant — every
    // active-workspace active tab is materialised — holds after every action,
    // including the close/move cascades that re-pick an active tab.
    namespace mount_detail
    {
        // Ensure the active tab of `leaf` carries a mount, allocating a
        // fresh lifetime ContentId from `counter` if it has none. Returns a
        // rebuilt leaf when a mount was newly assigned, std::nullopt when
        // the leaf was already materialised (so the caller can avoid a
        // needless copy / rebuild higher up).
        [[nodiscard]] inline std::optional<LeafPane> materialiseActiveTab(const LeafPane& leaf,
                                                                          std::uint64_t& counter)
        {
            if (leaf.tabs.empty() || leaf.activeTabIdx >= leaf.tabs.size())
            {
                return std::nullopt; // malformed leaf; validator handles it
            }
            if (leaf.tabs[leaf.activeTabIdx].mount.has_value())
            {
                return std::nullopt; // already materialised — nothing to do
            }
            LeafPane out = leaf;
            out.tabs[out.activeTabIdx].mount = ContentId{ ++counter };
            return out;
        }

        // Walk `node`, materialising every leaf's active tab. Returns a
        // rebuilt subtree when anything changed, std::nullopt otherwise so
        // unchanged substructure is shared (copy-on-write, mirroring
        // Cascade.cpp's transformLeaf).
        [[nodiscard]] inline std::optional<PaneNode> materialiseSubtree(const PaneNode& node,
                                                                        std::uint64_t& counter)
        {
            if (const auto* leaf = std::get_if<LeafPane>(&node))
            {
                if (auto rebuilt = materialiseActiveTab(*leaf, counter))
                {
                    return PaneNode{ std::move(*rebuilt) };
                }
                return std::nullopt;
            }

            const auto& split = std::get<SplitPane>(node);
            std::optional<PaneNode> newLeft =
                split.left ? materialiseSubtree(*split.left, counter) : std::nullopt;
            std::optional<PaneNode> newRight =
                split.right ? materialiseSubtree(*split.right, counter) : std::nullopt;
            if (!newLeft && !newRight)
            {
                return std::nullopt; // nothing changed in this subtree
            }
            SplitPane rebuilt = split;
            if (newLeft)
            {
                rebuilt.left = std::make_shared<const PaneNode>(std::move(*newLeft));
            }
            if (newRight)
            {
                rebuilt.right = std::make_shared<const PaneNode>(std::move(*newRight));
            }
            return PaneNode{ std::move(rebuilt) };
        }
    }

    // Derive TabRecord.mount deterministically from active-ness: every leaf's
    // active tab in the ACTIVE workspace is materialised (lazy-allocated a
    // lifetime ContentId if it has none). Inactive workspaces and already-
    // materialised tabs are left untouched, so a previously-materialised
    // content keeps its stable ContentId across switches. Idempotent.
    inline void applyMountPolicy(WorkspaceModelData& m)
    {
        if (!m.activeWorkspaceId.has_value())
        {
            return; // empty model — nothing is live
        }
        for (auto& ws : m.workspaces)
        {
            if (ws.id != *m.activeWorkspaceId)
            {
                continue;
            }
            if (auto rebuilt = mount_detail::materialiseSubtree(ws.root, m.idCounter))
            {
                ws.root = std::move(*rebuilt);
            }
            break;
        }
    }

    // Pinned-float normalizer. Re-sequence `workspaces` so every pinned
    // workspace precedes every unpinned one (invariant 10), preserving the
    // relative order WITHIN each group (a stable partition). This is the
    // universal invariant-keeper: setWorkspacePinned establishes the precise
    // pin-recency order itself (PIN→end-of-pinned-block, UNPIN→start-of-
    // unpinned-block) and this normalizer is then a no-op for it, but it ALSO
    // re-normalizes any action that could otherwise leave a pinned/unpinned
    // interleave — most notably a free-form reorderWorkspace whose dstIdx lands
    // a pinned workspace among the unpinned (or vice versa). Running it in
    // finalize() makes "pinned before unpinned" hold after every action, so the
    // interleaved state the validator forbids is unrepresentable. Idempotent.
    inline void enforcePinnedOrdering(WorkspaceModelData& m)
    {
        std::stable_partition(m.workspaces.begin(), m.workspaces.end(),
                              [](const WorkspaceState& ws) { return ws.pinned; });
    }

    // Wraps a mutated WorkspaceModelData in a shared_ptr<const>. Re-normalizes
    // the pinned-float ordering (invariant 10) and applies the mount policy (the
    // sole writer of TabRecord.mount), so every action's returned state has its
    // pinned block contiguous AND its active content materialised. The two are
    // independent (the partition only reorders the workspaces vector; the mount
    // policy only writes mounts on the active workspace's leaves), so their
    // order here is immaterial. Debug builds assert that the result satisfies
    // validate(); release builds silently return the pointer (bug surfaces in
    // the test suite where every test calls validate() explicitly).
    [[nodiscard]] inline ModelState finalize(WorkspaceModelData&& m)
    {
        enforcePinnedOrdering(m);
        applyMountPolicy(m);
        assert(!validate(m).has_value());
        return std::make_shared<const WorkspaceModelData>(std::move(m));
    }

    // Allocate the next monotonic id from the model's counter, mutating
    // the counter in place. Returns the freshly-allocated raw value;
    // wrap in the strong-typed id at the call site.
    [[nodiscard]] inline std::uint64_t allocId(std::uint64_t& counter) noexcept
    {
        return ++counter;
    }

    // Remove `id` from the MRU deque (no-op if absent).
    inline void mruErase(std::deque<WorkspaceId>& mru, WorkspaceId id) noexcept
    {
        for (auto it = mru.begin(); it != mru.end(); ++it)
        {
            if (*it == id)
            {
                mru.erase(it);
                return;
            }
        }
    }

    // Move `id` to the front of the MRU (or push if absent).
    inline void mruTouch(std::deque<WorkspaceId>& mru, WorkspaceId id) noexcept
    {
        mruErase(mru, id);
        mru.push_front(id);
    }

    // Look up a workspace by id (mutable reference). Returns nullptr if
    // not found.
    [[nodiscard]] inline WorkspaceState* findWorkspace(WorkspaceModelData& m, WorkspaceId id) noexcept
    {
        for (auto& ws : m.workspaces)
        {
            if (ws.id == id)
            {
                return &ws;
            }
        }
        return nullptr;
    }

    // Const lookup of a workspace by id.
    [[nodiscard]] inline const WorkspaceState* findWorkspace(const WorkspaceModelData& m, WorkspaceId id) noexcept
    {
        for (const auto& ws : m.workspaces)
        {
            if (ws.id == id)
            {
                return &ws;
            }
        }
        return nullptr;
    }

    // Locate the workspace (by index in m.workspaces) containing the leaf
    // with id `paneId`. Returns std::nullopt if not found. Returning an
    // index rather than a pointer avoids lifetime questions when the
    // action subsequently rewrites the workspace's pane tree.
    [[nodiscard]] inline std::optional<std::size_t> findWorkspaceIndexForLeaf(
        const WorkspaceModelData& m,
        PaneId paneId) noexcept
    {
        for (std::size_t i = 0; i < m.workspaces.size(); ++i)
        {
            if (detail::findLeaf(m.workspaces[i].root, paneId) != nullptr)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    // Locate the workspace (by index) + leaf-id + tab-index for a TabId.
    // Returns std::nullopt if the tab isn't found in any workspace.
    struct TabLocationByIndex
    {
        std::size_t workspaceIdx{ 0 };
        PaneId leafId{};
        std::size_t indexInLeaf{ 0 };
    };
    [[nodiscard]] inline std::optional<TabLocationByIndex> findTabLocationByIndex(
        const WorkspaceModelData& m,
        TabId tabId) noexcept
    {
        for (std::size_t i = 0; i < m.workspaces.size(); ++i)
        {
            auto r = detail::findTabInSubtree(m.workspaces[i].root, tabId);
            if (r.leaf != nullptr)
            {
                return TabLocationByIndex{ i, r.leaf->id, r.indexInLeaf };
            }
        }
        return std::nullopt;
    }
}
