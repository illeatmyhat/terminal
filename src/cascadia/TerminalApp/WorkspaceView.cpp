// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "WorkspaceView.h"
#include "TerminalPage.h"

namespace winrt::TerminalApp::implementation
{
    WorkspaceView::WorkspaceView(winrt::weak_ref<TerminalPage> owner) noexcept :
        _owner{ std::move(owner) }
    {
    }

    void WorkspaceView::setState(::WorkspaceModel::ModelState state) noexcept
    {
        _state = std::move(state);
    }

    winrt::com_ptr<TerminalPage> WorkspaceView::_page() const
    {
        return _owner.get();
    }

    // -------------------------------------------------------------------
    // Phase 1 progress:
    //   Slice 2: TabAdded routes default-profile new-tab.
    //   Slice 4: ActiveWorkspaceChanged routes classic-tab selection.
    // Remaining arms are intentional stubs — subsequent slices fill them in.
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

    void WorkspaceView::apply(const ::WorkspaceModel::ActiveWorkspaceChanged& c)
    {
        // Phase 1 maps one model workspace == one classic window-level tab.
        // Switching the active workspace therefore corresponds to selecting
        // the classic tab whose index in _tabs matches the new active
        // workspace's index in workspaces_view(). When activeWorkspaceId is
        // std::nullopt (the empty-model case), there is no classic tab to
        // select.
        auto page = _page();
        if (!page || !_state)
        {
            return;
        }
        if (!c.id.has_value())
        {
            return;
        }

        const auto& workspaces = _state->workspaces_view();
        for (std::size_t i = 0; i < workspaces.size(); ++i)
        {
            if (workspaces[i].id == *c.id)
            {
                // _SelectTab is responsible for both _tabView.SelectedItem
                // mutation and the focus-tracking downstream (see the
                // _UpdatedSelectedTab / _SetFocusedTab branches in its
                // implementation). This is the same entry point clicks on
                // a TabViewItem already use on the flag-off path.
                //
                // Skip when the target tab is already selected. The Slice-2
                // new-workspace case fires TabAdded immediately before this
                // ActiveWorkspaceChanged; the classic _OpenNewTab inside
                // TabAdded already selected the new tab via
                // _tabView.SelectedItem(newItem). Re-entering _SelectTab
                // here would post a redundant _SetFocusedTab dispatcher hop.
                if (i < page->_tabs.Size())
                {
                    const auto idx = static_cast<uint32_t>(i);
                    if (page->_GetFocusedTabIndex() != idx)
                    {
                        page->_SelectTab(idx);
                    }
                }
                return;
            }
        }
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
        // ActiveTabChanged fires when a LEAF's activeTabIdx changes (i.e.
        // the user switched between tabs that share a single pane). Phase 1
        // holds exactly one tab per leaf, so this arm is unreachable from
        // the migrated actions in this slice. Per-leaf tab strips (the
        // case that exercises this arm) land in Phase 2 slices 9-10.
        //
        // Cross-classic-tab selection on the flag-on path is driven by
        // ActiveWorkspaceChanged above; ActiveTabChanged here intentionally
        // stays a no-op until per-leaf tab strips exist.
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
