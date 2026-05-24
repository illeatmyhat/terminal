// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TabStripView.h"

#include "TabStripView.g.cpp"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    TabStripView::TabStripView()
    {
        InitializeComponent();
    }

    // The inner ListView owns the live ItemsSource reference; project both ways
    // through it so there is a single source of truth and no shadow copy to keep
    // in sync. TabsList is the x:Name'd ListView in TabStripView.xaml.
    IInspectable TabStripView::ItemsSource()
    {
        return TabsList().ItemsSource();
    }

    void TabStripView::ItemsSource(const IInspectable& value)
    {
        TabsList().ItemsSource(value);
    }

    // Test-only structural accessor — the x:Name'd inner ListView.
    ListView TabStripView::TabsListView()
    {
        return TabsList();
    }
}
