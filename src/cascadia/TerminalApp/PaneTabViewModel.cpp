// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "PaneTabViewModel.h"
#include "PaneTabViewModel.g.cpp"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;

namespace winrt::TerminalApp::implementation
{
    // Single-tap activation (Big-flip Slice C, #54). Raise ActivateRequested so
    // the page dispatches selectTab(Id); the resulting ActiveTabChanged diff arm
    // flips this row's IsActive and swaps the host's content. The VM never
    // touches the model — it only signals intent. TabStripView raises this from
    // TabView.SelectionChanged (user intent only).
    void PaneTabViewModel::RequestActivate()
    {
        ActivateRequested.raise(*this, nullptr);
    }

    // Close intent (Big-flip Slice C, #54). Raise CloseRequested so the page
    // dispatches closeTab(Id); the resulting TabRemoved diff arm removes this
    // VM from its leaf's strip collection. The VM never touches the model.
    // TabStripView raises this from TabView.TabCloseRequested.
    void PaneTabViewModel::RequestClose()
    {
        CloseRequested.raise(*this, nullptr);
    }

    // Rename intent (Workspaces M2, #54, ADR-001). Raise RenameRequested carrying
    // the committed title so the page dispatches setTabTitle(Id, newTitle); the
    // resulting TabDecorationUpdated diff arm projects the new customTitle back
    // onto this VM's CustomTitle (custom-wins over the live title). The VM never
    // touches the model. TabStripView raises this from the hosted
    // TabHeaderControl.TitleChangeRequested.
    void PaneTabViewModel::RequestRename(const winrt::hstring& newTitle)
    {
        RenameRequested.raise(*this, winrt::box_value(newTitle));
    }

    // Pin/unpin intent (Workspaces M5, #54, ADR-001). Raise TogglePinRequested
    // carrying the DESIRED pinned state so the page dispatches setTabPinned(Id,
    // pinned); the resulting TabDecorationUpdated diff arm projects the new pinned
    // state back onto this VM's Pinned. The VM never touches the model. TabStripView
    // raises this from the per-tab context menu's Pin/Unpin item (which reads the
    // current Pinned state and requests its negation).
    void PaneTabViewModel::RequestTogglePin(bool pinned)
    {
        TogglePinRequested.raise(*this, winrt::box_value(pinned));
    }

    // Set-color intent (Workspaces M4, #54, ADR-001). Raise SetColorRequested
    // carrying the chosen color (boxed as an IReference<Color>) so the page
    // dispatches setTabColor(Id, color); the resulting TabDecorationUpdated diff
    // arm projects the new runtimeColor back onto this VM's RuntimeColor (which
    // wins over the live Background in EffectiveBackground). The VM never touches
    // the model. TabStripView raises this from the per-tab "Color…" menu item's
    // ColorPickupFlyout.ColorSelected.
    void PaneTabViewModel::RequestSetColor(const winrt::Windows::UI::Color& color)
    {
        SetColorRequested.raise(*this, winrt::box_value(color).try_as<winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>>());
    }

    // Clear-color intent (Workspaces M4, #54, ADR-001). Raise SetColorRequested
    // with a NULL payload so the page dispatches setTabColor(Id, null); the diff
    // arm clears RuntimeColor, falling EffectiveBackground back to the live
    // Background. The VM never touches the model. TabStripView raises this from the
    // ColorPickupFlyout.ColorCleared.
    void PaneTabViewModel::RequestClearColor()
    {
        SetColorRequested.raise(*this, nullptr);
    }
}
