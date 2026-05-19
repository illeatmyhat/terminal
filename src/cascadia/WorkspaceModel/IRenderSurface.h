// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// IRenderSurface — the abstract surface that consumes a stream of RenderOps
// produced by reconcile(). Production code wires up an XaxlRenderSurface
// (deferred to a later slice); tests use MockRenderSurface.
//
// The interface defines one apply() overload per RenderOp variant arm so
// implementers cannot accidentally forget an arm — the compiler will
// complain about a missing pure virtual override.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <span>

#include "RenderOp.h"

namespace WorkspaceModel
{
    struct IRenderSurface
    {
        virtual ~IRenderSurface() = default;

        virtual void apply(const AddWorkspace&) = 0;
        virtual void apply(const RemoveWorkspace&) = 0;
        virtual void apply(const SetActiveWorkspace&) = 0;
        virtual void apply(const CreateLeafPane&) = 0;
        virtual void apply(const CreateSplitPane&) = 0;
        virtual void apply(const CollapseSplitPane&) = 0;
        virtual void apply(const SetSplitRatio&) = 0;
        virtual void apply(const AddTab&) = 0;
        virtual void apply(const RemoveTab&) = 0;
        virtual void apply(const MoveTab&) = 0;
        virtual void apply(const SetActiveTab&) = 0;
        virtual void apply(const MountContent&) = 0;
        virtual void apply(const UnmountContent&) = 0;
        virtual void apply(const UpdateTabDecoration&) = 0;
    };

    // Thin loop: std::visit each op and forward to the surface. Lives in
    // Reconciler.cpp.
    void applyOps(IRenderSurface& surface, std::span<const RenderOp> ops);
}
