// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// WorkspaceView is the only IWorkspaceView implementation. It holds a
// weak reference to the owning TerminalPage and translates the stream of
// WorkspaceChange events emitted by diff() into mutations against
// TerminalPage's classic XAML data structures (_tabs, _tabView,
// Pane._tabs).
//
// This is a plain C++ class, not a WinRT runtimeclass. It is constructed
// and owned by TerminalPage and lives for the lifetime of the page.
//
// Each apply() overload corresponds to one WorkspaceChange arm. The
// arms a migrated Phase 1 action can actually emit carry real logic;
// the remaining arms are stubs that later slices fill in.
//
// Every change arm is self-describing: diff() enriches each WorkspaceChange
// with the payload its apply() overload needs (e.g. TabAdded carries the
// content spec, the owning workspace, and a "leaf nested in a split" flag),
// so the view never holds or resolves model state of its own.

#pragma once

#include "../WorkspaceModel/IWorkspaceView.h"
#include "../WorkspaceModel/WorkspaceChange.h"

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;

    class WorkspaceView final : public ::WorkspaceModel::IWorkspaceView
    {
    public:
        explicit WorkspaceView(winrt::weak_ref<TerminalPage> owner) noexcept;

        void apply(const ::WorkspaceModel::WorkspaceAdded& c) override;
        void apply(const ::WorkspaceModel::WorkspaceRemoved& c) override;
        void apply(const ::WorkspaceModel::ActiveWorkspaceChanged& c) override;
        void apply(const ::WorkspaceModel::LeafPaneCreated& c) override;
        void apply(const ::WorkspaceModel::SplitPaneCreated& c) override;
        void apply(const ::WorkspaceModel::SplitPaneCollapsed& c) override;
        void apply(const ::WorkspaceModel::SplitRatioChanged& c) override;
        void apply(const ::WorkspaceModel::TabAdded& c) override;
        void apply(const ::WorkspaceModel::TabRemoved& c) override;
        void apply(const ::WorkspaceModel::TabMoved& c) override;
        void apply(const ::WorkspaceModel::ActiveTabChanged& c) override;
        void apply(const ::WorkspaceModel::ContentMounted& c) override;
        void apply(const ::WorkspaceModel::ContentUnmounted& c) override;
        void apply(const ::WorkspaceModel::TabDecorationUpdated& c) override;

    private:
        // Resolves the weak reference to a strong com_ptr each time. Returns
        // an empty com_ptr if the page is gone.
        winrt::com_ptr<TerminalPage> _page() const;

        winrt::weak_ref<TerminalPage> _owner;
    };
}
