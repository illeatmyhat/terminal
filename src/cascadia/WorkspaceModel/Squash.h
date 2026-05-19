// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// squash() collapses a ModelState plus an actions.log into a single
// fresh state.json snapshot. After squash:
//   - snapshotPath holds the JSON shape of `state`.
//   - logPath is truncated to zero bytes.
//
// The write is atomic against partial-snapshot corruption: we write to
// snapshotPath + ".tmp", flush+close, then std::filesystem::rename over
// the live snapshot. A crash mid-write leaves the prior snapshot intact;
// a crash AFTER rename but BEFORE truncate replays the log on top of the
// new snapshot one extra time -- benign because actions are idempotent
// over their structural intent (the IDs differ but the structure does
// not).
//
// The squash *scheduler* (200 ops / 60 s idle / clean exit) is not in
// this header; this header only provides the operation itself. The
// callers that own persistence wire the trigger.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <filesystem>

#include "WorkspaceActions.h"

namespace WorkspaceModel
{
    // Serialize `state` to `snapshotPath` via a write-then-rename pattern,
    // then truncate `logPath`. Throws SerializerError on any filesystem
    // failure. The state itself is not validated by squash; callers that
    // want a validity guarantee must run validate() beforehand.
    //
    // If `snapshotPath`'s parent directory does not exist, it is created.
    // If `logPath` does not exist, it is left absent (no-op).
    void squash(const WorkspaceModelData& state,
                const std::filesystem::path& snapshotPath,
                const std::filesystem::path& logPath);
}
