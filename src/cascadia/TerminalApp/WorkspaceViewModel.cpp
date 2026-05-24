// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceViewModel.h"
#include "WorkspaceViewModel.g.cpp"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;

namespace winrt::TerminalApp::implementation
{
    // The active row's background. Looks up a subtle list-selection theme brush
    // from the application resources for the active row (falling back to a
    // semi-transparent overlay if the resource is absent), and a transparent
    // brush otherwise. Re-evaluated by x:Bind whenever IsActive raises
    // PropertyChanged.
    Brush WorkspaceViewModel::ActiveToRowBrush(bool isActive)
    {
        if (!isActive)
        {
            return SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() };
        }

        if (const auto app = Application::Current())
        {
            const auto resources = app.Resources();
            static constexpr std::wstring_view selectionBrushKey{ L"SystemControlHighlightListAccentLowBrush" };
            const auto key = winrt::box_value(winrt::hstring{ selectionBrushKey });
            if (resources && resources.HasKey(key))
            {
                if (const auto brush = resources.Lookup(key).try_as<Brush>())
                {
                    return brush;
                }
            }
        }

        // Resource-absent fallback: a faint white overlay that reads as a
        // selection tint on either theme.
        return SolidColorBrush{ winrt::Windows::UI::Color{ 0x20, 0xFF, 0xFF, 0xFF } };
    }

    // Dynamic context-menu label for the pin toggle. Literal strings this
    // slice (no workspace-specific RS_ resources yet — color/pin UI is a
    // later stage); re-evaluated by x:Bind when IsPinned raises PropertyChanged.
    winrt::hstring WorkspaceViewModel::PinLabel(bool isPinned)
    {
        return isPinned ? winrt::hstring{ L"Unpin" } : winrt::hstring{ L"Pin" };
    }

    // Enter inline rename: seed the editor text from the current title and
    // flip IsEditing, which swaps the row's TextBlock for the TextBox via the
    // OneWay visibility bindings. View-only — the model is untouched until the
    // user commits.
    void WorkspaceViewModel::BeginRename()
    {
        if (IsEditing())
        {
            return;
        }
        EditText(Title());
        IsEditing(true);
    }

    // Commit the rename: raise RenameCommitted with the editor text so the
    // page can dispatch renameWorkspace(...). We leave IsEditing-driven row
    // re-projection to the model diff, but flip IsEditing here so the editor
    // closes immediately even if the name is unchanged (no diff arm fires).
    void WorkspaceViewModel::CommitRename()
    {
        if (!IsEditing())
        {
            return;
        }
        IsEditing(false);
        RenameCommitted.raise(*this, EditText());
    }

    // Abandon the rename without raising RenameCommitted; the row reverts to
    // its (unchanged) Title.
    void WorkspaceViewModel::CancelRename()
    {
        IsEditing(false);
    }

    // Signal the desired new pinned value (!IsPinned). The page dispatches
    // setWorkspacePinned(...); the resulting WorkspaceMetadataUpdated diff arm
    // flips IsPinned on this VM, so we do NOT mutate IsPinned here.
    void WorkspaceViewModel::TogglePin()
    {
        PinToggleRequested.raise(*this, !IsPinned());
    }

    // Single-tap activation (#52, Stage 3c). Raise ActivateRequested so the
    // page dispatches switchToWorkspace(Id); the resulting ActiveWorkspaceChanged
    // diff arm flips this row's highlight and selects its classic tab. The VM
    // never touches the model — it only signals intent (mirrors TogglePin).
    void WorkspaceViewModel::RequestActivate()
    {
        ActivateRequested.raise(*this, nullptr);
    }

    // Open the per-row color picker (#52, Stage 3b). Lazily construct the
    // ColorPickupFlyout (the same control tabs use) and wire its ColorSelected
    // / ColorCleared to ColorCommitted: ColorSelected forwards the chosen color
    // boxed in an IReference; ColorCleared forwards a null IReference, which the
    // page reads as "clear". We keep the subscriptions for the VM's lifetime so
    // a second open reuses the same picker. The picker is shown anchored to the
    // row element captured in OnRowLoaded; if it hasn't realized yet there is
    // nothing to anchor to and we no-op. The VM never touches the model.
    void WorkspaceViewModel::ShowColorPicker()
    {
        const auto anchor = _rowAnchor.get();
        if (!anchor)
        {
            return;
        }

        if (!_colorPickup)
        {
            _colorPickup = winrt::TerminalApp::ColorPickupFlyout{};

            auto weakThis{ get_weak() };
            _colorSelectedToken = _colorPickup.ColorSelected([weakThis](const winrt::Windows::UI::Color& color) {
                if (auto self{ weakThis.get() })
                {
                    self->ColorCommitted.raise(*self, winrt::box_value(color).try_as<winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>>());
                }
            });
            _colorClearedToken = _colorPickup.ColorCleared([weakThis]() {
                if (auto self{ weakThis.get() })
                {
                    self->ColorCommitted.raise(*self, nullptr);
                }
            });
        }

        _colorPickup.ShowAt(anchor);
    }

    // Capture the row's anchor element so ShowColorPicker can position the
    // flyout. View-layer wiring only (mirrors Tab anchoring to its
    // TabViewItem); the model is untouched.
    void WorkspaceViewModel::OnRowLoaded(const winrt::Windows::Foundation::IInspectable& sender,
                                         const winrt::Windows::UI::Xaml::RoutedEventArgs& /*e*/)
    {
        if (const auto element = sender.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
        {
            _rowAnchor = element;
        }
    }

    // On editor realize: focus and select-all so the user can immediately type
    // over the existing name (mirrors TabHeaderControl::BeginRename). Operates
    // only on the TextBox passed as sender.
    void WorkspaceViewModel::OnEditorLoaded(const winrt::Windows::Foundation::IInspectable& sender,
                                            const winrt::Windows::UI::Xaml::RoutedEventArgs& /*e*/)
    {
        if (const auto box = sender.try_as<TextBox>())
        {
            box.SelectAll();
            box.Focus(FocusState::Programmatic);
        }
    }

    // Enter commits, Escape cancels. We mark the key handled so it doesn't
    // bubble. Cancel must run before the LostFocus handler (which calls
    // CommitRename) would otherwise fire: clearing focus after CancelRename
    // is harmless because CommitRename early-returns once IsEditing is false.
    void WorkspaceViewModel::OnEditorKeyDown(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                             const winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs& e)
    {
        switch (e.Key())
        {
        case winrt::Windows::System::VirtualKey::Enter:
            e.Handled(true);
            CommitRename();
            break;
        case winrt::Windows::System::VirtualKey::Escape:
            e.Handled(true);
            CancelRename();
            break;
        default:
            break;
        }
    }
}
