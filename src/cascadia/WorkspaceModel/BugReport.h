// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// dumpBugReport() writes a bundle directory containing:
//   - state.json    (the current in-memory ModelState, serialized fresh)
//   - actions.log   (a snapshot copy of the live WAL file)
//
// The bundle is independent of the live persistence files: modifying the
// live log AFTER dumpBugReport() returns does not affect the bundle.
// (We achieve this by copying the file contents at dump time rather than
// hard-linking or referencing the live file.)
//
// Callers are responsible for choosing a destination directory. Typical
// callers will pick something like
// `%LOCALAPPDATA%/.../bugreport_<iso8601>/` so multiple dumps don't
// collide. This helper does not generate that path; it just writes the
// two files at `destDir`.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <filesystem>

#include "WorkspaceActions.h"

namespace WorkspaceModel
{
    // Dump a snapshot + log bundle under `destDir`. `destDir` is created
    // (with parents) if it does not exist. `logPath` may be a path to a
    // file that does not exist (e.g. when no mutations have happened
    // since the last squash); the bundle's actions.log will be empty in
    // that case.
    void dumpBugReport(const WorkspaceModelData& state,
                       const std::filesystem::path& logPath,
                       const std::filesystem::path& destDir);
}
