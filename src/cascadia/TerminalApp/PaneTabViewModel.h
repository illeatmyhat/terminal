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
// Modeled on WorkspaceViewModel: a thin INotifyPropertyChanged runtimeclass of
// WINRT_OBSERVABLE_PROPERTYs. `Id` is the stable TabId.v identity and is not
// observed. The VM NEVER touches the WorkspaceModel; it raises intent events
// that the owning page dispatches as model actions.
//
// INVISIBLE this slice: the strip ListView this VM feeds lives inside the
// still-Collapsed WorkspaceContentHost, so nothing this VM drives is on screen
// (the classic tab stays the only visible thing). Production wiring of the
// visible triggers is deferred to Slice F.

#pragma once

#include "PaneTabViewModel.g.h"

namespace winrt::TerminalApp::implementation
{
    struct PaneTabViewModel : PaneTabViewModelT<PaneTabViewModel>
    {
        PaneTabViewModel() = default;

        // The active row's font weight: Bold when active, Normal otherwise.
        // Re-evaluated by x:Bind whenever IsActive raises PropertyChanged.
        static winrt::Windows::UI::Text::FontWeight ActiveToFontWeight(bool isActive)
        {
            return isActive ? winrt::Windows::UI::Text::FontWeights::Bold()
                            : winrt::Windows::UI::Text::FontWeights::Normal();
        }

        // Slice 2a (#54): tab-chrome projection helpers, ALL keyed off IsActive
        // (OneWay) so the SELECTED look is a pure projection of the model — never
        // the ListView's click-selection. Re-evaluated by x:Bind whenever
        // IsActive raises PropertyChanged.
        //
        // ActiveToVisibility gates the connected selected-tab background AND the
        // selected (primary) foreground label: Visible when active, Collapsed
        // otherwise. RestToVisibility is its inverse and gates the rest
        // (secondary) foreground label. Both labels carry theme-aware
        // {ThemeResource} foreground brushes in the template (Slice 2a.1), so the
        // foreground is theme-correct without an imperative brush lookup.
        static winrt::Windows::UI::Xaml::Visibility ActiveToVisibility(bool isActive)
        {
            return isActive ? winrt::Windows::UI::Xaml::Visibility::Visible
                            : winrt::Windows::UI::Xaml::Visibility::Collapsed;
        }

        static winrt::Windows::UI::Xaml::Visibility RestToVisibility(bool isActive)
        {
            return isActive ? winrt::Windows::UI::Xaml::Visibility::Collapsed
                            : winrt::Windows::UI::Xaml::Visibility::Visible;
        }

        // Intent signals (Big-flip Slice C, #54). These only raise events; they
        // never mutate this VM's state and never touch the WorkspaceModel. The
        // page subscribes and dispatches the model action (selectTab / closeTab),
        // then the resulting diff re-projects the strip. Production wiring of the
        // visible triggers (row tap / close button) lands in Slice F.
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

        // Slice 2a.2 (#54): the live content background COLOR, carried as a FRESH
        // non-acrylic SolidColorBrush (the page extracts the color from the
        // mounted IPaneContent's BackgroundBrush() and builds this brush — it
        // never shares the content's own brush, which may be acrylic). The
        // SELECTED tab's connected background x:Binds to this OneWay, so the
        // active pane-tab merges into the terminal content the way the classic WT
        // focused tab does ("tab.background = terminalBackground"). A pure
        // projection of the mounted content; nullptr until ContentMounted seeds
        // it (the Border then renders neutral/transparent, never the old inverted
        // floating fill).
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, Background, PropertyChanged.raise, nullptr);
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(PaneTabViewModel);
}
