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
}
