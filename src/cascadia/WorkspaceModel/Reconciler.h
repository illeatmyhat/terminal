// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// reconcile(prev, next) — the pure, side-effect-free diff that turns two
// ModelState values into a typed list of RenderOps. See Reconciler.cpp for
// the algorithm and emit-ordering rationale.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <vector>

#include "Mutators.h" // for ModelState alias
#include "RenderOp.h"

namespace WorkspaceModel
{
    // Diff two model states and return the sequence of RenderOps that
    // would transform a surface from `prev` to `next`. A null shared_ptr
    // is treated as an empty model (the spec's "no workspaces" state).
    //
    // The function is pure: no globals touched, no I/O, no allocations
    // beyond what's needed to construct the returned vector.
    [[nodiscard]] std::vector<RenderOp> reconcile(const ModelState& prev,
                                                  const ModelState& next);
}
