// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// PaneTabViewModel — the observable per-tab view-model for a single leaf
// pane's tab strip (Big-flip Slice C, #54). One instance per model TabRecord
// lives in a per-leaf IObservableVector owned by TerminalPage, keyed by the
// leaf's PaneId. The collection is mutated ONLY by the WorkspaceChange-stream
// arms (TabAdded / TabRemoved / ActiveTabChanged) routed through
// TerminalPage; the strip is a pure downstream projection of the model.
//
// Workspaces M1 (#54, ADR-001): the strip is now the real MUX TabView. The VM
// shrank to the bits the native control consumes — Id / Title / IsActive /
// Background / Icon and the activate/close intents. The bespoke re-skin's
// projection helpers (ActiveToVisibility / RestToVisibility / ActiveToFontWeight
// / ShowSeparatorToVisibility / HoverToVisibility) and the hover state
// (ShowSeparator / IsHovered + the hover intents) are GONE — native TabViewItem
// chrome renders the selection visual, the separators and the hover highlight.
//
// The VM NEVER touches the WorkspaceModel; it raises intent events that the
// owning page dispatches as model actions.

#pragma once

#include "PaneTabViewModel.g.h"

namespace winrt::TerminalApp::implementation
{
    struct PaneTabViewModel : PaneTabViewModelT<PaneTabViewModel>
    {
        PaneTabViewModel() = default;

        // Intent signals (Big-flip Slice C, #54). These only raise events; they
        // never mutate this VM's state and never touch the WorkspaceModel. The
        // page subscribes and dispatches the model action (selectTab / closeTab),
        // then the resulting diff re-projects the strip. TabStripView raises
        // RequestActivate from TabView.SelectionChanged (user intent) and
        // RequestClose from TabView.TabCloseRequested.
        void RequestActivate();
        void RequestClose();

        til::property_changed_event PropertyChanged;

        // sender = *this (carries Id); args = null. The page resolves the tab
        // from the sender's Id and dispatches the model action.
        til::typed_event<winrt::TerminalApp::PaneTabViewModel, winrt::Windows::Foundation::IInspectable> ActivateRequested;
        til::typed_event<winrt::TerminalApp::PaneTabViewModel, winrt::Windows::Foundation::IInspectable> CloseRequested;

        // Stable identity (TabId.v); not observed.
        WINRT_PROPERTY(uint64_t, Id, 0);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Title, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsActive, PropertyChanged.raise);

        // The live content background COLOR, carried as a FRESH non-acrylic
        // SolidColorBrush (the page extracts the color from the mounted
        // IPaneContent's BackgroundBrush() and builds this brush — it never
        // shares the content's own brush, which may be acrylic). A pure
        // projection of the mounted content; nullptr until ContentMounted seeds
        // it. (M1: the native MUX TabView themes its selected tab itself; wiring
        // the selected-tab background to track this content color the classic way
        // is a DEFERRED SHOULD per ADR-001 — the projection is kept running for
        // when that lands.) APPENDED LAST — inserting a property mid-runtimeclass
        // shifts vtable slots and trips the /RTCs "Stack around 'value'
        // corrupted" trap.
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, Background, PropertyChanged.raise, nullptr);

        // Workspaces M1 (#54, ADR-001): the tab icon PATH, projected from the
        // mounted IPaneContent's Icon(). TabStripView resolves it to a MUX
        // IconSource (IconPathConverter::IconSourceMUX) on the native
        // TabViewItem.IconSource. A pure downstream projection of the mounted
        // content. APPENDED LAST (after Background) for the same vtable-slot
        // reason.
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Icon, PropertyChanged.raise);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(PaneTabViewModel);
}
