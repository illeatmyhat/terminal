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

        // Workspaces M2 (#54, ADR-001): rename intent. TabStripView wires the
        // hosted TabHeaderControl.TitleChangeRequested to call this with the
        // committed title; the page dispatches setTabTitle(Id, newTitle) and the
        // resulting TabDecorationUpdated diff arm projects the new customTitle
        // back onto CustomTitle. The VM never touches the model.
        void RequestRename(const winrt::hstring& newTitle);

        til::property_changed_event PropertyChanged;

        // sender = *this (carries Id); args = null. The page resolves the tab
        // from the sender's Id and dispatches the model action.
        til::typed_event<winrt::TerminalApp::PaneTabViewModel, winrt::Windows::Foundation::IInspectable> ActivateRequested;
        til::typed_event<winrt::TerminalApp::PaneTabViewModel, winrt::Windows::Foundation::IInspectable> CloseRequested;

        // sender = *this (carries Id); args = the committed title (boxed hstring).
        // The page resolves the tab from the sender's Id and dispatches setTabTitle.
        til::typed_event<winrt::TerminalApp::PaneTabViewModel, winrt::Windows::Foundation::IInspectable> RenameRequested;

        // Stable identity (TabId.v); not observed.
        WINRT_PROPERTY(uint64_t, Id, 0);

    public:
        // Workspaces M2 (#54, ADR-001): Title is a COMPUTED effective title with
        // custom-wins precedence. It is NOT a plain observable property: it is
        // derived from two backing fields — _customTitle (the model's
        // TabRecord.customTitle, set by rename via the CustomTitle setter) and
        // _liveTitle (the live shell title, set by the live-push path via the
        // Title setter). The effective title is _customTitle when non-empty, else
        // _liveTitle. Each setter recomputes the effective value and raises
        // PropertyChanged(L"Title") only when it actually changed, so:
        //   * a rename (CustomTitle set non-empty) immediately surfaces and
        //   * a subsequent live TitleChanged (Title/_liveTitle set) is SWALLOWED
        //     by the getter while a custom title is in force — custom wins.
        // The Title SETTER is kept (writing _liveTitle) so the existing live-push
        // call sites (_appendPaneTabVm seed, _setPaneTabTitleForTab) need no
        // change.
        winrt::hstring Title() const noexcept
        {
            return _customTitle.empty() ? _liveTitle : _customTitle;
        }
        void Title(const winrt::hstring& value)
        {
            _setLiveTitle(value);
        }

        // The model's customTitle. The rename diff arm sets this through the page.
        // Empty = "no custom title". Setting it recomputes the effective Title.
        winrt::hstring CustomTitle() const noexcept
        {
            return _customTitle;
        }
        void CustomTitle(const winrt::hstring& value)
        {
            if (_customTitle != value)
            {
                const auto before = Title();
                _customTitle = value;
                if (Title() != before)
                {
                    PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Title" });
                }
            }
        }

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

    private:
        // Workspaces M2 (#54, ADR-001): the two inputs to the computed Title (see
        // the Title()/CustomTitle() accessors above). _liveTitle is the live shell
        // title (the live-push path); _customTitle is the model's customTitle (set
        // by rename). custom-wins: Title() == _customTitle.empty() ? _liveTitle :
        // _customTitle.
        winrt::hstring _liveTitle{};
        winrt::hstring _customTitle{};

        // Set the live shell title and raise PropertyChanged(L"Title") only when
        // the EFFECTIVE title actually changes. While a custom title is in force
        // (_customTitle non-empty) the effective title is unaffected by a live
        // change, so the event is swallowed — a live TitleChanged can never
        // clobber the user's custom title in the view.
        void _setLiveTitle(const winrt::hstring& value)
        {
            if (_liveTitle != value)
            {
                const auto before = Title();
                _liveTitle = value;
                if (Title() != before)
                {
                    PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"Title" });
                }
            }
        }
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(PaneTabViewModel);
}
