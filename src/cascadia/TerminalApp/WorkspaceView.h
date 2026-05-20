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

#include <unordered_map>

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

        // Erase the content bound to `tabId` (if any) from the registry — the
        // only place a single content's ConPTY tears down. Called by the
        // removal arms. A no-op when the tab had no mounted content.
        void _removeContentForTab(::WorkspaceModel::TabId tabId);
    };
}
