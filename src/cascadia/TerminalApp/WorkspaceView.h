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

#include <optional>
#include <unordered_map>
#include <vector>

#include "../WorkspaceModel/IWorkspaceView.h"
#include "../WorkspaceModel/WorkspaceChange.h"

#include "ContentRegistry.h"

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
        void apply(const ::WorkspaceModel::WorkspaceMetadataUpdated& c) override;
        void apply(const ::WorkspaceModel::WorkspaceReordered& c) override;
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

        // Test-only observers of the content registry. The registry is the
        // single strong owner of every live IPaneContent; these let a page
        // test confirm the ContentMounted factory genuinely materialised
        // content (Size) and that a given model mount ContentId resolves to a
        // live entry (Contains), without driving any real geometry.
        [[nodiscard]] std::size_t contentRegistrySizeForTest() const noexcept { return _contentRegistry.Size(); }
        [[nodiscard]] bool contentRegistryContainsForTest(::WorkspaceModel::ContentId id) const noexcept { return _contentRegistry.Contains(id); }

        // Big-flip Slice B (#54): the ContentId currently attached into the
        // flag-on WorkspaceContentHost (the active workspace's content), or
        // std::nullopt when nothing has been attached. Lets a page test assert
        // the host's backing content flipped to the right workspace's content
        // on a switch, paired with the page's _workspaceHostChildForTest()
        // (which exposes the actual parented FrameworkElement) for identity.
        [[nodiscard]] std::optional<::WorkspaceModel::ContentId> hostContentIdForTest() const noexcept { return _hostContentId; }

    private:
        // Resolves the weak reference to a strong com_ptr each time. Returns
        // an empty com_ptr if the page is gone.
        winrt::com_ptr<TerminalPage> _page() const;

        // Phase 2 id-resolver foundation (#45/#44). Maps a stable
        // WorkspaceId to the current display index of its classic Tab in the
        // page's _tabs vector. Returns std::nullopt when `page` is gone or
        // the id is unknown / stale (no registry entry, expired Tab, or the
        // Tab is no longer in _tabs). The apply() arms route through this
        // instead of casting a positional display index, so a missing id is
        // handled explicitly and can never select/decorate the wrong tab.
        std::optional<std::uint32_t> _resolveClassicTabIndex(::WorkspaceModel::WorkspaceId ws) const;

        winrt::weak_ref<TerminalPage> _owner;

        // Phase 2 Slice 3 (#47): the single strong owner of every live
        // IPaneContent in this window, keyed by the model's ContentId. This is
        // the content arm of the S1 id-resolver — the ContentMounted /
        // ContentUnmounted / content-removal arms resolve and mutate it. An
        // unmounted (inactive-workspace) content stays owned here so its
        // TermControl / ConPTY survives detachment from the visual tree; the
        // entry is only erased — and its ConPTY only torn down — on removal.
        ContentRegistry _contentRegistry;

        // tab -> content binding, populated by the ContentMounted arm. The
        // removal arms (TabRemoved / WorkspaceRemoved) carry only a TabId, so
        // this is how they resolve which ContentId to erase from the registry.
        // A node-based map; the registry is the strong owner, this only maps
        // ids to ids.
        std::unordered_map<::WorkspaceModel::TabId, ::WorkspaceModel::ContentId> _contentByTab;

        // Live pane-tab title (#54): per-tab TitleChanged revoker for the strip
        // VM's title binding, keyed by TabId. auto_revoke fires on erase/destroy,
        // so dropping an entry (in _unbindTabTitle, on content teardown) detaches
        // the handler from the content; clearing the whole map on page/view
        // teardown detaches all of them. This guarantees no handler outlives the
        // tab it titles (and none outlives this view). Indexed by tab — the
        // ContentMounted arm carries a TabId and the content is 1:1 with it.
        std::unordered_map<::WorkspaceModel::TabId, winrt::TerminalApp::IPaneContent::TitleChanged_revoker> _tabTitleRevokers;

        // Big-flip Slice E (#54): workspace -> contents reverse index,
        // populated by the ContentMounted arm (keyed by the arm's
        // owningWorkspace). A whole-workspace close emits WorkspaceRemoved —
        // NOT per-tab TabRemoved/ContentUnmounted — so apply(WorkspaceRemoved)
        // can't reach the workspace's contents through _contentByTab alone
        // (it only carries a WorkspaceId). This is how that arm enumerates
        // every ContentId the closing workspace owns to Remove() it from the
        // registry (tearing down the ConPTY) — without it the factory-built
        // content leaks until the window exits.
        std::unordered_map<::WorkspaceModel::WorkspaceId, std::vector<::WorkspaceModel::ContentId>> _contentsByWorkspace;

        // Erase the content bound to `tabId` (if any) from the registry — the
        // only place a single content's ConPTY tears down. Called by the
        // removal arms. A no-op when the tab had no mounted content.
        void _removeContentForTab(::WorkspaceModel::TabId tabId);

        // Live pane-tab chrome (#54): bind the strip VM for `tabId` to follow the
        // mounted content's title AND background. Sets the VM's Title to
        // content.Title() and its Background to content.BackgroundBrush() now,
        // then subscribes to content.TitleChanged so every later title change
        // re-pushes BOTH content.Title() and content.BackgroundBrush() into the
        // VM — mirroring how the classic Tab derives its title from
        // content.Title(). Slice 2a.2 (#54): the background piggybacks on the
        // TitleChanged subscription because IPaneContent has no
        // BackgroundBrushChanged event and the bg changes rarely (a new event
        // surface/revoker is out of scope). The TitleChanged revoker is stored in
        // _tabTitleRevokers keyed by tabId and revoked by _unbindTabTitle (on
        // TabRemoved / WorkspaceRemoved content teardown and page teardown), so
        // the handler never fires after the tab is gone. The handler captures the
        // page WEAKLY (never keeps the page alive) and re-resolves the VM by id
        // each fire (never holds a strong/dangling VM ref); a re-mount of an
        // already-bound tab replaces its revoker (auto-revoking the prior one) so
        // the subscription is never duplicated.
        void _bindTabChromeToContent(::WorkspaceModel::TabId tabId, const winrt::TerminalApp::IPaneContent& content);

        // Revoke + drop the TitleChanged subscription for `tabId` (if any). A
        // no-op when the tab had no live-title binding. Called by the content-
        // teardown path so a removed/torn-down tab's handler can never fire.
        void _unbindTabTitle(::WorkspaceModel::TabId tabId);

        // Big-flip Slice B (#54): parent the ACTIVE workspace's mounted content
        // into the page's (collapsed) WorkspaceContentHost. Resolves the single
        // (this slice) ContentId the active workspace owns from
        // _contentsByWorkspace, Find()s its live IPaneContent in the registry,
        // and asks the page to parent its GetRoot() as the host's sole child.
        // Driven from apply(ContentMounted) and apply(ActiveWorkspaceChanged)
        // (after the existing classic _SelectTab swap, which this does NOT
        // remove). A no-op — leaving _hostContentId untouched — when the
        // workspace owns no content yet or the page is gone. Records
        // _hostContentId so a test can observe which content the host backs.
        void _showActiveWorkspaceContentInHost(::WorkspaceModel::WorkspaceId active);

        // Big-flip Slice C (#54): swap the (collapsed) WorkspaceContentHost's
        // child to the content of a SPECIFIC tab — the per-tab analogue of
        // _showActiveWorkspaceContentInHost. Resolves the tab's bound ContentId
        // from _contentByTab (populated by the ContentMounted arm), Find()s its
        // live IPaneContent in the registry, and asks the page to parent its
        // GetRoot() as the host's sole child. Driven by apply(ActiveTabChanged)
        // so switching between a leaf's tabs shows the newly-active tab's
        // content. A no-op — leaving _hostContentId untouched — when the page is
        // gone, the tab has no bound content, or the id no longer resolves.
        // Single-leaf scope: splits land in Slice D. INVISIBLE this slice (host
        // Collapsed).
        void _showTabContentInHost(::WorkspaceModel::TabId tabId);

        // Big-flip Slice F-0 (#54): for EVERY projected leaf, parent that leaf's
        // ACTIVE tab's live content GetRoot() into the leaf's per-leaf content
        // host — so a SPLIT workspace renders each leaf's terminal in its own
        // cell. The page enumerates (leaf, active-tab) via _leafContentTabs()
        // (it owns leaf->active-tab); this resolves each active TabId to its live
        // IPaneContent via _contentByTab + the registry (the view owns
        // tab->content) and drives the page's _attachContentToLeafHost per leaf.
        // A leaf whose active tab has no bound/owned content (a not-yet-mounted
        // tab, or a torn-down id) is skipped — its host stays empty until its
        // ContentMounted arrives. Driven by apply(ContentMounted),
        // apply(ActiveTabChanged), and after a pane-tree rebuild (the fresh tree
        // has empty leaf hosts that must be re-populated). INVISIBLE: the whole
        // tree lives inside the still-Collapsed WorkspaceContentHost.
        void _reattachLeafContents();

        // Big-flip Slice F-0 (#54): rebuild the active workspace's projected pane
        // tree (page->_rebuildActiveWorkspacePaneTree) AND immediately re-attach
        // each surviving leaf's active-tab content into its fresh host. A rebuild
        // discards the old per-leaf hosts (and their parented content), so the
        // two MUST be paired: every arm that re-derives the tree calls this
        // instead of the bare rebuild, so the projection never ends a turn with
        // empty leaf hosts. A no-op when the page is gone.
        void _rebuildAndReattachLeafContents();

        // Big-flip Slice F-5 (#54): focus the active workspace's active-leaf
        // active-tab terminal via IPaneContent.Focus(Programmatic). The
        // model-driven replacement for the focus tracking the classic
        // _SelectTab -> _UpdatedSelectedTab path performed before the cutover.
        // Driven by apply(ActiveWorkspaceChanged) and apply(ActiveTabChanged).
        // A no-op when the page is gone, the active leaf is unknown, or the
        // content is not yet mounted.
        void _focusActiveLeafContent();

        // The ContentId currently attached into the WorkspaceContentHost (the
        // backing content of the host's sole child), or std::nullopt when
        // nothing has been attached. Exposed read-only via hostContentIdForTest.
        std::optional<::WorkspaceModel::ContentId> _hostContentId{ std::nullopt };
    };
}
