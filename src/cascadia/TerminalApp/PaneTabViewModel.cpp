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

    // Slice 2a.4 (#54): hover intent. Like RequestActivate/RequestClose these
    // only raise events — they never mutate this VM and never touch the
    // WorkspaceModel. The page tracks the hovered tab id (transient view state)
    // and recomputes ShowSeparator so hovering a tab hides the dividers on both
    // of its sides (classic WinUI TabViewItem PointerOver sets the separator's
    // opacity to 0). Bound to the item template's PointerEntered/PointerExited.
    void PaneTabViewModel::RequestHoverEnter()
    {
        HoverEnterRequested.raise(*this, nullptr);
    }

    void PaneTabViewModel::RequestHoverExit()
    {
        HoverExitRequested.raise(*this, nullptr);
    }
}
