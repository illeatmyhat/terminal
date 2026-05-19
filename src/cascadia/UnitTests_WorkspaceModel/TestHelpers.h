// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Shared fixture-builder helpers for the WorkspaceModel action tests.
// Each helper returns minimal well-formed shapes; tests then layer their
// specific scenario on top with the rich action API.

#pragma once

#include "../WorkspaceModel/WorkspaceActions.h"
#include "../WorkspaceModel/PaneTree.h"
#include "../WorkspaceModel/TabContent.h"
#include "../WorkspaceModel/Validator.h"
#include "../WorkspaceModel/WorkspaceState.h"

#include <memory>

namespace WorkspaceModelUnitTests
{
    // Produce a freshly-initialised, empty model.
    inline WorkspaceModel::ModelState emptyModel()
    {
        return std::make_shared<const WorkspaceModel::WorkspaceModelData>();
    }

    // Build a TerminalSpec whose profile bytes start with the given seed
    // so tests can produce distinguishable specs.
    inline WorkspaceModel::TerminalSpec termSpec(std::uint8_t seed)
    {
        WorkspaceModel::TerminalSpec s;
        s.profile[0] = seed;
        return s;
    }

    // Create a single-workspace model with one Terminal tab. Returns the
    // model state plus the workspace id, leaf pane id, and tab id minted
    // by newWorkspace.
    struct SingleWorkspaceFixture
    {
        WorkspaceModel::ModelState state;
        WorkspaceModel::WorkspaceId wsId;
        WorkspaceModel::PaneId leafId;
        WorkspaceModel::TabId tabId;
    };

    inline SingleWorkspaceFixture makeSingleWorkspace()
    {
        auto initial = emptyModel();
        auto r = WorkspaceModel::newWorkspace(initial, "ws1", termSpec(1));
        // newWorkspace allocates ids in order: workspace, pane, tab.
        // We pull them out via the queries (or by indexing the resulting
        // model directly).
        const auto& wsList = r.state->workspaces;
        SingleWorkspaceFixture out;
        out.state = r.state;
        out.wsId = r.id;
        const auto& ws = wsList[0];
        const auto& leaf = std::get<WorkspaceModel::LeafPane>(ws.root);
        out.leafId = leaf.id;
        out.tabId = leaf.tabs[0].id;
        return out;
    }
}
