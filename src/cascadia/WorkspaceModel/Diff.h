// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// diff(prev, next) — the pure, side-effect-free function that turns two
// ModelState values into a typed list of WorkspaceChanges. See Diff.cpp
// for the algorithm and emit-ordering rationale.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <vector>

#include "WorkspaceActions.h" // for ModelState alias
#include "WorkspaceChange.h"

namespace WorkspaceModel
{
    // Compare two model states and return the sequence of WorkspaceChanges
    // that would transform a view from `prev` to `next`. A null shared_ptr
    // is treated as an empty model (the spec's "no workspaces" state).
    //
    // The function is pure: no globals touched, no I/O, no allocations
    // beyond what's needed to construct the returned vector.
    [[nodiscard]] std::vector<WorkspaceChange> diff(const ModelState& prev,
                                                    const ModelState& next);
}
