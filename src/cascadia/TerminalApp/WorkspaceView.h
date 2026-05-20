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
// arms used by the Phase 1 Slice 2 migrated actions (startup-replay,
// default-profile new-tab) and the Phase 1 Slice 6 migrated actions
// (rename / color decoration, explicit-profile new-tab) carry real
// logic; the remaining arms are stubs that will be filled in by
// subsequent slices.
//
// The view also holds the latest ModelState. Some change arms
// (notably TabAdded) describe a state transition without carrying every
// field of the affected record — the view looks the rest up by id
// against the held state. Callers must call setState() with the new
// state BEFORE invoking applyChanges() so lookups resolve against the
// post-diff state.

#pragma once

#include "../WorkspaceModel/IWorkspaceView.h"
#include "../WorkspaceModel/WorkspaceActions.h"
#include "../WorkspaceModel/WorkspaceChange.h"

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage;

    class WorkspaceView final : public ::WorkspaceModel::IWorkspaceView
    {
    public:
        explicit WorkspaceView(winrt::weak_ref<TerminalPage> owner) noexcept;

        // The latest ModelState the view should resolve id lookups
        // against. Set this to `next` BEFORE calling applyChanges() so
        // arms like TabAdded can find the post-diff TabRecord.
        void setState(::WorkspaceModel::ModelState state) noexcept;

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
        ::WorkspaceModel::ModelState _state;
    };
}
