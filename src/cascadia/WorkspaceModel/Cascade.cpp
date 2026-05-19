// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "Cascade.h"

namespace WorkspaceModel::detail
{
    std::optional<PaneNode> transformLeaf(const PaneNode& node,
                                          PaneId target,
                                          const LeafTransform& transform,
                                          bool& found)
    {
        if (const auto* leaf = std::get_if<LeafPane>(&node))
        {
            if (leaf->id != target)
            {
                // Unchanged leaf — return a copy.
                return PaneNode{ *leaf };
            }
            found = true;
            auto result = transform(*leaf);
            if (!result.has_value())
            {
                return std::nullopt;
            }
            return PaneNode{ *result };
        }

        const auto& split = std::get<SplitPane>(node);

        // We must rebuild this split if either child contains the target.
        // Recurse into left first; if it found+collapsed, replace this
        // split with whatever right subtree resolves to.
        bool leftFound = false;
        std::optional<PaneNode> newLeft = split.left
                                              ? transformLeaf(*split.left, target, transform, leftFound)
                                              : std::nullopt;
        if (leftFound)
        {
            found = true;
            if (!newLeft.has_value())
            {
                // Left collapsed — this split becomes its right child.
                if (!split.right)
                {
                    // The split was malformed (no right child). Bubble
                    // the disappearance up; the caller will handle it.
                    return std::nullopt;
                }
                return *split.right;
            }
            // Rebuild the split with newLeft and the unchanged right.
            SplitPane rebuilt;
            rebuilt.id = split.id;
            rebuilt.axis = split.axis;
            rebuilt.ratio = split.ratio;
            rebuilt.left = std::make_shared<const PaneNode>(*newLeft);
            rebuilt.right = split.right;
            return PaneNode{ rebuilt };
        }

        // Not in left; try right.
        bool rightFound = false;
        std::optional<PaneNode> newRight = split.right
                                               ? transformLeaf(*split.right, target, transform, rightFound)
                                               : std::nullopt;
        if (rightFound)
        {
            found = true;
            if (!newRight.has_value())
            {
                // Right collapsed — this split becomes its left child.
                if (!split.left)
                {
                    return std::nullopt;
                }
                return *split.left;
            }
            SplitPane rebuilt;
            rebuilt.id = split.id;
            rebuilt.axis = split.axis;
            rebuilt.ratio = split.ratio;
            rebuilt.left = split.left;
            rebuilt.right = std::make_shared<const PaneNode>(*newRight);
            return PaneNode{ rebuilt };
        }

        // Not found in either subtree — return the split unchanged.
        return PaneNode{ split };
    }

    void collectLeaves(const PaneNode& node, std::vector<const LeafPane*>& out)
    {
        if (const auto* leaf = std::get_if<LeafPane>(&node))
        {
            out.push_back(leaf);
            return;
        }
        const auto& split = std::get<SplitPane>(node);
        if (split.left)
        {
            collectLeaves(*split.left, out);
        }
        if (split.right)
        {
            collectLeaves(*split.right, out);
        }
    }

    const LeafPane* findLeaf(const PaneNode& node, PaneId target) noexcept
    {
        if (const auto* leaf = std::get_if<LeafPane>(&node))
        {
            return (leaf->id == target) ? leaf : nullptr;
        }
        const auto& split = std::get<SplitPane>(node);
        if (split.left)
        {
            if (auto* r = findLeaf(*split.left, target))
            {
                return r;
            }
        }
        if (split.right)
        {
            if (auto* r = findLeaf(*split.right, target))
            {
                return r;
            }
        }
        return nullptr;
    }

    TabLocation findTabInSubtree(const PaneNode& node, TabId target) noexcept
    {
        if (const auto* leaf = std::get_if<LeafPane>(&node))
        {
            for (std::size_t i = 0; i < leaf->tabs.size(); ++i)
            {
                if (leaf->tabs[i].id == target)
                {
                    return TabLocation{ leaf, i };
                }
            }
            return TabLocation{ nullptr, 0 };
        }
        const auto& split = std::get<SplitPane>(node);
        if (split.left)
        {
            auto r = findTabInSubtree(*split.left, target);
            if (r.leaf != nullptr)
            {
                return r;
            }
        }
        if (split.right)
        {
            auto r = findTabInSubtree(*split.right, target);
            if (r.leaf != nullptr)
            {
                return r;
            }
        }
        return TabLocation{ nullptr, 0 };
    }

    PaneNode replaceLeaf(const PaneNode& node,
                         PaneId target,
                         const LeafPane& replacement,
                         bool& found)
    {
        auto result = transformLeaf(
            node,
            target,
            [&](const LeafPane&) -> std::optional<LeafPane> { return replacement; },
            found);
        // The transform always returns a leaf, so `transformLeaf` should
        // never return std::nullopt. But if it does (malformed tree),
        // fall back to a copy of the input.
        if (!result.has_value())
        {
            return node;
        }
        return *result;
    }

    PaneNode replaceLeafWithSubtree(const PaneNode& node,
                                    PaneId target,
                                    const PaneNode& replacement,
                                    bool& found)
    {
        if (const auto* leaf = std::get_if<LeafPane>(&node))
        {
            if (leaf->id != target)
            {
                return *leaf;
            }
            found = true;
            return replacement;
        }
        const auto& split = std::get<SplitPane>(node);

        // Recurse into left if it exists; only consider the result if
        // the target was found inside that subtree.
        if (split.left)
        {
            bool leftFound = false;
            auto newLeft = replaceLeafWithSubtree(*split.left, target, replacement, leftFound);
            if (leftFound)
            {
                found = true;
                SplitPane rebuilt;
                rebuilt.id = split.id;
                rebuilt.axis = split.axis;
                rebuilt.ratio = split.ratio;
                rebuilt.left = std::make_shared<const PaneNode>(newLeft);
                rebuilt.right = split.right;
                return PaneNode{ rebuilt };
            }
        }

        if (split.right)
        {
            bool rightFound = false;
            auto newRight = replaceLeafWithSubtree(*split.right, target, replacement, rightFound);
            if (rightFound)
            {
                found = true;
                SplitPane rebuilt;
                rebuilt.id = split.id;
                rebuilt.axis = split.axis;
                rebuilt.ratio = split.ratio;
                rebuilt.left = split.left;
                rebuilt.right = std::make_shared<const PaneNode>(newRight);
                return PaneNode{ rebuilt };
            }
        }

        return PaneNode{ split };
    }

    const LeafPane* firstLeaf(const PaneNode& node) noexcept
    {
        if (const auto* leaf = std::get_if<LeafPane>(&node))
        {
            return leaf;
        }
        const auto& split = std::get<SplitPane>(node);
        if (split.left)
        {
            if (auto* r = firstLeaf(*split.left))
            {
                return r;
            }
        }
        if (split.right)
        {
            if (auto* r = firstLeaf(*split.right))
            {
                return r;
            }
        }
        return nullptr;
    }
}
