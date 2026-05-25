// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TabStripView.h"

// Workspaces M2 (#54, ADR-001): the projected TabHeaderControl activation factory
// (we construct one per TabViewItem to host the reused inline renamer).
#include "winrt/TerminalApp.h"

#include "TabStripView.g.cpp"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Data;

namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    TabStripView::TabStripView()
    {
        InitializeComponent();

        // Wire the control-level intents ONCE (the TabView outlives every
        // projection rebuild). SelectionChanged is a user intent only (guarded);
        // TabCloseRequested raises the VM's close intent. Drag is OFF in XAML.
        TabView().SelectionChanged({ this, &TabStripView::_onSelectionChanged });
        TabView().TabCloseRequested({ this, &TabStripView::_onTabCloseRequested });
    }

    IInspectable TabStripView::ItemsSource()
    {
        return _source;
    }

    // The page sets this to the leaf's PaneTabViewModel collection. Re-anchor
    // the live source, (re-)subscribe to its membership changes, and rebuild the
    // TabView.TabItems() projection. The vector is mutated only by the page's
    // arm-driven helpers (a pure downstream projection of the model), so reacting
    // to its VectorChanged keeps the strip in lockstep with the model.
    void TabStripView::ItemsSource(const IInspectable& value)
    {
        _sourceChangedRevoker.revoke();
        _source = value.try_as<IObservableVector<winrt::TerminalApp::PaneTabViewModel>>();

        if (_source)
        {
            _sourceChangedRevoker = _source.VectorChanged(winrt::auto_revoke, [this](const auto& /*sender*/, const IVectorChangedEventArgs& /*args*/) {
                // Membership changed (TabAdded / TabRemoved diff arms). Rebuild
                // the projection wholesale: the per-leaf strips are tiny, and a
                // full re-project keeps the TabItems structurally identical to
                // the model by construction (illegal divergence unrepresentable),
                // mirroring _rebuildActiveWorkspacePaneTree's rebuild rationale.
                _rebuildProjection();
            });
        }

        _rebuildProjection();
    }

    MUXC::TabView TabStripView::TabViewControl()
    {
        return TabView();
    }

    // Tear down the old projection (TabItems + per-VM subscriptions) and rebuild
    // it from _source: one TabViewItem per VM in declared order, each carrying
    // native chrome + the VM in its Tag, with a PropertyChanged subscription that
    // pushes selection / refreshes chrome. Then sync SelectedItem to the active
    // VM. MUST run on the UI thread (TabItems / selection mutation has UI-thread
    // affinity — classic asserts ASSERT_UI_THREAD).
    void TabStripView::_rebuildProjection()
    {
        // Guard the rebuild so the SelectedItem churn it causes (clearing then
        // re-adding items resets SelectedItem) is never treated as a user intent.
        // Depth counter: this guard nests with the one _syncSelectionFromModel
        // (called at the end of this method) raises, so the inner scope_exit must
        // not clear the guard the outer rebuild still relies on.
        _selectionPushDepth++;
        auto restore = wil::scope_exit([this]() { _selectionPushDepth--; });

        _vmPropertyChangedRevokers.clear();

        auto items = TabView().TabItems();
        items.Clear();

        if (!_source)
        {
            return;
        }

        for (const auto& vm : _source)
        {
            items.Append(_makeTabViewItem(vm));

            // Subscribe to the VM so its IsActive flip pushes SelectedItem (the
            // model→control selection projection) and a Title/Icon/Background
            // change refreshes the native chrome. The VM carries its stable Id,
            // so we resolve the matching TabViewItem by Tag identity each fire.
            //
            // Workspaces M1 (#54, ADR-001) thread affinity: this handler mutates
            // live MUX UIElements (TabViewItem.Header/IconSource via _applyChrome,
            // and TabView.SelectedItem via _syncSelectionFromModel), which have
            // UI-thread affinity. The driving PropertyChanged is NOT guaranteed to
            // arrive on the UI thread: the chrome-push path
            // (WorkspaceView::_bindTabChromeToContent) rides IPaneContent.TitleChanged,
            // which TerminalPaneContent re-raises synchronously off ControlCore's
            // TitleChanged — raised on the connection OUTPUT thread (see
            // ControlCore::_terminalTitleChanged: "this can only ever be triggered
            // by output from the connection ... the Terminal already has the write
            // lock"). The classic Tab marshals exactly this hazard (Tab.cpp
            // ~line 1061: TitleChanged via a ThrottledFunc bound to the dispatcher;
            // sibling content events via co_await wil::resume_foreground). We mirror
            // that here at the choke point closest to the UIElement writes so EVERY
            // property source (Title/Icon/Background AND IsActive) is covered, not
            // just TitleChanged. Hop to the UI dispatcher ONLY when we are not
            // already on it (TerminalPage::_SetFocusedTab's HasThreadAccess pattern)
            // — the model-driven IsActive push already runs on the UI thread and
            // stays synchronous, while the off-thread TitleChanged path marshals. A
            // weak `this` (get_weak) makes a late resume on a torn-down strip a safe
            // no-op, and we re-resolve the weak VM after a hop.
            const auto inpc = vm.as<INotifyPropertyChanged>();
            _vmPropertyChangedRevokers.push_back(
                inpc.PropertyChanged(winrt::auto_revoke, [weakThis = get_weak(), weakVm = winrt::make_weak(vm)](const IInspectable& /*sender*/, const PropertyChangedEventArgs& e) -> safe_void_coroutine {
                    // Read everything off the (possibly soon-destroyed) closure
                    // BEFORE any co_await — revoking the auto_revoker during
                    // suspension can free the lambda object out from under us, so
                    // the weak refs + property name live in the coroutine frame as
                    // locals (mirrors Tab::_AttachEventHandlersToContent's
                    // weakThisCopy dance).
                    const auto weakThisCopy = weakThis;
                    const auto weakVmCopy = weakVm;
                    const auto prop = e.PropertyName();

                    auto strongThis = weakThisCopy.get();
                    if (!strongThis)
                    {
                        co_return;
                    }
                    if (!strongThis->Dispatcher().HasThreadAccess())
                    {
                        co_await wil::resume_foreground(strongThis->Dispatcher());

                        // Re-acquire after the dispatcher hop — the strip may have
                        // been torn down while suspended.
                        strongThis = weakThisCopy.get();
                        if (!strongThis)
                        {
                            co_return;
                        }
                    }

                    const auto vm = weakVmCopy.get();
                    if (!vm)
                    {
                        co_return;
                    }

                    if (prop == L"IsActive")
                    {
                        strongThis->_syncSelectionFromModel();
                    }
                    else if (prop == L"Title" || prop == L"Icon" || prop == L"Background")
                    {
                        // Refresh this VM's native chrome in place.
                        auto items = strongThis->TabView().TabItems();
                        for (uint32_t i = 0; i < items.Size(); ++i)
                        {
                            if (const auto item = items.GetAt(i).try_as<MUXC::TabViewItem>())
                            {
                                if (TabStripView::_vmFromItem(item) == vm)
                                {
                                    strongThis->_applyChrome(item, vm);
                                    break;
                                }
                            }
                        }
                    }
                }));
        }

        _syncSelectionFromModel();
    }

    MUXC::TabViewItem TabStripView::_makeTabViewItem(const winrt::TerminalApp::PaneTabViewModel& vm)
    {
        MUXC::TabViewItem item{};

        // Carry the VM so SelectionChanged / TabCloseRequested resolve the intent
        // target by Tag identity (no positional indexing into the model).
        item.Tag(vm);

        // BODGY (mirrors classic Tab::_MakeTabViewItem): a TabViewItem needs
        // either an Item or a Content for the close/selection events to report
        // the correct item. The terminal lives in the page's leafContentHost, NOT
        // here, so the Content is a throwaway empty Border (the drag-identity
        // bodge). Virtualization can never kill a terminal — lifetime is
        // decoupled.
        item.Content(winrt::Windows::UI::Xaml::Controls::Border{});

        // Workspaces M2 (#54, ADR-001): host a TabHeaderControl in the item's
        // Header (mirroring classic Tab — Tab.cpp ~line 112: "Use our header
        // control as the TabViewItem's header"), reusing its inline renamer rather
        // than reimplementing it. _applyChrome pushes the title into it; the
        // renamer's TitleChangeRequested becomes the VM's RequestRename intent.
        winrt::TerminalApp::TabHeaderControl header{};

        // Double-tap the tab → BeginRename() (mirrors classic Tab::_MakeTabViewItem
        // ~line 158: TabViewItem().DoubleTapped → ActivateTabRenamer). Weak-capture
        // the header so a late double-tap on a torn-down tab is a safe no-op.
        item.DoubleTapped([weakHeader = winrt::make_weak(header)](auto&& /*s*/, auto&& /*e*/) {
            if (auto h{ weakHeader.get() })
            {
                h.BeginRename();
            }
        });

        // The renamer committed a new title → raise the VM's RequestRename intent
        // (→ setTabTitle model action). We never write the title onto the view
        // here; the committed title comes back via the model projection
        // (TabDecorationUpdated → CustomTitle). Weak-capture the VM so a commit
        // after the row was re-projected is a safe no-op.
        header.TitleChangeRequested([weakVm = winrt::make_weak(vm)](auto&& title) {
            if (auto vm{ weakVm.get() })
            {
                vm.RequestRename(title);
            }
        });

        item.Header(header);

        _applyChrome(item, vm);
        return item;
    }

    // Refresh the native TabViewItem chrome (header title + icon + tooltip) from
    // the VM — the maintainers' control renders the rounded top / flare / hover /
    // separator natively, so the old bespoke projection is gone. The header is a
    // hosted TabHeaderControl (M2) whose Title we push; an in-progress rename is
    // NOT clobbered because the renamer collapses the title TextBlock while the
    // TextBox is up. The icon uses IconPathConverter::IconSourceMUX (mirrors
    // classic Tab::UpdateIcon ~line 377); an empty path clears it. The tooltip
    // mirrors the title.
    void TabStripView::_applyChrome(const MUXC::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm)
    {
        const auto title = vm.Title();
        if (const auto header = item.Header().try_as<winrt::TerminalApp::TabHeaderControl>())
        {
            header.Title(title);
        }

        // Native tooltip (classic uses ToolTipService.SetToolTip on the
        // TabViewItem). Mirror the title.
        winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(item, winrt::box_value(title));

        const auto iconPath = vm.Icon();
        if (iconPath.empty())
        {
            item.IconSource(MUXC::IconSource{ nullptr });
        }
        else
        {
            // false = full-color (not grayscale); matches classic's default
            // colored tab icon.
            item.IconSource(Microsoft::Terminal::UI::IconPathConverter::IconSourceMUX(iconPath, false));
        }
    }

    // Push the active VM into TabView.SelectedItem (model→control), guarded so the
    // SelectionChanged the control re-fires from this push is not treated as a
    // user intent. Resolves the active VM's TabViewItem by Tag identity. If no VM
    // is active, leave the selection as-is (the model always has an active tab in
    // a non-empty leaf; an empty strip has nothing to select).
    void TabStripView::_syncSelectionFromModel()
    {
        if (!_source)
        {
            return;
        }

        auto items = TabView().TabItems();

        // Find the TabViewItem whose VM IsActive.
        MUXC::TabViewItem activeItem{ nullptr };
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            if (const auto item = items.GetAt(i).try_as<MUXC::TabViewItem>())
            {
                if (const auto vm = _vmFromItem(item))
                {
                    if (vm.IsActive())
                    {
                        activeItem = item;
                        break;
                    }
                }
            }
        }

        if (!activeItem)
        {
            return;
        }

        // Short-circuit if it is already selected — avoids needless churn and a
        // spurious guarded SelectionChanged.
        if (TabView().SelectedItem() == activeItem)
        {
            return;
        }

        _selectionPushDepth++;
        auto restore = wil::scope_exit([this]() { _selectionPushDepth--; });
        TabView().SelectedItem(activeItem);
    }

    // TabView.SelectionChanged — a USER INTENT only. When the user clicks a tab,
    // raise the selected VM's RequestActivate (→ selectTab model action) and let
    // the resulting diff flip IsActive, which re-pushes SelectedItem through
    // _syncSelectionFromModel. We NEVER write model state here. The reentrancy
    // guard suppresses the SelectionChanged that our own programmatic push
    // re-fires, so the push can never loop back into an intent (a re-entrant
    // TabView mutation is a 0xc000027b failfast corner).
    void TabStripView::_onSelectionChanged(const IInspectable& /*sender*/, const SelectionChangedEventArgs& /*args*/)
    {
        if (_selectionPushDepth > 0)
        {
            return;
        }

        const auto selected = TabView().SelectedItem().try_as<MUXC::TabViewItem>();
        if (!selected)
        {
            return;
        }
        if (const auto vm = _vmFromItem(selected))
        {
            // If the user re-selected the already-active VM there is nothing to
            // do; otherwise raise the activate intent. (Raising it for the
            // already-active VM is harmless — selectTab on the active tab is a
            // no-op — but skipping avoids needless model churn.)
            if (!vm.IsActive())
            {
                vm.RequestActivate();
            }
        }
    }

    // TabCloseRequested → the VM's RequestClose intent (→ closeTab model action).
    // Not a direct TabItems removal: the model is the single source of truth, so
    // the close flows intent → action → TabRemoved diff → re-project (which
    // removes the item via the VectorChanged rebuild).
    void TabStripView::_onTabCloseRequested(const MUXC::TabView& /*sender*/, const MUXC::TabViewTabCloseRequestedEventArgs& args)
    {
        if (const auto item = args.Tab().try_as<MUXC::TabViewItem>())
        {
            if (const auto vm = _vmFromItem(item))
            {
                vm.RequestClose();
            }
        }
    }

    winrt::TerminalApp::PaneTabViewModel TabStripView::_vmFromItem(const MUXC::TabViewItem& item)
    {
        if (!item)
        {
            return nullptr;
        }
        return item.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
    }
}
