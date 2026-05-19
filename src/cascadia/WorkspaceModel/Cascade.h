// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Internal cascade helpers shared across action families.
//
// The pane tree is immutable shared-substructure (every SplitPane child is
// `shared_ptr<const PaneNode>`); structural updates rebuild only the path
// from the root to the affected leaf. The functions in this header are the
// rebuild primitives plus the "split collapses when a child disappears"
// rule that close-style actions all share.
//
// These helpers are intentionally not exported from the library; consumers
// outside the action family don't need them and shouldn't depend on them.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include "Ids.h"
#include "PaneTree.h"
#include "WorkspaceState.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace WorkspaceModel::detail
{
    // A leaf transform takes a leaf (by value) and returns either a
    // replacement leaf (the mutated form) or std::nullopt to signal "this
    // leaf is gone — cascade up".
    using LeafTransform = std::function<std::optional<LeafPane>(const LeafPane&)>;

    // Walk `node` looking for the leaf with id `target`. When found, invoke
    // `transform(leaf)`. If the transform returns a leaf, that leaf
    // replaces the original in the rebuilt subtree. If the transform
    // returns std::nullopt, the leaf disappears and the parent split
    // collapses to its surviving sibling — which may itself cascade if the
    // surviving sibling is the result of an earlier collapse.
    //
    // Returns std::nullopt iff `target` was the only leaf in the subtree
    // and was eliminated. Callers can use that to detect "workspace's root
    // pane is gone — remove the workspace" cascades.
    //
    // `found` is set to true iff the target leaf was located. A false
    // return for `found` with non-nullopt result means the structure was
    // unchanged.
    [[nodiscard]] std::optional<PaneNode> transformLeaf(const PaneNode& node,
                                                        PaneId target,
                                                        const LeafTransform& transform,
                                                        bool& found);

    // Walk `node` and collect every leaf in left-to-right tree order.
    void collectLeaves(const PaneNode& node, std::vector<const LeafPane*>& out);

    // Returns the leaf with the given id, or nullptr if not found.
    [[nodiscard]] const LeafPane* findLeaf(const PaneNode& node, PaneId target) noexcept;

    // Returns the LeafPane* for the leaf containing the given TabId
    // anywhere in the subtree, plus the index of the tab within that
    // leaf's vector. Returns {nullptr, 0} if not found.
    struct TabLocation
    {
        const LeafPane* leaf{ nullptr };
        std::size_t indexInLeaf{ 0 };
    };
    [[nodiscard]] TabLocation findTabInSubtree(const PaneNode& node, TabId target) noexcept;

    // Replace the leaf with id `target` in `node` with `replacement`. The
    // replacement's id should equal `target` (caller's responsibility);
    // this helper does not enforce it. Returns the rebuilt subtree.
    // `found` is set to true iff the target was located.
    [[nodiscard]] PaneNode replaceLeaf(const PaneNode& node,
                                       PaneId target,
                                       const LeafPane& replacement,
                                       bool& found);

    // Replace the leaf with id `target` with an arbitrary subtree
    // `replacement` (which may itself be a split). Used by splitPane to
    // wrap the original leaf in a new SplitPane node.
    [[nodiscard]] PaneNode replaceLeafWithSubtree(const PaneNode& node,
                                                  PaneId target,
                                                  const PaneNode& replacement,
                                                  bool& found);

    // Returns the first leaf in left-to-right tree order. Used to pick a
    // fallback activePaneId after a cascade rewrites the tree.
    [[nodiscard]] const LeafPane* firstLeaf(const PaneNode& node) noexcept;
}
