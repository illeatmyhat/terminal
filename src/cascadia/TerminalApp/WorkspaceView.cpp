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

    winrt::com_ptr<TerminalPage> WorkspaceView::_page() const
    {
        return _owner.get();
    }

    // Phase 2 id-resolver foundation (#45/#44). The view owns the id->XAML
    // mapping: it resolves a stable WorkspaceId to the CURRENT display index
    // of its classic Tab via the page's WorkspaceId->Tab registry, never via
    // a positional cast of a display index the model handed it. Any failure
    // (page gone, unknown id, expired Tab, Tab no longer in _tabs) returns
    // std::nullopt so the caller can skip the apply explicitly rather than
    // route to the wrong tab.
    std::optional<std::uint32_t> WorkspaceView::_resolveClassicTabIndex(::WorkspaceModel::WorkspaceId ws) const
    {
        auto page = _page();
        if (!page)
        {
            return std::nullopt;
        }
        return page->_classicTabIndexForWorkspace(ws);
    }

    // Phase 2 Slice 3 (#47): the removal path. The removal arms carry only a
    // TabId; this resolves the tab's bound ContentId (recorded by the
    // ContentMounted arm) and erases it from the registry — the ONLY place a
    // single content's ConPTY tears down. A no-op when the tab had no mounted
    // content (e.g. a not-yet-materialised tab, or a flag-on Phase-1 tab whose
    // content the classic path still owns).
    void WorkspaceView::_removeContentForTab(::WorkspaceModel::TabId tabId)
    {
        const auto it = _contentByTab.find(tabId);
        if (it == _contentByTab.end())
        {
            return;
        }
        const auto contentId = it->second;
        _contentByTab.erase(it);
        _contentRegistry.Remove(contentId);
    }

    // -------------------------------------------------------------------
    // Each apply() overload corresponds to one WorkspaceChange arm. Arms
    // that a migrated Phase 1 action can actually emit carry real logic;
    // the rest are intentional stubs that later slices fill in.
    // -------------------------------------------------------------------

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceAdded& c)
    {
        // Phase 2 Slice 2 (#46): each model workspace gets one read-only
        // sidebar row, appended in declared order. WorkspaceAdded is emitted
        // in diff Phase 1 (before any ActiveWorkspaceChanged) and in workspace
        // display order, so appending here keeps the sidebar in the workspaces'
        // declared top→bottom order with no positional bookkeeping.
        //
        // The classic window-level tab that also backs this workspace in
        // Phase 1 is still materialised by the TabAdded arm; this arm only
        // maintains the sidebar projection and drives nothing.
        auto page = _page();
        if (!page)
        {
            return;
        }
        page->_addWorkspaceSidebarRow(c.id, c.name);
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
        // Phase 2 Slice 2 (#46): drop the read-only sidebar row that mirrored
        // this workspace (located by id, not slot), then tear down the classic
        // tab. Order is not load-bearing — the sidebar row carries no model
        // state — but removing it first keeps the projection consistent before
        // the tab teardown can re-enter the page.
        page->_removeWorkspaceSidebarRow(c.id);
        page->_removeClassicTabForRemovedWorkspace(c.id);
    }

    void WorkspaceView::apply(const ::WorkspaceModel::ActiveWorkspaceChanged& c)
    {
        // Switching the active workspace corresponds to selecting the classic
        // tab that backs the newly-active workspace. diff() carries the
        // workspace's stable id (std::nullopt for the empty-model case, where
        // there is no tab to select); we resolve it to the CURRENT classic
        // tab index through the view-owned resolver instead of trusting a
        // positional display index. An unknown / stale id resolves to
        // std::nullopt and we skip — no out-of-range or wrong-tab routing.
        auto page = _page();
        if (!page || !c.id.has_value())
        {
            return;
        }

        // Phase 2 Slice 2 (#46): move the read-only sidebar's active-row
        // highlight to the newly-active workspace. Resolved by id identity
        // (the row stores its WorkspaceId), never by a positional index into
        // the workspace list. Done before the tab-selection resolve below so a
        // workspace that has a sidebar row but no resolvable classic tab still
        // gets its highlight updated.
        page->_highlightActiveWorkspaceSidebarRow(*c.id);

        const auto resolved = _resolveClassicTabIndex(*c.id);
        if (!resolved.has_value())
        {
            return;
        }
        const auto idx = *resolved;

        // _SelectTab is responsible for both _tabView.SelectedItem mutation
        // and the focus-tracking downstream (see the _UpdatedSelectedTab /
        // _SetFocusedTab branches in its implementation). This is the same
        // entry point clicks on a TabViewItem already use on the flag-off
        // path.
        //
        // Skip when the target tab is already selected. The new-workspace
        // case fires TabAdded immediately before this ActiveWorkspaceChanged;
        // the classic _OpenNewTab inside TabAdded already selected the new
        // tab via _tabView.SelectedItem(newItem). Re-entering _SelectTab here
        // would post a redundant _SetFocusedTab dispatcher hop.
        if (page->_GetFocusedTabIndex() != idx)
        {
            page->_SelectTab(idx);
        }
    }

    void WorkspaceView::apply(const ::WorkspaceModel::LeafPaneCreated& c)
    {
        // Phase 1 split topology: a non-root leaf appearing in an
        // existing workspace means the user just split the workspace's
        // focused pane. The classic representation is "add a sibling
        // Pane inside the window-tab that backs the workspace".
        //
        // The split's axis and ratio come from the parent SplitPane node;
        // the new sibling always lives on the right/bottom (matching
        // WorkspaceActions::splitPane semantics).
        //
        // A leaf that has no parent is either the workspace's root (the
        // implicit leaf created by newWorkspace; the classic tab is
        // materialised by TabAdded) or a brand-new workspace, which is
        // also handled by TabAdded. Skip in either case.
        if (!c.parent.has_value())
        {
            return;
        }
        auto page = _page();
        if (!page)
        {
            return;
        }
        // The containing split's axis/ratio ride along on the change; a
        // leaf with a parent is always nested under a SplitPane, so no
        // node-kind re-resolution is needed.
        page->_splitFocusedPaneForWorkspace(c.parent->axis, c.parent->ratio);
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitPaneCreated& /*c*/)
    {
        // Phase 1 keeps the visible split topology rendered by the classic
        // Pane tree inside the workspace's window-level tab. The structural
        // split is materialised by apply(LeafPaneCreated) when the new
        // sibling leaf is created; the SplitPane node itself has no
        // dedicated XAML representation in Phase 1, so there is nothing
        // for this arm to mutate.
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitPaneCollapsed& /*c*/)
    {
        // The classic Pane tree collapses single-child splits internally
        // when a sibling is removed (see Pane::Close / Tab::DetachPane).
        // Phase 1's view layer does not need to re-drive that collapse —
        // the model + classic representations end up consistent because
        // both reach the same single-leaf shape from a shared origin
        // (TabRemoved or TabMoved).
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitRatioChanged& /*c*/)
    {
        // Phase 1: the classic Pane tree continues to own the rendered
        // split ratio (drag-separator + ResizeDirection keyboard moves
        // update it directly). The model arm exists so Phase 3 persistence
        // sees ratio changes; no XAML mutation is needed here yet.
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
        if (!page)
        {
            return;
        }

        if (!std::holds_alternative<::WorkspaceModel::TerminalSpec>(c.description))
        {
            // TODO(workspace-phase-2-slice-4): non-TerminalSpec
            // dispatch (Settings / Snippets / Markdown / Scratchpad).
            return;
        }

        // If this leaf lives inside a split, it was just created as the
        // sibling of an existing leaf — apply(LeafPaneCreated) has already
        // driven the classic _SplitPane call (which creates the live
        // Pane + TermControl) for it. Don't fire an additional new-tab,
        // and don't bind a new workspace registry entry.
        if (c.leafInsideSplit)
        {
            return;
        }

        const auto& spec = std::get<::WorkspaceModel::TerminalSpec>(c.description);

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

        // The owning workspace rides along on the change (diff resolved it
        // against the post-action state). Phase 1 holds one tab per leaf
        // per workspace, so this is the workspace to bind the classic Tab to.
        if (c.owningWorkspace.valid() && newTab)
        {
            page->_registerClassicTabForWorkspace(c.owningWorkspace, newTab);
        }
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabRemoved& c)
    {
        // Phase 1 maps one tab per leaf per workspace, so a tab close
        // always cascades to WorkspaceRemoved (which carries the classic
        // teardown). TabRemoved on its own only fires when a leaf has
        // multiple tabs — Phase 2 Slice 9 lifts that constraint and
        // wires the per-leaf TabView teardown.
        //
        // Phase 2 Slice 3 (#47): a TabRemoved means this tab — and therefore
        // its content — is structurally destroyed, so erase the registry entry
        // (the ConPTY teardown for that content). NOTE this only fires when a
        // leaf has MULTIPLE tabs; Phase 1 holds one tab per leaf, so the
        // DOMINANT close path is a whole-workspace close, which diff() emits as
        // WorkspaceRemoved (NOT TabRemoved). apply(WorkspaceRemoved) does NOT
        // yet erase registry content — that teardown, plus a workspace->contents
        // reverse index, is an S4 (#48) prerequisite tracked on that issue.
        // Until S4 makes the factory real nothing is ever inserted, so neither
        // path can leak today.
        _removeContentForTab(c.id);
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabMoved& c)
    {
        // CONTRACT-ONLY STUB until Phase 2 wires user-reachable cross-leaf
        // moves. This arm is intentionally empty in Phase 1.
        //
        // Identity-preserving move. The model carries the source and
        // destination leaf ids plus the destination tab index, and the
        // moved TabId is the same as it was pre-move — diff() emits a
        // single TabMoved (NOT TabRemoved + TabAdded) which is the
        // signal that lets a view-layer apply arm preserve the live
        // IPaneContent.
        //
        // What Phase 1 actually proves: the MoveTab_FlagOn_* tests call
        // WorkspaceModel::moveTab directly and assert the identity-
        // preserving DIFF CONTRACT (a single TabMoved is emitted). They do
        // NOT exercise the AC's UX-level claim that a live TermControl /
        // ConPTY / scrollback survives a move, because no user-reachable
        // Phase 1 action emits TabMoved — the classic _MoveTab + _MovePane
        // entry points still own the visible reparent (DetachPane +
        // AttachPane already preserve the live TermControl + ConPTY there).
        //
        // Phase 2 lifts the "one tab per leaf" rule and replaces this stub
        // with a real AttachPane call; the UX-survival claim is verified
        // then, against a user-reachable cross-leaf move.
        (void)c;
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

    void WorkspaceView::apply(const ::WorkspaceModel::ContentMounted& c)
    {
        // Phase 2 Slice 3 (#47): bind the model's ContentId to a live
        // IPaneContent in the registry — the single strong owner of every
        // live content in this window.
        //
        // EnsureMounted is the only way to obtain a mountable content: it
        // returns the SAME instance if `contentId` is already owned (so a
        // re-mount after a switch reattaches the live TermControl / ConPTY /
        // scrollback that the registry kept alive across the unmount), or
        // creates+inserts one from the spec when the id is new. Mounting an id
        // the registry does not own is therefore unrepresentable: there is no
        // path that attaches a bare ContentId with no content behind it.
        //
        // Record the tab -> content binding so TabRemoved can resolve which
        // ContentId to erase. (WorkspaceRemoved is the dominant Phase-1 close
        // path but does NOT yet erase registry content — that, plus a
        // workspace->contents reverse index, is an S4 (#48) prerequisite.)
        // ContentUnmounted is NOT a removal — it only detaches for an inactive
        // workspace.
        //
        // What this slice does NOT do: the actual XAML reparent into the active
        // workspace's leaf (which workspace, which Pane) is driven by S4 (#48,
        // workspace switching). The registry + binding stood up here are what
        // S4 attaches; the live content's lifetime is owned here regardless of
        // whether it is currently parented into the tree.
        const auto live = _contentRegistry.EnsureMounted(
            c.contentId,
            [&c]() -> winrt::TerminalApp::IPaneContent {
                // TODO(workspace-phase-2-slice-4): materialise the live
                // IPaneContent from c.description (TerminalSpec ->
                // _OpenNewTab content, Settings/Snippets/Markdown/Scratchpad
                // -> their content classes) and hand it back. Until S4 drives
                // a user-reachable mount, no diff path that constructs content
                // here is reachable; the contract is proved by the direct
                // ContentRegistry lifetime test instead.
                return nullptr;
            });
        if (live)
        {
            _contentByTab[c.tabId] = c.contentId;
        }
        (void)live;
    }

    void WorkspaceView::apply(const ::WorkspaceModel::ContentUnmounted& c)
    {
        // Phase 2 Slice 3 (#47): an unmount detaches the content from the
        // active visual tree but the registry KEEPS its strong ref, so the
        // TermControl / ConPTY / scrollback stays alive while the workspace is
        // inactive. This is the whole point of the registry: inactive content
        // survives detachment.
        //
        // Crucially this arm does NOT erase the registry entry — diff() emits
        // ContentUnmounted both on a switch-away (keep alive) and immediately
        // before TabRemoved when a tab is genuinely destroyed. Treating either
        // as a teardown here would kill a ConPTY that should have survived a
        // switch. Teardown is TabRemoved's job (and, once S4 (#48) wires it,
        // the WorkspaceRemoved close path); this arm only notes the keep-alive.
        //
        // The detach from the live tree (which Pane / leaf to unparent) is
        // driven by S4 (#48); pre-S4 there is no user-reachable switch to
        // exercise it.
        _contentRegistry.NoteUnmounted(c.contentId);
    }

    void WorkspaceView::apply(const ::WorkspaceModel::TabDecorationUpdated& c)
    {
        // diff() carries the stable id of the workspace that owns the
        // decorated tab; we resolve it to the CURRENT classic tab index
        // through the view-owned resolver and route the rename/color there.
        // An unknown / stale id resolves to std::nullopt and we skip — no
        // positional cast, so a decoration can never land on the wrong tab.
        //
        // Pinning is carried by the model but has no classic XAML
        // surface yet — the dedicated pin glyph lands in Phase 2.
        auto page = _page();
        if (!page)
        {
            return;
        }
        const auto resolved = _resolveClassicTabIndex(c.workspaceId);
        if (!resolved.has_value())
        {
            return;
        }
        page->_applyTabDecoration(*resolved, c.customTitle, c.runtimeColor);
    }
}
