/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- WorkspacePersistence.h

Abstract:
- Round-trippable serialization of WorkspaceListState to/from the action-replay
  JSON form described in the workspaces spec. Workspace boundaries are implicit
  in the order of `newWorkspace` actions; the active workspace is captured as
  a final `selectWorkspace`.
- The pane tree of each workspace is held opaquely as a JSON array of action
  records and is round-tripped verbatim. This module does not depend on
  ActionAndArgs or the action map registry.
--*/
#pragma once

#include "WorkspaceState.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct WorkspacePersistence
    {
        // Action keys used in the persisted JSON.
        static constexpr std::string_view NewWorkspaceAction = "newWorkspace";
        static constexpr std::string_view SelectWorkspaceAction = "selectWorkspace";

        // Serializes the workspace list as a flat array of action records
        // suitable for storage in WindowLayout.tabLayout. The actives are
        // emitted as a trailing `selectWorkspace`.
        static Json::Value SerializeActions(const WorkspaceListState& state);

        // Inverse of SerializeActions. Returns std::nullopt on malformed
        // input (so the caller can fall back to a fresh window per spec).
        static std::optional<WorkspaceListState> DeserializeActions(const Json::Value& actions);

        // Wraps SerializeActions with the WindowLayout-level metadata
        // (sidebar width, etc) used to round-trip a single window's
        // workspace state through ApplicationState.
        static Json::Value SerializeWindowLayout(const WorkspaceListState& state);
        static std::optional<WorkspaceListState> DeserializeWindowLayout(const Json::Value& obj);

        // Json keys for the wrapping object.
        static constexpr std::string_view TabLayoutKey = "tabLayout";
        static constexpr std::string_view SidebarWidthKey = "sidebarWidth";

    private:
        static Json::Value _workspaceHeader(const WorkspaceState& ws);
        static bool _readWorkspaceHeader(const Json::Value& header, WorkspaceState& out);
    };
}
