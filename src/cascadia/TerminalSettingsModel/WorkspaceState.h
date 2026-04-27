/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- WorkspaceState.h

Abstract:
- Plain data carriers for the cmux-inspired workspaces feature.
- Lives in the implementation namespace (no IDL projection); the higher-level
  workspace logic in TerminalApp converts to/from these structs at the boundary.
- The pane tree is held opaquely as a Json::Value array of action records so
  the four deep modules (Placement, List, Persistence, Migration) can operate
  on it without depending on ActionAndArgs or the rest of the action map.
--*/
#pragma once

#include <json/json.h>
#include <winrt/Windows.UI.h>

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    struct WorkspaceState
    {
        std::wstring title;
        std::optional<winrt::Windows::UI::Color> runtimeColor;
        std::wstring customDescription;
        bool pinned = false;
        uint64_t id = 0;

        // Json::arrayValue of opaque action records that build the pane tree
        // (splitPane, newPaneTab, focusPane, selectPaneTab, etc).
        Json::Value paneTree{ Json::arrayValue };

        bool operator==(const WorkspaceState& other) const noexcept
        {
            return title == other.title &&
                   runtimeColor == other.runtimeColor &&
                   customDescription == other.customDescription &&
                   pinned == other.pinned &&
                   id == other.id &&
                   paneTree == other.paneTree;
        }
    };

    struct WorkspaceListState
    {
        std::vector<WorkspaceState> workspaces;
        std::optional<size_t> activeIndex;
        std::optional<size_t> previousActiveIndex;
        std::optional<double> sidebarWidth;

        bool operator==(const WorkspaceListState& other) const noexcept
        {
            return workspaces == other.workspaces &&
                   activeIndex == other.activeIndex &&
                   previousActiveIndex == other.previousActiveIndex &&
                   sidebarWidth == other.sidebarWidth;
        }
    };
}
