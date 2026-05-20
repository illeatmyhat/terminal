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
    // Each apply() overload corresponds to one WorkspaceChange arm. Arms
    // that a migrated Phase 1 action can actually emit carry real logic;
    // the rest are intentional stubs that later slices fill in.
    // -------------------------------------------------------------------

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceAdded& /*c*/)
    {
        // Phase 1: one model workspace == one classic window-level tab.
        // The classic tab is materialised by the TabAdded arm; there is
        // no separate workspace-level XAML object yet (that lands in
        // Phase 2's sidebar slice).
    }

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceRemoved& c)
    {
        // Phase 1: one classic window-level tab per model workspace.
        // The page keeps a WorkspaceId -> classic Tab registry that the
        // TabAdded arm populates after _openDefaultTabForWorkspace.
        // Removing a workspace from the model means we should tear down
        // the matching classic tab (which in turn fires
        // CloseWindowRequested when it's the last tab — the cascade's
        // window-close behaviour falls out of the existing _RemoveTab
        // path with no additional handling).
        auto page = _page();
        if (!page)
        {
            return;
        }
        page->_removeClassicTabForRemovedWorkspace(c.id);
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
        // is the only XAML we own at Phase 1. Slice 2 routed the
        // default-profile TerminalSpec case; Slice 6 adds the explicit-
        // profile dispatch. Non-TerminalSpec content kinds (Settings,
        // Snippets, Markdown, Scratchpad) still have no user-action
        // entry point in classic Terminal and are not exercised here.
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
            // TODO(workspace-phase-2-slice-4): non-TerminalSpec
            // dispatch (Settings / Snippets / Markdown / Scratchpad).
            return;
        }

        const auto& spec = std::get<::WorkspaceModel::TerminalSpec>(record->description);

        // Locate the workspace that owns this tab BEFORE creating the
        // classic Tab, so the registry binding step has the id ready.
        // Phase 1 holds one tab per leaf per workspace, so the scan is
        // bounded by workspaces.size().
        ::WorkspaceModel::WorkspaceId owningWs{};
        for (const auto& ws : _state->workspaces_view())
        {
            for (const auto* leaf : _state->leaves(ws.id))
            {
                for (const auto& t : leaf->tabs)
                {
                    if (t.id == c.id)
                    {
                        owningWs = ws.id;
                        break;
                    }
                }
                if (owningWs.valid())
                {
                    break;
                }
            }
            if (owningWs.valid())
            {
                break;
            }
        }

        // The zero GUID is the model's "no explicit profile" sentinel:
        // ask the classic path for the default profile (Slice 2);
        // otherwise dispatch the explicit profile (Slice 6). Both helpers
        // return the newly-appended Tab, or nullptr if _OpenNewTab didn't
        // actually add one (spawn failure). Passing that exact Tab into
        // the registry — rather than inferring it from _tabs.back() —
        // prevents mis-binding the new workspace to a pre-existing Tab
        // when _OpenNewTab bails after at least one tab is already on
        // screen.
        const ::WorkspaceModel::TerminalSpec defaultSentinel{};
        const auto newTab = (spec == defaultSentinel)
                                ? page->_openDefaultTabForWorkspace()
                                : page->_openProfileTabForWorkspace(spec.profile);

        if (owningWs.valid() && newTab)
        {
            page->_registerClassicTabForWorkspace(owningWs, newTab);
        }
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabRemoved& /*c*/)
    {
        // Phase 1 maps one tab per leaf per workspace, so a tab close
        // always cascades to WorkspaceRemoved (which carries the classic
        // teardown). TabRemoved on its own only fires when a leaf has
        // multiple tabs — Phase 2 Slice 9 lifts that constraint and
        // wires the per-leaf TabView teardown.
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
        // Phase 1: the classic Tab teardown (driven by the
        // WorkspaceRemoved arm via _RemoveTab -> tab.Shutdown -> Pane
        // -> _setPaneContent(nullptr)) already disposes IPaneContent.
        // The dedicated ContentRegistry mount/unmount lifecycle lands in
        // Phase 2 Slice 4.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabDecorationUpdated& c)
    {
        // Phase 1 maps each model workspace 1:1 to a classic window-
        // level tab and pins exactly one model tab per workspace, so
        // the workspace's display index doubles as the classic tab
        // index. Locate the workspace whose leaf contains this TabId,
        // then route the rename/color back to the classic Tab.
        //
        // Pinning is carried by the model but has no classic XAML
        // surface yet — the dedicated pin glyph lands in Phase 2.
        auto page = _page();
        if (!page || !_state)
        {
            return;
        }

        const auto& workspaces = _state->workspaces_view();
        for (std::size_t idx = 0; idx < workspaces.size(); ++idx)
        {
            const auto& ws = workspaces[idx];
            const auto leaves = _state->leaves(ws.id);
            bool found = false;
            for (const auto* leaf : leaves)
            {
                for (const auto& t : leaf->tabs)
                {
                    if (t.id == c.id)
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    break;
                }
            }
            if (!found)
            {
                continue;
            }
            page->_applyTabDecoration(static_cast<uint32_t>(idx), c.customTitle, c.runtimeColor);
            return;
        }
    }
}
