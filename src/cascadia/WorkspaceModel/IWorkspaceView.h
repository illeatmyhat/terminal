// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// IWorkspaceView — the consumer's view onto workspace state changes. A
// view receives a stream of WorkspaceChange events emitted by diff() and
// translates each event into whatever representation it owns (XAML in
// production, an in-memory recorder in tests).
//
// The interface defines one apply() overload per WorkspaceChange variant
// arm so implementers cannot accidentally forget an arm — the compiler
// will complain about a missing pure virtual override.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <span>

#include "WorkspaceChange.h"

namespace WorkspaceModel
{
    struct IWorkspaceView
    {
        virtual ~IWorkspaceView() = default;

        virtual void apply(const WorkspaceAdded&) = 0;
        virtual void apply(const WorkspaceRemoved&) = 0;
        virtual void apply(const ActiveWorkspaceChanged&) = 0;
        virtual void apply(const LeafPaneCreated&) = 0;
        virtual void apply(const SplitPaneCreated&) = 0;
        virtual void apply(const SplitPaneCollapsed&) = 0;
        virtual void apply(const SplitRatioChanged&) = 0;
        virtual void apply(const TabAdded&) = 0;
        virtual void apply(const TabRemoved&) = 0;
        virtual void apply(const TabMoved&) = 0;
        virtual void apply(const ActiveTabChanged&) = 0;
        virtual void apply(const ContentMounted&) = 0;
        virtual void apply(const ContentUnmounted&) = 0;
        virtual void apply(const TabDecorationUpdated&) = 0;
    };

    // Thin loop: std::visit each change and forward it to the view. Lives
    // in Diff.cpp.
    void applyChanges(IWorkspaceView& view, std::span<const WorkspaceChange> changes);
}
