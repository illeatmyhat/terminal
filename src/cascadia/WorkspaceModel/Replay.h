// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Replay engine. Given a starting ModelState and a vector of LogEntry,
// applies the recorded mutators in order and returns the final state.
//
// ID-replay choice (option (b) in the Slice 4 plan): replay calls the
// existing mutators from Mutators.h, which allocate FRESH IDs from the
// state's monotonic counter. As a result the IDs in the replayed model
// generally differ from the IDs that the original session produced.
//
// Why this is OK:
//   - The user-visible state is unchanged. Workspace names, tab
//     descriptions, custom titles, runtime colors, pinned flags, split
//     ratios, axes, active-by-position -- all preserved.
//   - Old WAL records that referenced the original ids are invalidated by
//     a squash, which is expected behaviour: squash writes a fresh
//     snapshot (with new ids) and truncates the log.
//   - The renderer reconciles by structural identity, not raw id equality
//     against any external value, so the reshuffle is invisible to it.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ActionLog.h"
#include "Mutators.h"

namespace WorkspaceModel
{
    // Apply every entry in `entries` to `start`, returning the resulting
    // state. If any mutator throws (or produces a state that fails
    // validate()), replay halts and the partially-applied state is
    // returned. Use replaySafe() if you want to know whether the replay
    // ran to completion.
    [[nodiscard]] ModelState replay(ModelState start,
                                    const std::vector<LogEntry>& entries);

    struct ReplayResult
    {
        // The state after the last successful application.
        ModelState state;

        // std::nullopt iff replay ran to completion. Otherwise an error
        // string describing the failure of the entry at index
        // `entriesApplied` (i.e. the failing entry is the *next* one that
        // would have been applied).
        std::optional<std::string> error;

        // Number of entries that were applied successfully. Always
        // <= entries.size(); equals entries.size() iff error == nullopt.
        std::size_t entriesApplied{ 0 };
    };

    [[nodiscard]] ReplayResult replaySafe(ModelState start,
                                          const std::vector<LogEntry>& entries);
}
