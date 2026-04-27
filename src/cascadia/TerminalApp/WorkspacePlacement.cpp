// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspacePlacement.h"

namespace TerminalApp
{
    using winrt::Microsoft::Terminal::Settings::Model::WorkspacePlacementPolicy;

    size_t WorkspacePlacement::CountPinnedPrefix(const std::vector<bool>& existingPinned) noexcept
    {
        size_t count = 0;
        for (const auto pinned : existingPinned)
        {
            if (!pinned)
            {
                break;
            }
            ++count;
        }
        return count;
    }

    size_t WorkspacePlacement::ResolveInsertionIndex(
        WorkspacePlacementPolicy policy,
        const std::vector<bool>& existingPinned,
        std::optional<size_t> currentIndex,
        bool newWorkspaceIsPinned) noexcept
    {
        const auto total = existingPinned.size();
        const auto pinnedCount = CountPinnedPrefix(existingPinned);

        // Region: pinned workspaces occupy [0, pinnedCount), unpinned occupy
        // [pinnedCount, total). The new workspace can only land in the region
        // matching its own pin state.
        const size_t regionBegin = newWorkspaceIsPinned ? 0 : pinnedCount;
        const size_t regionEnd = newWorkspaceIsPinned ? pinnedCount : total;

        // currentIndex is "inside the region" only if it points to a workspace
        // whose pin state matches the new workspace's. If the active workspace
        // is in the other region, `afterCurrent` falls back to the end of the
        // new workspace's region.
        const bool currentInRegion = currentIndex.has_value() &&
                                     *currentIndex < total &&
                                     existingPinned[*currentIndex] == newWorkspaceIsPinned;

        switch (policy)
        {
        case WorkspacePlacementPolicy::Top:
            return regionBegin;
        case WorkspacePlacementPolicy::End:
            return regionEnd;
        case WorkspacePlacementPolicy::AfterCurrent:
        default:
            if (currentInRegion)
            {
                // Insert immediately after the current selection, but never
                // outside the region.
                return std::min(*currentIndex + 1, regionEnd);
            }
            return regionEnd;
        }
    }
}
