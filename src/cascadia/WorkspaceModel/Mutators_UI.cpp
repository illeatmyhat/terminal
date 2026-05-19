// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// UI prefs mutator: setSidebarWidth.

#include "pch.h"

#include "MutatorHelpers.h"
#include "Mutators.h"

namespace WorkspaceModel
{
    ModelState setSidebarWidth(const ModelState& state, double width)
    {
        auto m = detail::copyOf(state);
        // Clamp to a non-negative finite value. NaN maps to 0.
        if (!(width == width)) // NaN check
        {
            width = 0.0;
        }
        if (width < 0.0)
        {
            width = 0.0;
        }
        m.sidebarWidth = width;
        return detail::finalize(std::move(m));
    }
}
