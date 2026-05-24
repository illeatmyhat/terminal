// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// WorkspaceViewModel — the observable per-row view-model for the workspaces
// sidebar (issue #52, Stage 2). One instance per model workspace lives in
// TerminalPage's IObservableVector, which is the ItemsSource of the sidebar
// ItemsControl. The collection is mutated ONLY by the WorkspaceChange-stream
// arms (WorkspaceAdded / WorkspaceRemoved / ActiveWorkspaceChanged /
// WorkspaceMetadataUpdated) routed through TerminalPage; the UI is a pure
// downstream projection of the model.
//
// Modeled on TerminalTabStatus: a thin INotifyPropertyChanged runtimeclass
// of WINRT_OBSERVABLE_PROPERTYs. `Id` is the stable WorkspaceId.v identity
// and is not observed. The static helpers are converter-free x:Bind
// function-binding targets that re-evaluate when their observable input
// raises PropertyChanged.

#pragma once

#include "WorkspaceViewModel.g.h"
#include "ColorPickupFlyout.h"

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceViewModel : WorkspaceViewModelT<WorkspaceViewModel>
    {
        WorkspaceViewModel() = default;

        static winrt::Windows::UI::Xaml::Visibility BoolToVisibility(bool value)
        {
            return value ? winrt::Windows::UI::Xaml::Visibility::Visible
                         : winrt::Windows::UI::Xaml::Visibility::Collapsed;
        }

        // The inverse: Collapsed when true, Visible when false. Lets one
        // observable (IsEditing) drive the TextBlock (visible when NOT editing)
        // and the TextBox (visible when editing) without an extra property.
        static winrt::Windows::UI::Xaml::Visibility InvertedBoolToVisibility(bool value)
        {
            return value ? winrt::Windows::UI::Xaml::Visibility::Collapsed
                         : winrt::Windows::UI::Xaml::Visibility::Visible;
        }

        static winrt::Windows::UI::Text::FontWeight ActiveToFontWeight(bool isActive)
        {
            return isActive ? winrt::Windows::UI::Text::FontWeights::Bold()
                            : winrt::Windows::UI::Text::FontWeights::Normal();
        }

        // The active row's background. Resolves a theme brush from the app
        // resources for the active row, transparent otherwise; defined in the
        // .cpp because it touches Application.Current().Resources().
        static winrt::Windows::UI::Xaml::Media::Brush ActiveToRowBrush(bool isActive);

        // "Unpin" when pinned, "Pin" otherwise. The dynamic context-menu label.
        static winrt::hstring PinLabel(bool isPinned);

        // Intent signals (Stage 3a). These only mutate this VM's own observable
        // state and raise events; they never touch the WorkspaceModel or the
        // owning TerminalPage. The page subscribes and dispatches the model
        // action, then the resulting diff re-projects the row.
        void BeginRename();
        void CommitRename();
        void CancelRename();
        void TogglePin();

        // Open the per-row color picker (#52, Stage 3b). Lazily constructs and
        // owns a ColorPickupFlyout (one row == one VM == one picker, analogous
        // to Tab owning _tabColorPickup), seeds it from the current Color when
        // HasColor, and shows it anchored to the row. ColorSelected /
        // ColorCleared from the picker are forwarded as ColorCommitted (a null
        // IReference signals clear). The VM never touches the WorkspaceModel.
        void ShowColorPicker();

        // Single-tap activation (#52, Stage 3c). Raises ActivateRequested; the
        // page subscribes and dispatches switchToWorkspace. The VM never touches
        // the model — it only signals intent (mirrors TogglePin).
        void RequestActivate();

        // Editor view-layer handlers. They operate ONLY on the TextBox passed
        // as `sender` (focus / select-all on load, commit on Enter, cancel on
        // Escape) — never the model. Wired from the DataTemplate's TextBox.
        void OnEditorLoaded(const winrt::Windows::Foundation::IInspectable& sender,
                            const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void OnEditorKeyDown(const winrt::Windows::Foundation::IInspectable& sender,
                             const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);

        // Captures the row's anchor FrameworkElement (the row Grid) so
        // ShowColorPicker can position the flyout. View-layer wiring only.
        void OnRowLoaded(const winrt::Windows::Foundation::IInspectable& sender,
                         const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        til::property_changed_event PropertyChanged;

        // sender = *this (carries Id); args = committed name / desired pinned.
        til::typed_event<winrt::TerminalApp::WorkspaceViewModel, winrt::hstring> RenameCommitted;
        til::typed_event<winrt::TerminalApp::WorkspaceViewModel, bool> PinToggleRequested;

        // sender = *this (carries Id); args = the chosen color, or a null
        // IReference to clear it. One event, one dispatch path on the page.
        til::typed_event<winrt::TerminalApp::WorkspaceViewModel, winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>> ColorCommitted;

        // sender = *this (carries Id); args = null. The page resolves the
        // workspace from the sender's Id and dispatches switchToWorkspace.
        til::typed_event<winrt::TerminalApp::WorkspaceViewModel, winrt::Windows::Foundation::IInspectable> ActivateRequested;

        // Stable identity (WorkspaceId.v); not observed.
        WINRT_PROPERTY(uint64_t, Id, 0);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Title, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Color, Color, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, HasColor, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsPinned, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsActive, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, IsEditing, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, EditText, PropertyChanged.raise);

    private:
        // The per-row color picker, lazily constructed on first ShowColorPicker
        // and reused thereafter (we keep the subscriptions for the VM's life).
        winrt::TerminalApp::ColorPickupFlyout _colorPickup{ nullptr };
        winrt::event_token _colorSelectedToken{};
        winrt::event_token _colorClearedToken{};

        // The row's anchor element, captured in OnRowLoaded; held weakly so the
        // VM never roots a recycled DataTemplate container.
        winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> _rowAnchor{};
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceViewModel);
}
