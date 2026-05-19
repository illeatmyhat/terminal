// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Workspace-lifecycle actions: newWorkspace, closeWorkspace,
// closeOtherWorkspaces, closeAllWorkspaces, switchToWorkspace,
// renameWorkspace, setWorkspaceColor, setWorkspaceDescription,
// setWorkspacePinned, reorderWorkspace.

#include "pch.h"

#include "WorkspaceActionHelpers.h"
#include "WorkspaceActions.h"

namespace WorkspaceModel
{
    namespace
    {
        using namespace detail;

        // Erase the workspace at `idx` from the workspaces vector and
        // update activeWorkspaceId / MRU consistently. Active falls back
        // to the next entry in MRU (the next-most-recent workspace) and,
        // if no entries remain, to std::nullopt.
        void eraseWorkspaceAt(WorkspaceModelData& m, std::size_t idx) noexcept
        {
            const auto removedId = m.workspaces[idx].id;
            m.workspaces.erase(m.workspaces.begin() + static_cast<std::ptrdiff_t>(idx));
            mruErase(m.mru, removedId);

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
        }
    }

    // ---------------------------------------------------------------------
    NewWorkspaceResult newWorkspace(const ModelState& state,
                                    std::string name,
                                    TabContent initialTab,
                                    std::string initialTabTitle,
                                    std::optional<Color> initialTabColor,
                                    bool initialTabPinned)
    {
        auto m = detail::copyOf(state);

        const auto wsId = WorkspaceId{ detail::allocId(m.idCounter) };
        const auto paneId = PaneId{ detail::allocId(m.idCounter) };
        const auto tabId = TabId{ detail::allocId(m.idCounter) };

        TabRecord t;
        t.id = tabId;
        t.description = std::move(initialTab);
        t.customTitle = std::move(initialTabTitle);
        t.runtimeColor = initialTabColor;
        t.pinned = initialTabPinned;

        LeafPane leaf;
        leaf.id = paneId;
        leaf.tabs.push_back(std::move(t));
        leaf.activeTabIdx = 0;

        WorkspaceState ws;
        ws.id = wsId;
        ws.name = std::move(name);
        ws.root = std::move(leaf);
        ws.activePaneId = paneId;

        m.workspaces.push_back(std::move(ws));
        m.activeWorkspaceId = wsId;
        detail::mruTouch(m.mru, wsId);

        return NewWorkspaceResult{ detail::finalize(std::move(m)), wsId };
    }

    // ---------------------------------------------------------------------
    ModelState closeWorkspace(const ModelState& state, WorkspaceId id)
    {
        auto m = detail::copyOf(state);
        for (std::size_t i = 0; i < m.workspaces.size(); ++i)
        {
            if (m.workspaces[i].id == id)
            {
                eraseWorkspaceAt(m, i);
                return detail::finalize(std::move(m));
            }
        }
        return state ? state : detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState closeOtherWorkspaces(const ModelState& state, WorkspaceId keep)
    {
        auto m = detail::copyOf(state);

        bool keepExists = false;
        for (const auto& ws : m.workspaces)
        {
            if (ws.id == keep)
            {
                keepExists = true;
                break;
            }
        }
        if (!keepExists)
        {
            return state ? state : detail::finalize(std::move(m));
        }

        for (std::size_t i = m.workspaces.size(); i-- > 0;)
        {
            if (m.workspaces[i].id != keep)
            {
                eraseWorkspaceAt(m, i);
            }
        }
        m.activeWorkspaceId = keep;
        detail::mruTouch(m.mru, keep);
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState closeAllWorkspaces(const ModelState& state)
    {
        auto m = detail::copyOf(state);
        m.workspaces.clear();
        m.mru.clear();
        m.activeWorkspaceId.reset();
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState switchToWorkspace(const ModelState& state, WorkspaceId id)
    {
        auto m = detail::copyOf(state);
        bool exists = false;
        for (const auto& ws : m.workspaces)
        {
            if (ws.id == id)
            {
                exists = true;
                break;
            }
        }
        if (!exists)
        {
            return state ? state : detail::finalize(std::move(m));
        }
        m.activeWorkspaceId = id;
        detail::mruTouch(m.mru, id);
        return detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState renameWorkspace(const ModelState& state, WorkspaceId id, std::string name)
    {
        auto m = detail::copyOf(state);
        if (auto* ws = detail::findWorkspace(m, id))
        {
            ws->name = std::move(name);
            return detail::finalize(std::move(m));
        }
        return state ? state : detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState setWorkspaceColor(const ModelState& state, WorkspaceId id, std::optional<Color> color)
    {
        auto m = detail::copyOf(state);
        if (auto* ws = detail::findWorkspace(m, id))
        {
            ws->color = color;
            return detail::finalize(std::move(m));
        }
        return state ? state : detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState setWorkspaceDescription(const ModelState& state, WorkspaceId id, std::string description)
    {
        auto m = detail::copyOf(state);
        if (auto* ws = detail::findWorkspace(m, id))
        {
            ws->customDescription = std::move(description);
            return detail::finalize(std::move(m));
        }
        return state ? state : detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState setWorkspacePinned(const ModelState& state, WorkspaceId id, bool pinned)
    {
        auto m = detail::copyOf(state);
        if (auto* ws = detail::findWorkspace(m, id))
        {
            ws->pinned = pinned;
            return detail::finalize(std::move(m));
        }
        return state ? state : detail::finalize(std::move(m));
    }

    // ---------------------------------------------------------------------
    ModelState reorderWorkspace(const ModelState& state, WorkspaceId id, std::size_t dstIdx)
    {
        auto m = detail::copyOf(state);
        if (m.workspaces.empty())
        {
            return state ? state : detail::finalize(std::move(m));
        }

        std::size_t srcIdx = m.workspaces.size();
        for (std::size_t i = 0; i < m.workspaces.size(); ++i)
        {
            if (m.workspaces[i].id == id)
            {
                srcIdx = i;
                break;
            }
        }
        if (srcIdx == m.workspaces.size())
        {
            return state ? state : detail::finalize(std::move(m));
        }

        const auto clamped = std::min(dstIdx, m.workspaces.size() - 1);
        if (srcIdx == clamped)
        {
            return state ? state : detail::finalize(std::move(m));
        }

        auto ws = std::move(m.workspaces[srcIdx]);
        m.workspaces.erase(m.workspaces.begin() + static_cast<std::ptrdiff_t>(srcIdx));
        m.workspaces.insert(m.workspaces.begin() + static_cast<std::ptrdiff_t>(clamped),
                            std::move(ws));
        return detail::finalize(std::move(m));
    }
}
