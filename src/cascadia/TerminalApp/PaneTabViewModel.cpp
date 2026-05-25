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
}
