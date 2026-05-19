// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "WorkspaceView.h"
#include "TerminalPage.h"

namespace winrt::TerminalApp::implementation
{
    WorkspaceView::WorkspaceView(winrt::weak_ref<winrt::TerminalApp::TerminalPage> owner) noexcept :
        _owner{ std::move(owner) }
    {
    }

    void WorkspaceView::setState(::WorkspaceModel::ModelState state) noexcept
    {
        _state = std::move(state);
    }

    winrt::com_ptr<TerminalPage> WorkspaceView::_page() const
    {
        winrt::com_ptr<TerminalPage> p{ nullptr };
        if (auto strong{ _owner.get() })
        {
            p.copy_from(winrt::get_self<TerminalPage>(strong));
        }
        return p;
    }

    // -------------------------------------------------------------------
    // Phase 1 Slice 2: only TabAdded carries real logic. The other arms
    // are intentional stubs — subsequent slices fill them in.
    // -------------------------------------------------------------------

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceAdded& /*c*/)
    {
        // Phase 1: one model workspace == one classic window-level tab.
        // The classic tab is materialised by the TabAdded arm; there is
        // no separate workspace-level XAML object yet (that lands in
        // Phase 2's sidebar slice).
    }

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceRemoved& /*c*/)
    {
        // TODO(workspace-slice-3): close-cascade end-to-end. Issue #20.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::ActiveWorkspaceChanged& /*c*/)
    {
        // TODO(workspace-slice-4): workspace-switch + active-pane focus.
        // Issue #21.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::LeafPaneCreated& /*c*/)
    {
        // Phase 1: each workspace has exactly one leaf, created
        // implicitly by WorkspaceActions::newWorkspace. The classic
        // window-level tab that backs the workspace already represents
        // this leaf, so no extra XAML mutation is needed here.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitPaneCreated& /*c*/)
    {
        // TODO(workspace-slice-5): split-pane tree topology. Issue #22.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitPaneCollapsed& /*c*/)
    {
        // TODO(workspace-slice-5): split-pane tree topology. Issue #22.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitRatioChanged& /*c*/)
    {
        // TODO(workspace-slice-5): split-pane tree topology. Issue #22.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabAdded& c)
    {
        // The classic window-level tab that materialises this model tab
        // is the only XAML we own at Phase 1. For Slice 2 we route only
        // the default-profile TerminalSpec case; other content kinds
        // (Settings, Snippets, Markdown, Scratchpad) are exercised by
        // Slice 6.
        auto page = _page();
        if (!page || !_state)
        {
            return;
        }

        const auto* record = _state->tab(c.id);
        if (!record)
        {
            return;
        }
        if (!std::holds_alternative<::WorkspaceModel::TerminalSpec>(record->description))
        {
            return;
        }

        // For a default-profile spec the profile bytes are the zero
        // GUID; in that case we ask the classic path to use whatever
        // CascadiaSettings picks as the default profile. Explicit-
        // profile dispatch lands in Slice 6.
        page->_openDefaultTabForWorkspace();
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabRemoved& /*c*/)
    {
        // TODO(workspace-slice-3): close-cascade end-to-end. Issue #20.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabMoved& /*c*/)
    {
        // TODO(workspace-slice-5): identity-preserving cross-leaf moves.
        // Issue #22.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::ActiveTabChanged& /*c*/)
    {
        // TODO(workspace-slice-4): workspace-switch + active-tab focus.
        // Issue #21.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::ContentMounted& /*c*/)
    {
        // Phase 1: classic _CreateNewTabFromPane already materialises
        // the IPaneContent during the TabAdded arm. The dedicated
        // ContentRegistry/mount lifecycle lands in Phase 2 Slice 4.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::ContentUnmounted& /*c*/)
    {
        // TODO(workspace-phase-2-slice-4): ContentRegistry. Issue #20+.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabDecorationUpdated& /*c*/)
    {
        // TODO(workspace-slice-6): rename / color / pin decoration.
        // Issue #23.
    }
}
