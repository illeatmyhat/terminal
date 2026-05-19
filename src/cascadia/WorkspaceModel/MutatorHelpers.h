// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tiny private helpers shared across all the Mutators_*.cpp files.
//
// This header is intentionally internal — it sits inside the
// WorkspaceModel namespace alongside the public mutators but only the
// implementation files include it. Callers of the library reach for
// Mutators.h (or the helpers in Cascade.h if they're doing structural
// rebuilds) instead.

#pragma once

#include "Mutators.h"
#include "Validator.h"
#include "Cascade.h"

#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>

namespace WorkspaceModel::detail
{
    // Deep-mutable copy of the state's underlying data. Mutators build a
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

    // Wraps a mutated WorkspaceModelData in a shared_ptr<const>. Debug
    // builds assert that the result satisfies validate(); release builds
    // silently return the pointer (bug surfaces in the test suite where
    // every test calls validate() explicitly).
    [[nodiscard]] inline ModelState finalize(WorkspaceModelData&& m)
    {
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
    // mutator subsequently rewrites the workspace's pane tree.
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
