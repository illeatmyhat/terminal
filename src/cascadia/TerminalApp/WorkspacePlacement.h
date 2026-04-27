/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- WorkspacePlacement.h

Abstract:
- Pure-function module that resolves where a new workspace lands in an
  ordered list given the placement policy, the pinned state of each existing
  workspace, the current selection, and whether the new workspace is itself
  pinned.
- Pinned workspaces always sort above unpinned ones; the policy only governs
  ordering within whichever region the new workspace belongs to.
- Lives in TerminalApp because workspace placement is a runtime/UX concern,
  not part of the settings persistence layer. The `WorkspacePlacementPolicy`
  enum it consumes is still projected from TerminalSettingsModel as a
  user-facing setting.
--*/
#pragma once

#include <winrt/Microsoft.Terminal.Settings.Model.h>

namespace TerminalApp
{
    struct WorkspacePlacement
    {
        // Returns the index in the existing list at which the new workspace
        // should be inserted. Caller guarantees that `existingPinned` lists
        // pinned workspaces first, contiguously.
        static size_t ResolveInsertionIndex(
            winrt::Microsoft::Terminal::Settings::Model::WorkspacePlacementPolicy policy,
            const std::vector<bool>& existingPinned,
            std::optional<size_t> currentIndex,
            bool newWorkspaceIsPinned) noexcept;

        // Helper: count of pinned workspaces at the front of the list.
        static size_t CountPinnedPrefix(const std::vector<bool>& existingPinned) noexcept;
    };
}
