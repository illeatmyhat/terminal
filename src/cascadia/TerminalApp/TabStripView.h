// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// TabStripView — the per-leaf pane tab strip, extracted from the inline
// TerminalPage.xaml `PaneTabStrip` ListView into a real UserControl (big-flip
// per-pane strip Slice 1, #54).
//
// The programmatic per-leaf strip in `_projectLeafContainer` used to hand-build
// a bare ListView and clone only the inline strip's ItemTemplate + Background,
// OMITTING its ItemsPanel (horizontal StackPanel), ScrollViewer settings, and
// SelectionMode="Single" — so multiple tabs in a leaf stacked VERTICALLY. This
// control carries ALL of those, so `_projectLeafContainer` just instantiates it
// and binds ItemsSource. The orientation/scroll/selection now live in one place.
//
// Selection is a PURE PROJECTION of the model: the active-row highlight comes
// from PaneTabViewModel.IsActive (FontWeight binding); the inner ListView's
// SelectedItem is never bound as a source of truth and never writes back to the
// model. A user tap raises the VM's RequestActivate intent; the close Button
// raises RequestClose. The page dispatches the action; the diff re-projects.

#pragma once

#include "TabStripView.g.h"

namespace winrt::TerminalApp::implementation
{
    struct TabStripView : TabStripViewT<TabStripView>
    {
        TabStripView();

        // Projects onto the inner ListView's ItemsSource. Stored as a plain
        // IInspectable; the inner ListView holds the live reference, so the
        // getter reads it straight back off the ListView. (Non-const: the
        // generated x:Name accessor TabsList() is non-const.)
        winrt::Windows::Foundation::IInspectable ItemsSource();
        void ItemsSource(const winrt::Windows::Foundation::IInspectable& value);

        // Test-only structural accessor (see the .idl). The inner ListView so
        // headless TAEF can assert orientation / SelectionMode / items.
        winrt::Windows::UI::Xaml::Controls::ListView TabsListView();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TabStripView);
}
