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

        // Tear down + rebuild the whole TabView.TabItems() projection from
        // _source (append every VM as a TabViewItem; re-subscribe each VM's
        // PropertyChanged), then sync SelectedItem to the active VM. UI thread.
        void _rebuildProjection();

        // Build one TabViewItem for `vm` (header / icon / tooltip / Tag = vm /
        // empty-Border content). UI thread.
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem _makeTabViewItem(const winrt::TerminalApp::PaneTabViewModel& vm);

        // Refresh `item`'s native chrome (header text + icon + tooltip) from
        // `vm`. Used at build time and on the VM's Title/PropertyChanged.
        void _applyChrome(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Push the active VM (the one with IsActive) into TabView.SelectedItem,
        // guarded so the re-fired SelectionChanged is not treated as an intent.
        void _syncSelectionFromModel();

        // The TabView's SelectionChanged handler: a user intent only. Resolves
        // the selected TabViewItem's Tag VM and raises its RequestActivate
        // (unless we are mid programmatic push). Never writes model state.
        void _onSelectionChanged(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);

        // TabCloseRequested → the VM's RequestClose intent (→ closeTab).
        void _onTabCloseRequested(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& args);

        // Resolve the VM carried in a TabViewItem's Tag (nullptr if none).
        static winrt::TerminalApp::PaneTabViewModel _vmFromItem(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TabStripView);
}
