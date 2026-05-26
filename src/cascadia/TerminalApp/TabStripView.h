// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// TabStripView — the per-leaf pane tab strip. Workspaces M1 (#54, ADR-001)
// rewrote it from a re-skinned WUX ListView into the REAL MUX TabView
// (Microsoft.UI.Xaml.Controls.TabView — the same control the classic per-window
// tab row uses), driven by WorkspaceModel as the single source of truth via
// MANUAL TabView.TabItems() projection (NOT TabItemsSource data-binding).
//
// The page sets ItemsSource to the leaf's PaneTabViewModel collection. The
// control subscribes to that vector's VectorChanged and, on the UI thread,
// projects each VM into a TabViewItem appended to TabView.TabItems() (mirroring
// classic WT's manual-TabItems shape: each TabViewItem.Content is a throwaway
// empty Border — the drag-identity bodge — and the real terminal is hosted
// separately in the page's leafContentHost, never inside the strip).
//
// Selection is a ONE-WAY projection of the model:
//   * The active tab is pushed into TabView.SelectedItem from each VM's
//     IsActive (we subscribe to every VM's PropertyChanged), so the
//     ActiveTabChanged diff arm drives the control.
//   * TabView.SelectionChanged is a USER INTENT only → it raises the selected
//     VM's RequestActivate (→ selectTab model action). The control never writes
//     model state. A reentrancy guard (_selectionPushDepth, a depth counter)
//     suppresses the SelectionChanged that the programmatic SelectedItem push
//     re-fires, so the push never loops back into an intent (this is
//     crash-avoidance — a re-entrant mutation inside a TabView callback is a
//     0xc000027b corner).
//   * TabCloseRequested → the VM's RequestClose (→ closeTab model action).
//
// Drag is OFF (CanReorderTabs / CanDragTabs / AllowDropTabs = false, set in
// XAML); M6 wires the full drag state machine. Native TabViewItem chrome / icon
// / tooltip replace the old bespoke re-skin.

#pragma once

#include "TabStripView.g.h"

namespace winrt::TerminalApp::implementation
{
    struct TabStripView : TabStripViewT<TabStripView>
    {
        TabStripView();

        // The leaf's PaneTabViewModel collection. The setter rebuilds the
        // TabView.TabItems() projection and (re-)subscribes the VectorChanged +
        // per-VM PropertyChanged handlers. The getter returns the stored source.
        winrt::Windows::Foundation::IInspectable ItemsSource();
        void ItemsSource(const winrt::Windows::Foundation::IInspectable& value);

        // Test-only structural accessor (see the .idl): the inner MUX TabView so
        // headless TAEF can assert TabItems / SelectedItem / drag flags.
        winrt::Microsoft::UI::Xaml::Controls::TabView TabViewControl();

        // Workspaces M6a (#54, ADR-001): the strip-level move INTENT (see the .idl).
        // A within-leaf reorder translates the captured drag from→to into this; the
        // page subscribes it and dispatches moveTab(state, tabId, LeafId(), dstIdx).
        // Declared BEFORE the WINRT_PROPERTY below: that macro closes with a
        // `private:` section, so a member placed after it would be inaccessible to
        // the generated projection glue (the event raiser must stay public).
        til::event<winrt::TerminalApp::MoveTabRequestedEventArgs> MoveTabRequested;

        // Workspaces M6 Stage 0 (#54, ADR-001): the model PaneId (LeafId.v) of the
        // leaf this strip projects, set by the page in _projectLeafContainer. Both
        // the within-leaf reorder (M6a) and the cross-leaf drop (M6b) use it as the
        // moveTab destination. A plain scalar; not observed.
        WINRT_PROPERTY(uint64_t, LeafId, 0);

