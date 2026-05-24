// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include <algorithm>

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

    // Big-flip Slice B (#54): parent the ACTIVE workspace's mounted content
    // into the page's (collapsed) WorkspaceContentHost. This stands up the
    // attach/swap-on-switch plumbing BEHIND the still-visible classic tab — it
    // changes NOTHING the user sees this slice (the host is Collapsed and the
    // classic _tabContent swap still owns the display).
    //
    // Resolves the single (this slice) ContentId the active workspace owns from
    // the _contentsByWorkspace reverse index the ContentMounted arm populated,
    // Find()s its live IPaneContent in the registry (the sole strong owner),
    // and asks the page to parent its GetRoot() as the host's sole child. We
    // take the LAST recorded content for the workspace: a re-mount appends, and
    // the active leaf's current content is what we want to show.
    //
    // A no-op — leaving _hostContentId untouched — when the page is gone, the
    // workspace owns no content yet, or the id no longer resolves (e.g. a
    // content torn down by a close). Single-leaf scope: the active workspace's
    // one content; multi-leaf attach lands in a later slice.
    void WorkspaceView::_showActiveWorkspaceContentInHost(::WorkspaceModel::WorkspaceId active)
    {
        auto page = _page();
        if (!page || !active.valid())
        {
            return;
        }

        const auto wsIt = _contentsByWorkspace.find(active);
        if (wsIt == _contentsByWorkspace.end() || wsIt->second.empty())
        {
            return;
        }

        const auto contentId = wsIt->second.back();
        const auto content = _contentRegistry.Find(contentId);
        if (!content)
        {
            return;
        }

        page->_attachContentToWorkspaceHost(content);
        _hostContentId = contentId;
    }

    // Big-flip Slice C (#54): swap the host's child to a SPECIFIC tab's content.
    // The per-tab analogue of _showActiveWorkspaceContentInHost — driven by
    // apply(ActiveTabChanged) so switching between a leaf's tabs shows the
    // newly-active tab's content. Resolves the tab's bound ContentId from
    // _contentByTab (the ContentMounted arm populated it), Find()s its live
    // IPaneContent in the registry (the sole strong owner), and asks the page to
    // parent its GetRoot() as the host's sole child. A no-op — leaving
    // _hostContentId untouched — when the page is gone, the tab has no bound
    // content (e.g. a not-yet-mounted inactive tab), or the id no longer
    // resolves. INVISIBLE this slice: the host is Collapsed.
    void WorkspaceView::_showTabContentInHost(::WorkspaceModel::TabId tabId)
    {
        auto page = _page();
        if (!page || !tabId.valid())
        {
            return;
        }

        const auto it = _contentByTab.find(tabId);
        if (it == _contentByTab.end())
        {
            return;
        }
        const auto contentId = it->second;
        const auto content = _contentRegistry.Find(contentId);
        if (!content)
        {
            return;
        }

        page->_attachContentToWorkspaceHost(content);
        _hostContentId = contentId;
    }

    // Big-flip Slice F-0 (#54): re-populate every projected leaf's per-leaf
    // content host with that leaf's content. The page owns the leaf->tab
    // projection (_leafContentTabs picks each leaf's active row, or its first row
    // when none is active yet); we own tab->content (_contentByTab + the
    // registry). For each (leaf, tab) we Find() the live IPaneContent and parent
    // its GetRoot() into the leaf's host. A leaf whose tab has no bound/owned
    // content (a tab not yet ContentMounted, or a torn-down id) is skipped — its
    // host stays empty until that content mounts. INVISIBLE: the tree lives in
    // the Collapsed host.
    void WorkspaceView::_reattachLeafContents()
    {
        auto page = _page();
        if (!page)
        {
            return;
        }

        for (const auto& [leaf, tab] : page->_leafContentTabs())
        {
            if (!tab.valid())
            {
                continue;
            }
            const auto it = _contentByTab.find(tab);
            if (it == _contentByTab.end())
            {
                continue;
            }
            const auto content = _contentRegistry.Find(it->second);
            if (!content)
            {
                continue;
            }
            const auto root = content.GetRoot();
            if (!root)
            {
                continue;
            }
            page->_attachContentToLeafHost(leaf, root);
        }
    }

    // Big-flip Slice F-0 (#54): rebuild the projected pane tree and immediately
    // re-attach each surviving leaf's active-tab content into its fresh host. A
    // rebuild discards the old per-leaf hosts and the content parented into them,
    // so the re-attach must follow every rebuild — pairing them here keeps every
    // arm that re-derives the tree from leaving empty leaf hosts behind.
    void WorkspaceView::_rebuildAndReattachLeafContents()
    {
        auto page = _page();
        if (!page)
        {
            return;
        }
        page->_rebuildActiveWorkspacePaneTree();
        _reattachLeafContents();
    }

    // -------------------------------------------------------------------
    // Each apply() overload corresponds to one WorkspaceChange arm. Arms
    // that a migrated Phase 1 action can actually emit carry real logic;
    // the rest are intentional stubs that later slices fill in.
    // -------------------------------------------------------------------

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceAdded& c)
    {
        // Phase 2 Slice 2 (#46) / Stage 2 (#52): each model workspace gets one
        // observable WorkspaceViewModel, appended in declared order.
        // WorkspaceAdded is emitted in diff Phase 1 (before any
        // ActiveWorkspaceChanged) and in workspace display order, so appending
        // here keeps the sidebar in the workspaces' declared top→bottom order
        // with no positional bookkeeping. name/color/pinned are the workspace's
        // initial render metadata carried on the arm.
        //
        // The classic window-level tab that also backs this workspace in
        // Phase 1 is still materialised by the TabAdded arm; this arm only
        // maintains the sidebar projection and drives nothing.
        auto page = _page();
        if (!page)
        {
            return;
        }
        page->_addWorkspaceVm(c.id, c.name, c.color, c.pinned);
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

        // Big-flip Slice E (#54): tear down the registry content this
        // workspace owns BEFORE the classic-tab teardown. A whole-workspace
        // close emits WorkspaceRemoved (NOT per-tab TabRemoved/ContentUnmounted
        // — diff() suppresses those for a removed workspace), so this arm is
        // the ONLY place a whole-workspace close can drop the factory-built
        // content; without it every workspace close leaks its ConPTY until the
        // window exits. _contentsByWorkspace is the reverse index the
        // ContentMounted arm populated. Remove() only drops the registry's
        // strong ref (the single place a content's ConPTY tears down) — it does
        // not itself fire a window-close or re-enter the model, so doing it
        // before _removeClassicTabForRemovedWorkspace (which CAN re-enter the
        // page via _RemoveTab) keeps the ordering safe. We also erase the
        // matching _contentByTab entries so a later TabRemoved for the same
        // content (which can't happen for this close, but is cheap to keep
        // consistent) finds nothing stale.
        if (const auto wsIt = _contentsByWorkspace.find(c.id); wsIt != _contentsByWorkspace.end())
        {
            for (const auto contentId : wsIt->second)
            {
                _contentRegistry.Remove(contentId);
                // Erase any tab -> content bindings that pointed at this
                // content. There is exactly one per content in Phase 1, but
                // scan defensively rather than assume a 1:1 inverse.
                for (auto it = _contentByTab.begin(); it != _contentByTab.end();)
                {
                    if (it->second == contentId)
                    {
                        it = _contentByTab.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            _contentsByWorkspace.erase(wsIt);
        }

        // Phase 2 Slice 2 (#46) / Stage 2 (#52): drop the view-model that
        // mirrored this workspace (located by id, not slot), then tear down the
        // classic tab. Order is not load-bearing — the view-model carries no
        // model state — but removing it first keeps the projection consistent
        // before the tab teardown can re-enter the page.
        page->_removeWorkspaceVm(c.id);
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

        // Phase 2 Slice 2 (#46) / Stage 2 (#52): move the sidebar's active-row
        // highlight to the newly-active workspace by flipping IsActive on the
        // matching view-model. Resolved by id identity (the view-model carries
        // its WorkspaceId), never by a positional index into the workspace
        // list. Done before the tab-selection resolve below so a workspace that
        // has a view-model but no resolvable classic tab still gets its
        // highlight updated.
        page->_setActiveWorkspaceVm(*c.id);

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

        // Big-flip Slice B (#54): AFTER the classic _SelectTab swap above (which
        // we intentionally leave intact — it still owns the visible display),
        // swap the (collapsed) WorkspaceContentHost's child to the newly-active
        // workspace's content. _SelectTab -> _UpdatedSelectedTab cleared
        // _tabContent's children (including the host); the attach helper
        // re-appends the host before parenting, so the plumbing is restored.
        // The user sees no change: the host is Collapsed and the classic
        // content is what _tabContent now displays.
        _showActiveWorkspaceContentInHost(*c.id);

        // Big-flip Slice D (#54): the projected pane tree is per-active-workspace,
        // so re-derive it for the newly-active workspace's `root`. INVISIBLE
        // (host Collapsed); the classic tab/pane tree stays the visible display.
        // Big-flip Slice F-0 (#54): re-attach each leaf's active-tab content into
        // its fresh per-leaf host after the rebuild (the rebuild discarded the
        // old hosts), so a switch back to a SPLIT workspace re-populates every
        // leaf's cell, not just the single shared host above.
        _rebuildAndReattachLeafContents();
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
        //
        // This drives the CLASSIC _SplitPane — the VISIBLE split this slice. We
        // LEAVE it intact: the classic Pane tree is what the user sees until
        // Slice F flips the host visible.
        page->_splitFocusedPaneForWorkspace(c.parent->axis, c.parent->ratio);

        // Big-flip Slice D (#54): now that the new sibling leaf exists in the
        // model, rebuild the (Collapsed) projected pane tree so the sibling's
        // leaf container appears nested under the projected split Grid. The
        // sibling's strip row is appended by the subsequent TabAdded arm into the
        // leaf's observable collection, which this rebuilt container's ListView
        // is already bound to — so the row flows in via the binding without a
        // further rebuild. INVISIBLE (host Collapsed); purely additive to the
        // classic split above.
        // Big-flip Slice F-0 (#54): re-attach each leaf's content into its fresh
        // per-leaf host after the rebuild. The new sibling's content arrives via
        // its own ContentMounted (which also re-attaches), but the EXISTING
        // leaf's content must be re-parented into ITS fresh host now — the
        // rebuild moved it from the single root container into a split cell.
        _rebuildAndReattachLeafContents();
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitPaneCreated& /*c*/)
    {
        // The classic Pane tree inside the workspace's window-level tab is still
        // the VISIBLE split (driven by apply(LeafPaneCreated) -> classic
        // _SplitPane). Big-flip Slice D (#54) ADDITIONALLY projects the model's
        // split topology into nested XAML inside the (Collapsed)
        // WorkspacePaneTreeRoot — invisible scaffolding F will make visible. We
        // rebuild the whole active-workspace pane tree from the model's `root`:
        // a rebuild keeps the projection structurally identical to the model and
        // reuses each leaf's existing strip collection by PaneId.
        //
        // SplitPaneCreated arrives in diff Phase 1 BEFORE the new sibling's
        // LeafPaneCreated/TabAdded, so the sibling leaf's strip collection may
        // not exist yet at this point — but the LeafPaneCreated arm rebuilds
        // again after the sibling is created, and the per-leaf strip helper
        // creates the collection on first use, so the final projection is
        // correct regardless of which arm rebuilds. INVISIBLE (host Collapsed).
        // Big-flip Slice F-0 (#54): re-attach each surviving leaf's content into
        // its fresh per-leaf host after the rebuild (the rebuild discards the old
        // hosts + their parented content).
        _rebuildAndReattachLeafContents();
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitPaneCollapsed& /*c*/)
    {
        // The classic Pane tree collapses single-child splits internally when a
        // sibling is removed (see Pane::Close / Tab::DetachPane) — that stays the
        // VISIBLE collapse. Big-flip Slice D (#54): re-derive the (Collapsed)
        // projected pane tree from the model's now-collapsed `root`, so the
        // projection's split Grid is replaced by the surviving child's container
        // (the model already lifted the survivor; the rebuild mirrors it). The
        // collapsed-away leaf's strip collection is left in place — leaf-strip
        // GC is a later concern — but its container is no longer in the tree.
        // INVISIBLE (host Collapsed).
        // Big-flip Slice F-0 (#54): re-attach each surviving leaf's content into
        // its fresh per-leaf host after the rebuild (the rebuild discards the old
        // hosts + their parented content).
        _rebuildAndReattachLeafContents();
    }

    void WorkspaceView::apply(const ::WorkspaceModel::SplitRatioChanged& /*c*/)
    {
        // The classic Pane tree continues to own the VISIBLE rendered split
        // ratio (drag-separator + ResizeDirection keyboard moves update it
        // directly). Big-flip Slice D (#54): re-derive the (Collapsed) projected
        // pane tree so the projected split Grid's two cells' star sizes follow
        // the model's new ratio (ratio / 1-ratio). A full rebuild is cheap and
        // keeps the one source of truth (the model `root`) authoritative; F can
        // optimise to an in-place ColumnDefinition/RowDefinition star update if
        // needed. INVISIBLE (host Collapsed).
        // Big-flip Slice F-0 (#54): re-attach each surviving leaf's content into
        // its fresh per-leaf host after the rebuild (the rebuild discards the old
        // hosts + their parented content).
        _rebuildAndReattachLeafContents();
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

        // Big-flip Slice C (#54): the per-leaf tab strip is the invisible MVVM
        // projection of a leaf's tabs. Append one strip view-model for this tab
        // BEFORE any classic-tab branching below — the strip is a pure model
        // projection, independent of whether the classic path also materialises
        // a window-level Tab. It lives inside the still-Collapsed
        // WorkspaceContentHost, so this is INVISIBLE this slice (the classic tab
        // stays the only thing on screen).
        //
        // Big-flip Slice D (#54): we now ALSO project the strip for a split
        // sibling leaf (leafInsideSplit) — Slice C skipped these and deferred
        // them to D, which owns the nested split tree. The strip VM append is
        // independent of the classic-tab gate below (a split sibling still
        // creates NO second classic Tab; the classic _SplitPane path drives the
        // visible split). The per-leaf strip is what the leaf's container in the
        // rebuilt pane tree binds to, so every leaf — root or split sibling —
        // gets its row here.
        if (c.leafId.valid())
        {
            page->_appendPaneTabVm(c.leafId, c.id, c.customTitle);
        }

        // Big-flip Slice D (#54): rebuild the (Collapsed) projected pane tree so
        // the active workspace's tree (including this leaf's container, with its
        // newly-appended strip row) is reflected. This is what stands up the
        // INITIAL single-leaf projection too — the startup tab's TabAdded builds
        // the root leaf's container before any split — and it tracks additional
        // tabs growing a leaf's strip. A rebuild reuses the leaf's observable
        // strip collection by PaneId, so the row appended just above is carried
        // through. Done before the classic-tab branching below (which is
        // unchanged); harmless if a split arm already rebuilt this turn (the
        // rebuild is idempotent and re-uses the same collections). INVISIBLE
        // (host Collapsed).
        // Big-flip Slice F-0 (#54): re-attach each leaf's active-tab content into
        // its fresh per-leaf host after the rebuild. For this tab's OWN content
        // the binding may not exist yet (its ContentMounted fires later this turn
        // and re-attaches then); this call re-populates the OTHER leaves' hosts
        // the rebuild just discarded.
        _rebuildAndReattachLeafContents();

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

        // Big-flip Slice C (#54): distinguish a NEW workspace's FIRST tab from
        // an ADDITIONAL tab in an existing leaf. The model emits TabAdded with
        // leafInsideSplit==false for BOTH (a newTab on the root leaf has no
        // parent), so the arm's own fields can't tell them apart — but the
        // owning workspace already has a registered classic Tab ONLY in the
        // additional-tab case: newWorkspace fires WorkspaceAdded (which does NOT
        // register a tab) before its first TabAdded, whereas newTab fires no
        // WorkspaceAdded against an already-registered workspace. For an
        // ADDITIONAL tab the strip VM appended above is the SOLE representation
        // — we must NOT create a second classic Tab (that would be visible AND
        // wrong). Only the new-workspace first-tab path creates the classic Tab,
        // exactly as before.
        if (c.owningWorkspace.valid() && page->_workspaceHasClassicTab(c.owningWorkspace))
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
        // WorkspaceRemoved (NOT TabRemoved).
        //
        // Big-flip Slice E (#54): apply(WorkspaceRemoved) now tears down the
        // whole-workspace-close content via the _contentsByWorkspace reverse
        // index, so the dominant close path no longer leaks. The two paths are
        // disjoint: diff() suppresses TabRemoved for a removed workspace, so a
        // given content is torn down by exactly one of the two arms — never
        // both — and there is no double-Remove.
        _removeContentForTab(c.id);

        // Big-flip Slice C (#54): drop the strip view-model that mirrored this
        // tab in its leaf, located by id identity. The arm carries the leafId,
        // so the strip removal is independent of the content teardown above.
        // INVISIBLE this slice (the strip lives in the Collapsed host). A
        // whole-workspace close emits WorkspaceRemoved (NOT TabRemoved), so this
        // only fires for an additional-tab close in a multi-tab leaf — exactly
        // the strip-VM-only case the TabAdded arm created without a classic Tab.
        if (auto page = _page())
        {
            page->_removePaneTabVm(c.leafId, c.id);
        }
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

    void WorkspaceView::apply(const ::WorkspaceModel::ActiveTabChanged& c)
    {
        // ActiveTabChanged fires when a LEAF's activeTabIdx changes (i.e. the
        // user switched between tabs that share a single pane). Big-flip Slice C
        // (#54) makes this real for the per-leaf tab strip: flip the strip's
        // active row to the newly-active tab, and swap the (collapsed) host's
        // child to that tab's content GetRoot (extending Slice B's single-content
        // attach to per-tab). All single-leaf-aware — splits land in Slice D.
        //
        // Cross-classic-tab selection on the flag-on path is still driven by
        // ActiveWorkspaceChanged; this arm only owns the WITHIN-leaf strip
        // selection + content swap. INVISIBLE this slice: the strip + host are
        // Collapsed, so the classic tab stays the only thing on screen.
        auto page = _page();
        if (!page)
        {
            return;
        }

        const auto active = page->_activatePaneTabByIndex(c.leafId, c.idx);
        if (!active.valid())
        {
            return;
        }
        _showTabContentInHost(active);

        // Big-flip Slice F-0 (#54): _activatePaneTabByIndex flipped this leaf's
        // active strip row to `active`, so re-attach each leaf's active-tab
        // content into its per-leaf host — this leaf's host now shows the
        // newly-active tab's content, and the other leaves' hosts are re-affirmed
        // (idempotent). The single shared host swap above (Slice C) is retained;
        // the per-leaf attach is what makes a SPLIT leaf's tab switch show in its
        // own cell. INVISIBLE (host Collapsed).
        _reattachLeafContents();
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
        // ContentId to erase, AND (Big-flip Slice E, #54) the workspace ->
        // contents reverse index (_contentsByWorkspace, keyed by the arm's
        // owningWorkspace) so the dominant whole-workspace close path
        // (WorkspaceRemoved) can tear down every content it owns. ContentUnmounted
        // is NOT a removal — it only detaches for an inactive workspace.
        //
        // What this slice does NOT do: the actual XAML reparent into the active
        // workspace's leaf (which workspace, which Pane) is driven by S4 (#48,
        // workspace switching). The registry + binding stood up here are what
        // S4 attaches; the live content's lifetime is owned here regardless of
        // whether it is currently parented into the tree.
        // Big-flip Slice A (#54): the factory is real. It materialises the live
        // IPaneContent from the spec via the owning page, so the registry
        // genuinely owns one live content per active tab. The spec is captured
        // BY VALUE — the change `c` is a reference that need not outlive a
        // deferred factory call — and the page WEAKLY (the factory must not keep
        // the page alive; if the page is gone there is nothing to mount). A null
        // return (non-Terminal spec, spawn failure, or elevation handoff) leaves
        // the registry untouched, per the EnsureMounted contract.
        //
        // NOTE: this changes NO display ownership. The classic Tab is still the
        // sole displayer; this arm only stands up the registry-owned content.
        // The XAML reparent into the active workspace's leaf is still S4 (#48).
        auto weakPage = _owner;
        const auto live = _contentRegistry.EnsureMounted(
            c.contentId,
            [weakPage, desc = c.description]() -> winrt::TerminalApp::IPaneContent {
                auto page = weakPage.get();
                return page ? page->_makePaneContentForSpec(desc) : nullptr;
            });
        if (live)
        {
            _contentByTab[c.tabId] = c.contentId;

            // Big-flip Slice E (#54): record the workspace -> contents reverse
            // index so apply(WorkspaceRemoved) can tear down every content a
            // whole-workspace close orphans. Guard against a duplicate id: a
            // re-mount of an already-owned ContentId (the EnsureMounted
            // keep-alive path) re-arrives with the same (workspace, content)
            // pair, and we must not double-record it or a later Remove would
            // leave a stale entry.
            if (c.owningWorkspace.valid())
            {
                auto& contents = _contentsByWorkspace[c.owningWorkspace];
                if (std::find(contents.begin(), contents.end(), c.contentId) == contents.end())
                {
                    contents.push_back(c.contentId);
                }
            }

            // Big-flip Slice B (#54): now that the content is recorded + owned,
            // parent it into the (collapsed) WorkspaceContentHost so the host
            // always backs the active workspace's content. A new workspace's
            // ContentMounted fires before its ActiveWorkspaceChanged, and the
            // newly-mounted content belongs to the workspace that is becoming
            // active, so attaching the owning workspace's content here is
            // correct. ActiveWorkspaceChanged re-attaches on a later switch.
            // This is purely additive: it does NOT touch the classic display.
            _showActiveWorkspaceContentInHost(c.owningWorkspace);

            // Big-flip Slice F-0 (#54): ALSO parent this freshly-mounted
            // content into its OWN leaf's per-leaf content host, so a SPLIT
            // workspace renders each leaf's terminal in its own cell. The
            // TabAdded arm (which ran before this ContentMounted) rebuilt the
            // tree, created this leaf's host, and appended its strip VM; the
            // tab->content binding was just recorded above; so re-attaching every
            // leaf's content now places this content into its leaf's host via the
            // first-row fallback (the startup / split-sibling first tab is not
            // yet active — that IsActive seed is F-2 — but the mounted content IS
            // that leaf's content). INVISIBLE: the hosts live in the Collapsed
            // WorkspaceContentHost.
            _reattachLeafContents();
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
        // switch. Teardown is TabRemoved's job (and the WorkspaceRemoved close
        // path, wired in Big-flip Slice E #54); this arm only notes the
        // keep-alive.
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

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceMetadataUpdated& c)
    {
        // Stage 2 (#52): a surviving workspace's display metadata (name / color
        // / pin) changed. The workspace analogue of TabDecorationUpdated:
        // re-project the new metadata onto the matching view-model, located by
        // id identity. The bound DataTemplate re-renders from the observable
        // property changes — no positional cast, so an update can never land on
        // the wrong row.
        auto page = _page();
        if (!page)
        {
            return;
        }
        page->_updateWorkspaceVm(c.id, c.name, c.color, c.pinned);
    }

    void WorkspaceView::apply(const ::WorkspaceModel::WorkspaceReordered& c)
    {
        // Pinned-float: the model's sidebar display order changed (e.g. a pin
        // floated a workspace within the list). Re-sequence the observable
        // view-model collection to match the new id-order the arm carries. This
        // is a pure projection: the page resolves each id to its existing row
        // view-model and moves it into place; no model state is read to decide.
        auto page = _page();
        if (!page)
        {
            return;
        }
        page->_reorderWorkspaceVms(c.order);
    }
}
