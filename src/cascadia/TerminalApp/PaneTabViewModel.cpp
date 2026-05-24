// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "PaneTabViewModel.h"
#include "PaneTabViewModel.g.cpp"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;

namespace winrt::TerminalApp::implementation
{
    // Slice 2a (#54): the row label foreground, a pure projection of IsActive.
    // Looks up the classic TabViewItem selected/rest foreground brush from the
    // application resources (TextFillColorPrimaryBrush when active,
    // TextFillColorSecondaryBrush otherwise — the same keys the WinUI 2.8
    // TabViewItem theme brushes resolve to), with a neutral fallback if absent.
    // Mirrors WorkspaceViewModel::ActiveToRowBrush. Re-evaluated by x:Bind
    // whenever IsActive raises PropertyChanged.
    Brush PaneTabViewModel::ActiveToForeground(bool isActive)
    {
        const std::wstring_view key{ isActive ? L"TextFillColorPrimaryBrush" : L"TextFillColorSecondaryBrush" };

        if (const auto app = Application::Current())
        {
            const auto resources = app.Resources();
            const auto boxedKey = winrt::box_value(winrt::hstring{ key });
            if (resources && resources.HasKey(boxedKey))
            {
                if (const auto brush = resources.Lookup(boxedKey).try_as<Brush>())
                {
                    return brush;
                }
            }
        }

        // Resource-absent fallback (e.g. headless TAEF with no XamlControls
        // resources merged): a near-opaque white that reads on either theme.
        const uint8_t alpha = isActive ? 0xFF : 0xC8;
        return SolidColorBrush{ winrt::Windows::UI::Color{ alpha, 0xFF, 0xFF, 0xFF } };
    }

    // Single-tap activation (Big-flip Slice C, #54). Raise ActivateRequested so
    // the page dispatches selectTab(Id); the resulting ActiveTabChanged diff arm
    // flips this row's IsActive and swaps the host's content. The VM never
    // touches the model — it only signals intent (mirrors
    // WorkspaceViewModel::RequestActivate). Production wiring of the visible
    // trigger (the strip row's tap) is deferred to Slice F.
    void PaneTabViewModel::RequestActivate()
    {
        ActivateRequested.raise(*this, nullptr);
    }

    // Close intent (Big-flip Slice C, #54). Raise CloseRequested so the page
    // dispatches closeTab(Id); the resulting TabRemoved diff arm removes this
    // VM from its leaf's strip collection. The VM never touches the model.
    // Production wiring of the visible trigger (the per-row close affordance)
    // is deferred to Slice F.
    void PaneTabViewModel::RequestClose()
    {
        CloseRequested.raise(*this, nullptr);
    }
}