    private:
        // The source VM collection the page set, and the live subscription to its
        // membership changes. Each membership change re-projects the TabItems.
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel> _source{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel>::VectorChanged_revoker _sourceChangedRevoker{};

        // Per-VM PropertyChanged subscriptions, parallel to _source's order, so a
        // VM's IsActive flip pushes TabView.SelectedItem (the model→control
        // selection projection). Rebuilt whenever the projection is rebuilt.
        std::vector<winrt::Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker> _vmPropertyChangedRevokers;

        // Reentrancy guard, a DEPTH COUNTER (not a bare bool). Non-zero while we
        // programmatically push SelectedItem from the model; suppresses the
        // SelectionChanged the control re-fires so the model push never loops back
        // into a RequestActivate intent. A counter (++ on entry / -- on scope_exit)
        // composes regardless of nesting order: _rebuildProjection guards the whole
        // rebuild AND calls _syncSelectionFromModel, which guards again — with a
        // bool the inner scope_exit would clear the outer guard early; the counter
        // keeps it raised until the OUTERMOST scope unwinds.
        int _selectionPushDepth{ 0 };

        // Workspaces M6a (#54, ADR-001): within-leaf reorder bookkeeping (ports
        // classic TabManagement.cpp:1035/1283). _rearranging is true between
        // TabDragStarting and TabDragCompleted; while it is set, TabItemsChanged is
        // the MUX-internal reorder write-back (NOT a projection rebuild), so we
        // record the Removed (from) and Inserted (to) indices it reports rather
        // than re-projecting. On TabDragCompleted we translate from→to into the
        // MoveTabRequested intent. Reset after each gesture.
        bool _rearranging{ false };
        std::optional<uint32_t> _rearrangeFrom{ std::nullopt };
        std::optional<uint32_t> _rearrangeTo{ std::nullopt };

        // Tear down + rebuild the whole TabView.TabItems() projection from
        // _source (append every VM as a TabViewItem; re-subscribe each VM's
        // PropertyChanged), then sync SelectedItem to the active VM. UI thread.
        void _rebuildProjection();

        // Build one TabViewItem for `vm` (header / icon / tooltip / Tag = vm /
        // empty-Border content). UI thread.
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem _makeTabViewItem(const winrt::TerminalApp::PaneTabViewModel& vm);

        // Refresh `item`'s native chrome (header text + icon + tooltip + color)
        // from `vm`. Used at build time and on the VM's Title/PropertyChanged.
        void _applyChrome(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Workspaces M5 (#54, ADR-001): build the per-tab right-click context menu
        // (a MenuFlyout set as the TabViewItem.ContextFlyout) whose items raise
        // TabId-scoped VM intents (NOT classic's _dispatch.DoAction reconcile). Owned
        // by the item, released on _rebuildProjection (weak captures, no use-after-free
        // on a torn-down tab). The `header` is captured so the Rename item reuses M2's
        // BeginRename() path. UI thread.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _makeContextFlyout(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm, const winrt::TerminalApp::TabHeaderControl& header);

        // Refresh the Pin/Unpin context-menu item's label from the VM's projected
        // Pinned state (the toggle text comes BACK from the model via the
        // TabDecorationUpdated diff arm). Used at build time and on the VM's Pinned
        // PropertyChanged. UI thread.
        void _applyContextMenuPinLabel(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Workspaces M4 (#54, ADR-001): open the reused ColorPickupFlyout anchored
        // to `item` (mirrors classic Tab::AttachColorPicker). The flyout's
        // ColorSelected → vm.RequestSetColor / ColorCleared → vm.RequestClearColor
        // (TabId-scoped model intents — NEVER a click-site write-back). The picker
        // is held in _tabColorPickup for its open lifetime and self-released on
        // Closed (so its handlers can't outlive a torn-down strip); the VM is
        // weak-captured so a commit after a re-projection is a safe no-op. UI thread.
        void _showColorPicker(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Workspaces M1.2 (#54, ADR-001): apply / clear the classic selected-tab
        // color treatment (ported from Tab::_ApplyTabColorOnUIThread /
        // _ClearTabBackgroundColor) — local per-TabViewItem theme-dictionary
        // resource overrides tracking the live terminal background color. UI thread.
        void _applyTabColor(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const til::color& color);
        void _clearTabColor(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item);

        // Push the active VM (the one with IsActive) into TabView.SelectedItem,
        // guarded so the re-fired SelectionChanged is not treated as an intent.
        void _syncSelectionFromModel();

        // The TabView's SelectionChanged handler: a user intent only. Resolves
        // the selected TabViewItem's Tag VM and raises its RequestActivate
        // (unless we are mid programmatic push). Never writes model state.
        void _onSelectionChanged(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);

        // TabCloseRequested → the VM's RequestClose intent (→ closeTab).
        void _onTabCloseRequested(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& args);

        // Workspaces M6a (#54, ADR-001): within-leaf reorder. CanReorderTabs(true)
        // lets MUX mutate TabItems internally during a drag (→ TabItemsChanged
        // Removed+Inserted), bracketed by TabDragStarting → TabDragCompleted. We
        // capture the from/to indices off TabItemsChanged and, on TabDragCompleted
        // with from≠to, resolve the dragged VM Id from the moved TabViewItem's Tag
        // and raise the strip-level MoveTabRequested(id, to) intent. The page
        // dispatches moveTab and the resulting TabMoved diff re-projects TabItems
        // from the model (authoritative) — MUX's optimistic visual reorder is
        // accepted then reconciled, never written back to the model here. Ports
        // classic TabManagement.cpp:1035 (_rearrangeFrom/_rearrangeTo) / :1283.
        // AllowDropTabs stays FALSE in M6a → the tear-out dispatcher is
        // short-circuited, so the missing-TabDroppedOutside crash is structurally
        // impossible (M6b wires that arm).
        void _onTabDragStarting(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragStartingEventArgs& args);
        void _onTabItemsChanged(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::Foundation::Collections::IVectorChangedEventArgs& args);
        void _onTabDragCompleted(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabDragCompletedEventArgs& args);

        // Resolve the VM carried in a TabViewItem's Tag (nullptr if none).
        static winrt::TerminalApp::PaneTabViewModel _vmFromItem(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item);

        // Workspaces M4 (#54, ADR-001): the currently-open per-tab color picker (the
        // reused ColorPickupFlyout). Held for its open lifetime so its
        // ColorSelected/ColorCleared/Closed handlers stay alive while shown; released
        // back to nullptr on Closed (mirrors classic Tab::_tabColorPickup). Only one
        // is ever open at a time. Its handler tokens revoke on Closed.
        winrt::TerminalApp::ColorPickupFlyout _tabColorPickup{ nullptr };
        winrt::event_token _colorSelectedToken{};
        winrt::event_token _colorClearedToken{};
        winrt::event_token _colorPickerClosedToken{};
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TabStripView);
}
