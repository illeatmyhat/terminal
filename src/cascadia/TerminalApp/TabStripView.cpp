// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TabStripView.h"

// Workspaces M2 (#54, ADR-001): the projected TabHeaderControl activation factory
// (we construct one per TabViewItem to host the reused inline renamer).
#include "winrt/TerminalApp.h"

// Workspaces M1.2 (#54, ADR-001): luminance-based readable-foreground math for the
// selected-tab color treatment, ported from classic Tab::_ApplyTabColorOnUIThread.
// Mirrors Tab.cpp's explicit relative include of the types ColorFix.
#include "../../types/inc/ColorFix.hpp"

// Workspaces M4 (#54, ADR-001): the reused per-tab color picker (the SAME control
// classic Tab and the workspace sidebar use). The per-tab "Color…" context-menu
// item opens it and routes its ColorSelected/ColorCleared as model intents.
#include "ColorPickupFlyout.h"

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
        // TabCloseRequested raises the VM's close intent.
        TabView().SelectionChanged({ this, &TabStripView::_onSelectionChanged });
        TabView().TabCloseRequested({ this, &TabStripView::_onTabCloseRequested });

        // Workspaces M6a + M6b (#54, ADR-001): tab drag. All three drag flags are
        // set TOGETHER in XAML — CanReorderTabs(true) (MUX reorders TabItems
        // internally for a WITHIN-leaf drag), CanDragTabs(true) + AllowDropTabs(true)
        // (the CROSS-leaf inter-control move). Because AllowDropTabs is true the
        // tear-out dispatcher is armed → a drag-out with no TabDroppedOutside handler
        // is the 0xc000027b failfast, so that handler MUST be wired here in the SAME
        // change as the flag flip (it is — _onTabDroppedOutside, an inert no-op
        // marking the M8 boundary).
        //
        // M6a (within-leaf): TabDragStarting opens the rearrange window; TabItemsChanged
        // records the MUX-internal from/to; TabDragCompleted translates from≠to into
        // the MoveTabRequested intent (the page dispatches moveTab and the diff
        // re-projects — no write-back here).
        //
        // M6b (cross-leaf, inter-control via DataPackage): TabDragStarting ALSO stuffs
        // the dragged VM's Id + our PID into the DataPackage; the DESTINATION strip
        // accepts the Move on TabStripDragOver (PID-guarded) and, on TabStripDrop,
        // hit-tests the drop index and raises the SAME MoveTabRequested intent with
        // ITS OWN LeafId as the destination, then reconciles both strips from the
        // model (the durable-divergence safety net — a cross-leaf moveTab CAN no-op).
        //
        // These wire ONCE (the TabView outlives every rebuild), mirroring the
        // selection intents above.
        TabView().TabDragStarting({ this, &TabStripView::_onTabDragStarting });
        TabView().TabItemsChanged({ this, &TabStripView::_onTabItemsChanged });
        TabView().TabDragCompleted({ this, &TabStripView::_onTabDragCompleted });
        TabView().TabStripDragOver({ this, &TabStripView::_onTabStripDragOver });
        TabView().TabStripDrop({ this, &TabStripView::_onTabStripDrop });
        TabView().TabDroppedOutside({ this, &TabStripView::_onTabDroppedOutside });
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
                    else if (prop == L"Pinned")
                    {
                        // Workspaces M5 (#54, ADR-001): the model projected a new
                        // pinned state onto the VM (via the TabDecorationUpdated diff
                        // arm → _setPaneTabPinnedForTab). Refresh THIS tab's
                        // context-menu Pin/Unpin label so it tracks the model — a pure
                        // downstream read, never a write-back. Re-resolve the matching
                        // TabViewItem by VM identity (mirrors the chrome-refresh path
                        // below). The full pinned-tab VISUALS (pin glyph, suppress
                        // close, sort-to-front) are DEFERRED — only the label toggles.
                        auto items = strongThis->TabView().TabItems();
                        for (uint32_t i = 0; i < items.Size(); ++i)
                        {
                            if (const auto item = items.GetAt(i).try_as<MUXC::TabViewItem>())
                            {
                                if (TabStripView::_vmFromItem(item) == vm)
                                {
                                    strongThis->_applyContextMenuPinLabel(item, vm);
                                    break;
                                }
                            }
                        }
                    }
                    else if (prop == L"Title" || prop == L"Icon" || prop == L"Background" || prop == L"BellIndicator" || prop == L"RuntimeColor")
                    {
                        // Workspaces M4 (#54, ADR-001): a RuntimeColor flip (the user
                        // color override, set/cleared via the per-tab "Color…" picker,
                        // returning from the model through _setPaneTabRuntimeColorForTab)
                        // rides the SAME chrome-refresh path as Background — _applyChrome
                        // re-reads vm.EffectiveBackground() (RuntimeColor wins over the
                        // live Background) and re-applies the tab color treatment.
                        //
                        // Workspaces M3 (#54, ADR-001): a BellIndicator flip rides
                        // the SAME chrome-refresh path as Title/Icon/Background —
                        // _applyChrome pushes the new state onto the header's
                        // TerminalTabStatus.BellIndicator. The off-thread origin of
                        // a bell (IPaneContent.BellRequested on the connection
                        // output thread) is already marshaled to the UI thread above
                        // (the HasThreadAccess-gated dispatcher hop), so this
                        // UIElement write is on the UI thread.
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

        // Workspaces M3 (#54, ADR-001): give the header a TerminalTabStatus so its
        // bound indicators (the BellIndicator FontIcon, etc. — see
        // TabHeaderControl.xaml) have a backing object to render from. Classic WT
        // builds one TerminalTabStatus per Tab (Tab.h _tabStatus) and the header
        // x:Binds its glyphs to it; mirror that here. _applyChrome drives its
        // BellIndicator from the VM each refresh. The header owns the status for the
        // life of the item.
        header.TabStatus(winrt::TerminalApp::TerminalTabStatus{});

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

        // Workspaces M5 (#54, ADR-001): attach the per-tab right-click context
        // menu. It mirrors classic Tab::_CreateContextMenu's CONSTRUCTION (a
        // MenuFlyout set as the TabViewItem.ContextFlyout) but each item raises a
        // TabId-scoped VM INTENT — never classic's _dispatch.DoAction(*this), which
        // is the reconcile pattern the big-flip forbids. The flyout is owned by the
        // item and released with it on the next _rebuildProjection (the items
        // weak-capture the VM / header, so a click after teardown is a safe no-op).
        item.ContextFlyout(_makeContextFlyout(item, vm, header));

        _applyChrome(item, vm);

        // Seed the Pin/Unpin label from the VM's projected pinned state (a later
        // Pinned PropertyChanged refreshes it via _applyContextMenuPinLabel).
        _applyContextMenuPinLabel(item, vm);
        return item;
    }

    // Workspaces M5 (#54, ADR-001): build the per-tab context menu. Mirrors classic
    // Tab::_CreateContextMenu's construction (MenuFlyoutItem + FontIcon glyph), but
    // routes every click as a model intent on the VM rather than classic's
    // _dispatch.DoAction reconcile. (Classic also sets a per-item HelpText tooltip;
    // the per-leaf menu omits it for now — the items are self-describing.) Entries
    // (model-ready only):
    //   * Rename — reuses M2's path: calls the hosted TabHeaderControl's
    //     BeginRename() (the SAME entry point the double-tap uses). Not a
    //     reimplementation — the renamer commit still flows TitleChangeRequested →
    //     RequestRename → setTabTitle.
    //   * Close — the existing M1 RequestClose intent (→ closeTab).
    //   * Pin / Unpin — the M5 RequestTogglePin intent (→ setTabPinned). The label
    //     toggles from the VM's projected Pinned state; the click requests the
    //     NEGATION, and the new state returns from the model (TabDecorationUpdated →
    //     Pinned). The Pin/Unpin MenuFlyoutItem is tagged with this VM so
    //     _applyContextMenuPinLabel can re-find it to refresh the label.
    // DEFERRED (model gaps / other slices): Color… (M4 — a clear spot is left
    // below), Duplicate / Close Other Tabs / Close Tabs to the Right (model gaps),
    // Move to New Window (M8).
    winrt::Windows::UI::Xaml::Controls::MenuFlyout TabStripView::_makeContextFlyout(const MUXC::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm, const winrt::TerminalApp::TabHeaderControl& header)
    {
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;
        namespace WUXMedia = winrt::Windows::UI::Xaml::Media;

        const auto fluentFont = WUXMedia::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" };

        WUXC::MenuFlyout flyout{};

        // "Rename tab" — reuse M2's renamer (the double-tap path). Weak-capture the
        // header so a late click on a torn-down tab is a safe no-op. Caveat: a
        // _rebuildProjection mid-flyout swaps in a NEW header/item, so this captured
        // header expires — the click then resolves to nothing and Rename is a
        // harmless no-op (the user simply re-opens the menu on the live row).
        {
            WUXC::MenuFlyoutItem renameItem{};
            WUXC::FontIcon renameSymbol{};
            renameSymbol.FontFamily(fluentFont);
            renameSymbol.Glyph(L"\xE8AC"); // Rename (matches classic Tab::_CreateContextMenu)
            renameItem.Icon(renameSymbol);
            renameItem.Text(RS_(L"RenameTabText"));
            renameItem.Click([weakHeader = winrt::make_weak(header)](auto&& /*s*/, auto&& /*e*/) {
                if (auto h{ weakHeader.get() })
                {
                    h.BeginRename();
                }
            });
            flyout.Items().Append(renameItem);
        }

        // "Change tab color" (Workspaces M4) — open the REUSED ColorPickupFlyout (the
        // same control classic Tab + the workspace sidebar use) anchored to the item;
        // its ColorSelected → vm.RequestSetColor (→ setTabColor), ColorCleared →
        // vm.RequestClearColor (→ setTabColor(null)). The user color is a MODEL
        // OVERRIDE (runtimeColor) that WINS over the live-terminal-bg tint
        // (EffectiveBackground) — like CustomTitle wins over the live title. The
        // picker is a SEPARATE flyout (NOT the item's ContextFlyout); _showColorPicker
        // owns its lifetime. Weak-capture the item so a late click on a torn-down tab
        // is a safe no-op. Glyph + label match classic Tab::_CreateContextMenu
        // (Tab.cpp:1651-1659: \xE790 + RS_(L"TabColorChoose")).
        {
            WUXC::MenuFlyoutItem colorItem{};
            WUXC::FontIcon colorSymbol{};
            colorSymbol.FontFamily(fluentFont);
            colorSymbol.Glyph(L"\xE790"); // Color (matches classic Tab::_CreateContextMenu)
            colorItem.Icon(colorSymbol);
            colorItem.Text(RS_(L"TabColorChoose"));
            colorItem.Click([weakThis = get_weak(), weakItem = winrt::make_weak(item), weakVm = winrt::make_weak(vm)](auto&& /*s*/, auto&& /*e*/) {
                auto strongThis{ weakThis.get() };
                auto i{ weakItem.get() };
                auto v{ weakVm.get() };
                if (strongThis && i && v)
                {
                    strongThis->_showColorPicker(i, v);
                }
            });
            flyout.Items().Append(colorItem);
        }

        // "Pin tab" / "Unpin tab" — the M5 RequestTogglePin intent. The label is set
        // by _applyContextMenuPinLabel (seeded at build, refreshed on the Pinned
        // PropertyChanged). Tag the item with the VM so that refresh can re-find it.
        // Weak-capture the VM so a late click after re-projection is a safe no-op;
        // the click requests the negation of the CURRENT projected Pinned state.
        {
            WUXC::MenuFlyoutItem pinItem{};
            WUXC::FontIcon pinSymbol{};
            pinSymbol.FontFamily(fluentFont);
            pinSymbol.Glyph(L"\xE718"); // Pinned (Segoe Fluent Icons)
            pinItem.Icon(pinSymbol);
            pinItem.Tag(winrt::box_value(L"PaneTabPinItem"));
            pinItem.Click([weakVm = winrt::make_weak(vm)](auto&& /*s*/, auto&& /*e*/) {
                if (auto v{ weakVm.get() })
                {
                    // Request the NEGATION of the current projected state; the new
                    // state returns from the model (no write-back here).
                    v.RequestTogglePin(!v.Pinned());
                }
            });
            flyout.Items().Append(pinItem);
        }

        flyout.Items().Append(WUXC::MenuFlyoutSeparator{});

        // "Close tab" — the existing M1 RequestClose intent (→ closeTab). The
        // single-tab "Close" only; the close SUB-MENU (Close other tabs / Close tabs
        // to the right) is DEFERRED (model gaps). Weak-capture the VM.
        {
            WUXC::MenuFlyoutItem closeItem{};
            closeItem.Text(RS_(L"TabClose"));
            closeItem.Click([weakVm = winrt::make_weak(vm)](auto&& /*s*/, auto&& /*e*/) {
                if (auto v{ weakVm.get() })
                {
                    v.RequestClose();
                }
            });
            flyout.Items().Append(closeItem);
        }

        return flyout;
    }

    // Workspaces M5 (#54, ADR-001): refresh the Pin/Unpin context-menu item's label
    // from the VM's projected Pinned state. The label toggle comes BACK from the
    // model (the TabDecorationUpdated diff arm → _setPaneTabPinnedForTab → Pinned),
    // so this is a pure downstream read — never a write-back. Re-finds the tagged
    // Pin item in the item's ContextFlyout (no stored handle to outlive the item).
    // Mirrors WorkspaceViewModel::PinLabel's literal Pin/Unpin (the workspace
    // sidebar's precedent). UI thread.
    void TabStripView::_applyContextMenuPinLabel(const MUXC::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm)
    {
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;

        const auto flyout = item.ContextFlyout().try_as<WUXC::MenuFlyout>();
        if (!flyout)
        {
            return;
        }

        const auto pinned = vm.Pinned();
        for (const auto& menuItem : flyout.Items())
        {
            if (const auto flyoutItem = menuItem.try_as<WUXC::MenuFlyoutItem>())
            {
                if (winrt::unbox_value_or<winrt::hstring>(flyoutItem.Tag(), winrt::hstring{}) == L"PaneTabPinItem")
                {
                    flyoutItem.Text(pinned ? winrt::hstring{ L"Unpin tab" } : winrt::hstring{ L"Pin tab" });
                    return;
                }
            }
        }
    }

    // Workspaces M4 (#54, ADR-001): open the REUSED ColorPickupFlyout anchored to the
    // tab item, mirroring classic Tab::AttachColorPicker (Tab.cpp:785-818). The
    // picker's ColorSelected raises the VM's RequestSetColor (→ setTabColor) and
    // ColorCleared raises RequestClearColor (→ setTabColor(null)); the new
    // runtimeColor returns from the model via the TabDecorationUpdated diff arm onto
    // the VM's RuntimeColor (which wins over the live Background in
    // EffectiveBackground). NEVER a click-site write-back — model-as-truth.
    //
    // Lifetime: hold the picker in _tabColorPickup for its open lifetime (so its
    // handlers stay alive while shown) and self-release on Closed (revoking the
    // handlers + nulling the member), exactly as classic does — its handlers must
    // not outlive a torn-down strip. The VM is weak-captured so a commit after a
    // re-projection is a safe no-op. UI thread (a user-initiated right-click click).
    void TabStripView::_showColorPicker(const MUXC::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm)
    {
        // A picker is already open — replace it (revoke its handlers first so the
        // stale ones can't fire). Mirrors the single-picker-at-a-time invariant.
        if (_tabColorPickup)
        {
            _tabColorPickup.ColorSelected(_colorSelectedToken);
            _tabColorPickup.ColorCleared(_colorClearedToken);
            _tabColorPickup.Closed(_colorPickerClosedToken);
            _tabColorPickup = nullptr;
        }

        _tabColorPickup = winrt::TerminalApp::ColorPickupFlyout{};

        const auto weakVm = winrt::make_weak(vm);

        _colorSelectedToken = _tabColorPickup.ColorSelected([weakVm](const winrt::Windows::UI::Color& color) {
            if (auto v{ weakVm.get() })
            {
                // Route the chosen color as a model intent (→ setTabColor); the new
                // runtimeColor returns from the model (no write-back here).
                v.RequestSetColor(color);
            }
        });

        _colorClearedToken = _tabColorPickup.ColorCleared([weakVm]() {
            if (auto v{ weakVm.get() })
            {
                // Route the clear as a model intent (→ setTabColor(null)).
                v.RequestClearColor();
            }
        });

        _colorPickerClosedToken = _tabColorPickup.Closed([weakThis = get_weak()](auto&& /*s*/, auto&& /*e*/) {
            // Release the picker + its handlers (mirrors classic Tab's Closed
            // handler) so they can't outlive a torn-down strip.
            if (auto strongThis{ weakThis.get() })
            {
                if (strongThis->_tabColorPickup)
                {
                    strongThis->_tabColorPickup.ColorSelected(strongThis->_colorSelectedToken);
                    strongThis->_tabColorPickup.ColorCleared(strongThis->_colorClearedToken);
                    strongThis->_tabColorPickup.Closed(strongThis->_colorPickerClosedToken);
                    strongThis->_tabColorPickup = nullptr;
                }
            }
        });

        // Anchor to the tab item (classic: _tabColorPickup.ShowAt(TabViewItem())).
        _tabColorPickup.ShowAt(item);
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

            // Workspaces M3 (#54, ADR-001): drive the header's bell/attention
            // indicator from the VM's runtime BellIndicator state. The header's
            // TerminalTabStatus.BellIndicator is x:Bind'd to a FontIcon's
            // Visibility (TabHeaderControl.xaml), so flipping it shows/hides the
            // bell glyph — mirroring classic Tab::ShowBellIndicator, which sets
            // _tabStatus.BellIndicator(show). The TerminalTabStatus was seeded in
            // _makeTabViewItem; guard defensively in case it is absent.
            if (const auto status = header.TabStatus())
            {
                status.BellIndicator(vm.BellIndicator());
            }
        }

        // Native tooltip (classic uses ToolTipService.SetToolTip on the
        // TabViewItem). Mirror the title.
        winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(item, winrt::box_value(title));

        // Workspaces M3 (#54, ADR-001) a11y fix: M2 swapped the TabViewItem's
        // Header from a plain string to a hosted TabHeaderControl, which left the
        // item's UIA automation Name empty (screen readers announced an unnamed
        // tab). Restore an accessible name by setting AutomationProperties.Name to
        // the effective (custom-wins) title — mirroring classic WT, which names its
        // tab via AutomationProperties::SetName(TabViewItem(), title) in
        // TabBase::UpdateTabHeader. Kept in sync here because _applyChrome runs both
        // at build time and on every Title PropertyChanged.
        winrt::Windows::UI::Xaml::Automation::AutomationProperties::SetName(item, title);

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

        // Workspaces M1.2 / M4 (#54, ADR-001): tint the tab. The EFFECTIVE background
        // is the user color override (RuntimeColor) when set, else the LIVE terminal
        // background color (Background) — exactly as CustomTitle wins over the live
        // title. vm.EffectiveBackground() folds that precedence:
        //   * RuntimeColor (M4 user override, the per-tab "Color…" picker) → a FRESH
        //     SolidColorBrush of that color, OR
        //   * the live Background (M1.2 — projected from the mounted IPaneContent's
        //     BackgroundBrush(), so the selected tab merges into its content), OR
        //   * nullptr (neither) → fall back to the native theme.
        // These are TWO DISTINCT concerns folded into one effective value here; the
        // classic color treatment (_applyTabColor) is identical regardless of source.
        if (const auto bgBrush = vm.EffectiveBackground().try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>())
        {
            _applyTabColor(item, til::color{ bgBrush.Color() });
        }
        else
        {
            _clearTabColor(item);
        }
    }

    // Workspaces M1.2 (#54, ADR-001): apply the classic color treatment to one
    // TabViewItem from a runtime color. A FAITHFUL port of classic
    // Tab::_ApplyTabColorOnUIThread (Tab.cpp ~line 2319): it computes the
    // selected / deselected / hover / pressed background brushes and a
    // luminance-based readable FOREGROUND, then writes them as local
    // per-TabViewItem theme-dictionary resource overrides (the exact same keys
    // classic sets). This runs on the UI thread — its only callers (_makeTabViewItem
    // at build time, the PropertyChanged path in _rebuildProjection) are both on
    // the UI thread; the off-thread content-color source already crossed the
    // HasThreadAccess()-gated marshaling choke point before reaching here.
    //
    // Fidelity note vs classic: classic layers the runtime color over a separate
    // _tabRowColor for its FONT-luminance decision (color.layer_over(_tabRowColor)).
    // The per-leaf strip carries no distinct tab-row color, and the live content
    // color is OPAQUE (extracted via ColorFromBrush → alpha 255), so for the
    // SELECTED tab layer_over(<anything>) == the color itself — the selected font
    // math is exact. For the DESELECTED tab classic layers the .3-alpha color over
    // the tab row; with no tab-row color we layer it over the opaque selected color
    // (the strip's own backdrop), which is the closest faithful analogue.
    void TabStripView::_applyTabColor(const MUXC::TabViewItem& item, const til::color& color)
    {
        namespace WUXMedia = winrt::Windows::UI::Xaml::Media;
        using winrt::Windows::UI::Xaml::ResourceDictionary;

        constexpr auto lightnessThreshold = 0.6f;

        WUXMedia::SolidColorBrush selectedTabBrush{};
        WUXMedia::SolidColorBrush deselectedTabBrush{};
        WUXMedia::SolidColorBrush fontBrush{};
        WUXMedia::SolidColorBrush deselectedFontBrush{};
        WUXMedia::SolidColorBrush secondaryFontBrush{};
        WUXMedia::SolidColorBrush hoverTabBrush{};
        WUXMedia::SolidColorBrush subtleFillColorSecondaryBrush{};
        WUXMedia::SolidColorBrush subtleFillColorTertiaryBrush{};

        // Luminance of the (selected) color picks the close-button fill (classic).
        if (ColorFix::GetLightness(color) >= lightnessThreshold)
        {
            auto subtleFillColorSecondary = winrt::Windows::UI::Colors::Black();
            subtleFillColorSecondary.A = 0x09;
            subtleFillColorSecondaryBrush.Color(subtleFillColorSecondary);
            auto subtleFillColorTertiary = winrt::Windows::UI::Colors::Black();
            subtleFillColorTertiary.A = 0x06;
            subtleFillColorTertiaryBrush.Color(subtleFillColorTertiary);
        }
        else
        {
            auto subtleFillColorSecondary = winrt::Windows::UI::Colors::White();
            subtleFillColorSecondary.A = 0x0F;
            subtleFillColorSecondaryBrush.Color(subtleFillColorSecondary);
            auto subtleFillColorTertiary = winrt::Windows::UI::Colors::White();
            subtleFillColorTertiary.A = 0x0A;
            subtleFillColorTertiaryBrush.Color(subtleFillColorTertiary);
        }

        // The SELECTED-tab font is based on the luminance of the (opaque) color —
        // classic layers it over _tabRowColor first, which is a no-op for an opaque
        // color, so this matches classic exactly for the live-content case.
        if (ColorFix::GetLightness(color) >= lightnessThreshold)
        {
            fontBrush.Color(winrt::Windows::UI::Colors::Black());
            auto secondaryFontColor = winrt::Windows::UI::Colors::Black();
            secondaryFontColor.A = 0x9E;
            secondaryFontBrush.Color(secondaryFontColor);
        }
        else
        {
            fontBrush.Color(winrt::Windows::UI::Colors::White());
            auto secondaryFontColor = winrt::Windows::UI::Colors::White();
            secondaryFontColor.A = 0xC5;
            secondaryFontBrush.Color(secondaryFontColor);
        }

        selectedTabBrush.Color(color);

        // Deselected = the same color at Opacity .3 (classic: with_alpha(77)). No
        // theme-override branch here — that path is the M4 user/theme override, not
        // the live content color.
        const auto deselectedTabColor = color.with_alpha(77); // 255 * .3
        deselectedTabBrush.Color(deselectedTabColor.with_alpha(255));
        deselectedTabBrush.Opacity(deselectedTabColor.a / 255.f);

        hoverTabBrush.Color(color);
        hoverTabBrush.Opacity(0.6);

        // Deselected font: classic layers the .3 color over _tabRowColor; with no
        // tab-row color we layer over the opaque selected color (the strip backdrop).
        const auto deselectedActualColor = deselectedTabColor.layer_over(color);
        if (ColorFix::GetLightness(deselectedActualColor) >= lightnessThreshold)
        {
            deselectedFontBrush.Color(winrt::Windows::UI::Colors::Black());
        }
        else
        {
            deselectedFontBrush.Color(winrt::Windows::UI::Colors::White());
        }

        // Empty theme dictionaries so HighContrast can carry its own adjustments
        // (classic does the same; HC overrides a couple of foreground keys).
        const auto& tabItemThemeResources{ item.Resources().ThemeDictionaries() };
        ResourceDictionary lightThemeDictionary;
        ResourceDictionary darkThemeDictionary;
        ResourceDictionary highContrastThemeDictionary;
        tabItemThemeResources.Insert(winrt::box_value(L"Light"), lightThemeDictionary);
        tabItemThemeResources.Insert(winrt::box_value(L"Dark"), darkThemeDictionary);
        tabItemThemeResources.Insert(winrt::box_value(L"HighContrast"), highContrastThemeDictionary);

        // The unselected resting background (GH#11382: never null — kills hit test).
        item.Background(deselectedTabBrush);

        for (const auto& [k, v] : tabItemThemeResources)
        {
            const bool isHighContrast = winrt::unbox_value<hstring>(k) == L"HighContrast";
            const auto& currentDictionary = v.as<ResourceDictionary>();

            // TabViewItem.Background
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackground"), selectedTabBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundSelected"), selectedTabBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundPointerOver"), isHighContrast ? fontBrush : hoverTabBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundPressed"), selectedTabBrush);

            // TabViewItem.Foreground (aka text)
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForeground"), deselectedFontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForegroundSelected"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForegroundPointerOver"), isHighContrast ? selectedTabBrush : fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderForegroundPressed"), fontBrush);

            // TabViewItem.CloseButton.Foreground (aka X)
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForeground"), deselectedFontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForegroundPressed"), isHighContrast ? deselectedFontBrush : secondaryFontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForegroundPointerOver"), isHighContrast ? deselectedFontBrush : fontBrush);

            // TabViewItem.CloseButton.Foreground _when_ interacting with the tab
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderPressedCloseButtonForeground"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderPointerOverCloseButtonForeground"), isHighContrast ? selectedTabBrush : fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderSelectedCloseButtonForeground"), fontBrush);

            // TabViewItem.CloseButton.Background (aka X button)
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackgroundPressed"), isHighContrast ? selectedTabBrush : subtleFillColorTertiaryBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackgroundPointerOver"), isHighContrast ? selectedTabBrush : subtleFillColorSecondaryBrush);

            // A few miscellaneous resources that WinUI said may be removed in the future
            currentDictionary.Insert(winrt::box_value(L"TabViewButtonForegroundActiveTab"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewButtonForegroundPressed"), fontBrush);
            currentDictionary.Insert(winrt::box_value(L"TabViewButtonForegroundPointerOver"), fontBrush);

            // BODGY (classic): Insert() throws if the key already exists, so only
            // add the HC-only border keys in the HC dictionary.
            if (isHighContrast)
            {
                currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBorderBrushPressed"), fontBrush);
                currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBorderBrushPointerOver"), fontBrush);
                currentDictionary.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBorderBrushSelected"), fontBrush);
            }
        }
    }

    // Workspaces M1.2 (#54, ADR-001): clear any tab-color overrides, falling back
    // to the native TabView theme. A port of classic Tab::_ClearTabBackgroundColor
    // (Tab.cpp ~line 2501) — removes every key from each theme dictionary and
    // resets the resting Background to Transparent (GH#11382: NOT null — a null
    // background is not hit-testable; Transparent is). Used when the VM carries no
    // content color yet (nullptr Background before ContentMounted seeds it).
    void TabStripView::_clearTabColor(const MUXC::TabViewItem& item)
    {
        static const winrt::hstring keys[] = {
            L"TabViewItemHeaderBackground",
            L"TabViewItemHeaderBackgroundSelected",
            L"TabViewItemHeaderBackgroundPointerOver",
            L"TabViewItemHeaderBackgroundPressed",

            L"TabViewItemHeaderForeground",
            L"TabViewItemHeaderForegroundSelected",
            L"TabViewItemHeaderForegroundPointerOver",
            L"TabViewItemHeaderForegroundPressed",

            L"TabViewItemHeaderCloseButtonForeground",
            L"TabViewItemHeaderCloseButtonForegroundPointerOver",
            L"TabViewItemHeaderCloseButtonForegroundPressed",

            L"TabViewItemHeaderPressedCloseButtonForeground",
            L"TabViewItemHeaderPointerOverCloseButtonForeground",
            L"TabViewItemHeaderSelectedCloseButtonForeground",

            L"TabViewItemHeaderCloseButtonBackgroundPressed",
            L"TabViewItemHeaderCloseButtonBackgroundPointerOver",

            L"TabViewButtonForegroundActiveTab",
            L"TabViewButtonForegroundPressed",
            L"TabViewButtonForegroundPointerOver",

            L"TabViewItemHeaderCloseButtonBorderBrushPressed",
            L"TabViewItemHeaderCloseButtonBorderBrushPointerOver",
            L"TabViewItemHeaderCloseButtonBorderBrushSelected"
        };

        const auto& tabItemThemeResources{ item.Resources().ThemeDictionaries() };
        for (const auto& keyString : keys)
        {
            const auto key = winrt::box_value(keyString);
            for (const auto& [_, v] : tabItemThemeResources)
            {
                const auto& themeDictionary = v.as<winrt::Windows::UI::Xaml::ResourceDictionary>();
                themeDictionary.Remove(key);
            }
        }

        // GH#11382: Transparent, not null, so the tab stays hit-testable.
        item.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() });
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

    // Workspaces M6a + M6b (#54, ADR-001): a drag began on THIS strip.
    //
    // M6a (within-leaf): open the rearrange window and clear the captured from/to
    // (ports classic TerminalPage::_TabDragStarted, TabManagement.cpp:1275). While
    // _rearranging is set, _onTabItemsChanged interprets the MUX-internal TabItems
    // mutation as the optimistic reorder write-back (recording its indices) rather
    // than a projection rebuild.
    //
    // M6b (cross-leaf): stuff the DataPackage with the dragged VM's stable Id +
    // our process id, and request a Move (ports classic
    // TerminalPage::_onTabDragStarting, TerminalPage.cpp:5907-5951 — but we carry
    // the model TabId, NOT a windowId/Tab handle; the destination strip resolves
    // the move purely from `paneTabId`). The PID lets a sibling strip's
    // TabStripDragOver/Drop reject a drag from the classic window's TabView or
    // another process. The dragged VM is resolved AT FIRE TIME from the dragged
    // TabViewItem's Tag (args.Tab()) — a rebuild mid-gesture could swap the item,
    // so we never index TabItems positionally. A null VM (a torn-down row) leaves
    // no payload, so the drop is a safe no-op.
    void TabStripView::_onTabDragStarting(const MUXC::TabView& /*sender*/, const MUXC::TabViewTabDragStartingEventArgs& args)
    {
        _rearranging = true;
        _rearrangeFrom = std::nullopt;
        _rearrangeTo = std::nullopt;

        // M6b: identify the dragged tab to a potential cross-leaf drop target.
        const auto item = args.Tab().try_as<MUXC::TabViewItem>();
        if (const auto vm = _vmFromItem(item))
        {
            const auto& props = args.Data().Properties();
            props.Insert(L"paneTabId", winrt::box_value<uint64_t>(vm.Id()));
            props.Insert(L"pid", winrt::box_value<uint32_t>(GetCurrentProcessId()));
            args.Data().RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        }
    }

    // Workspaces M6b (#54, ADR-001): a tab is being dragged OVER this strip (the
    // potential DROP TARGET). We must mark the operation Move or the system never
    // delivers TabStripDrop (mirrors classic TerminalPage::_onTabStripDragOver,
    // TerminalPage.cpp:5953-5970). Accept ONLY a drag carrying our `paneTabId` AND
    // our own process id (the PID guard rejects a drag from the classic window's
    // TabView or another process — we never interop with a foreign TabView).
    void TabStripView::_onTabStripDragOver(const IInspectable& /*sender*/, const winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        const auto& props = e.DataView().Properties();
        if (props.HasKey(L"paneTabId") &&
            props.HasKey(L"pid") &&
            winrt::unbox_value_or<uint32_t>(props.TryLookup(L"pid"), 0u) == GetCurrentProcessId())
        {
            e.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        }
    }

    // Workspaces M6b (#54, ADR-001): a tab from a SIBLING leaf's strip was dropped
    // on THIS strip (the DROP TARGET). Resolve the dragged VM's stable Id from the
    // DataPackage, compute the drop index by hit-testing the pointer against this
    // strip's TabViewItem containers (ports classic TerminalPage::_onTabStripDrop,
    // TerminalPage.cpp:6008-6024), and raise the SAME strip-level MoveTabRequested
    // intent the within-leaf reorder uses — with THIS strip's own LeafId as the
    // move destination (the page reads it at dispatch time). The model relocates
    // the tab (Stage 0 apply(TabMoved) → _movePaneTabVm) preserving the live
    // IPaneContent instance; the diff's TabMoved re-projects both strips.
    //
    // DURABLE-DIVERGENCE SAFETY NET: unlike the M6a within-leaf path, a cross-leaf
    // moveTab CAN no-op — Actions_Move.cpp:113/119 returns the SAME state pointer
    // if the tab isn't found or the dst leaf doesn't exist, so _applyWorkspaceAction
    // diffs state-against-itself, emits nothing, and triggers NO rebuild. To keep
    // the model authoritative regardless of the action outcome we ALWAYS reconcile
    // after raising the intent: force a re-projection of this (destination) strip
    // from _source (idempotent + cheap). The SOURCE strip needs no manual reconcile
    // here — an inter-control drop does NOT mutate the source's TabItems (MUX cannot
    // mutate across two separate collections; only the within-leaf CanReorderTabs
    // path mutates TabItems, and that fires TabDragCompleted on the source, not a
    // cross-strip Drop), so the source strip's projection is never optimistically
    // stranded. (The successful-move case re-projects the source strip anyway via
    // the TabMoved diff's VectorChanged on the source collection.)
    void TabStripView::_onTabStripDrop(const IInspectable& /*sender*/, const winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        // Split (slice 3c): the hit-test is the ONLY part that needs the framework
        // DragEventArgs (its coordinate transform); the data-resolution +
        // intent-raise + reconcile core is _resolveCrossLeafDrop, which the drag rig
        // drives directly with a constructed DataPackage view (a DragEventArgs is
        // not publicly constructible). Behaviour is unchanged: the hit-test has no
        // side effects, so running it before the PID/payload guard (which now lives
        // in the core) is equivalent — and in practice _onTabStripDragOver only
        // accepts same-process Move drags, so this fires only for our own drags.
        _resolveCrossLeafDrop(e.DataView().Properties(), _dropIndexFromPoint(e));
    }

    // Hit-test `e`'s drop point against this strip's TabViewItem containers and
    // return the insertion index (ports classic TerminalPage.cpp:6008-6024). If the
    // pointer is on the left half of a tab, insert before it; if it is past every
    // tab, append (the page clamps to [0, size], and moveTab clamps again).
    uint32_t TabStripView::_dropIndexFromPoint(const winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        auto dropIdx = -1;
        const auto tabView = TabView();
        const auto items = tabView.TabItems();
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            if (const auto item = tabView.ContainerFromIndex(i).try_as<MUXC::TabViewItem>())
            {
                const auto posX = e.GetPosition(item).X; // drop point relative to the tab
                const auto itemWidth = item.ActualWidth();
                if (posX < itemWidth / 2)
                {
                    dropIdx = static_cast<int>(i);
                    break;
                }
            }
        }
        return dropIdx < 0 ? items.Size() : static_cast<uint32_t>(dropIdx);
    }

    // The cross-leaf drop RESOLUTION core (no framework DragEventArgs). PID guard —
    // reject a drop from the classic window's TabView / another process (mirror
    // classic TerminalPage.cpp:5979-5993). Resolve the dragged VM's stable Id from
    // the DataPackage, then raise the strip-level intent: the page dispatches
    // moveTab(state, paneTabId, THIS strip's LeafId, dropIdx) and the resulting
    // TabMoved diff re-projects both strips (model-as-truth — never a write-back at
    // the drop site). Then reconcile THIS strip from the model even if the move
    // no-op'd (the durable-divergence safety net — a cross-leaf moveTab CAN no-op,
    // see the _onTabStripDrop banner; re-projecting from _source when the model
    // already matches is a structural no-op). Extracted so the drag rig can drive it
    // with a constructed DataPackage view (slice 3c).
    void TabStripView::_resolveCrossLeafDrop(const winrt::Windows::ApplicationModel::DataTransfer::DataPackagePropertySetView& props, uint32_t dropIdx)
    {
        const auto pidObj = props.TryLookup(L"pid");
        if (!pidObj || winrt::unbox_value_or<uint32_t>(pidObj, 0u) != GetCurrentProcessId())
        {
            return;
        }

        const auto tabIdObj = props.TryLookup(L"paneTabId");
        if (!tabIdObj)
        {
            return;
        }
        const auto paneTabId = winrt::unbox_value_or<uint64_t>(tabIdObj, 0ull);

        MoveTabRequested.raise(paneTabId, dropIdx);
        _rebuildProjection();
    }

    // Workspaces M6b (#54, ADR-001): a tab was released OUTSIDE every strip (the
    // tear-out gesture). This is the M8 boundary — moving a tab to a NEW window
    // requires monarch/IPC/ConPTY rehydration and is DEFERRED. This handler is an
    // INERT no-op present SOLELY to satisfy the AllowDropTabs(true) contract: a
    // draggable TabView with no TabDroppedOutside handler crashes
    // Windows.UI.Xaml.dll on drag-out (0xc000027b — see reference_mux_tabview_drag).
    // Do NOT create a window / touch monarch/IPC/RequestReceiveContent/_stashed/
    // _MoveContent here — that is M8. Dropping a tab outside the strips simply
    // leaves it where it was (the model is unchanged, so the projection is correct).
    void TabStripView::_onTabDroppedOutside(const MUXC::TabView& /*sender*/, const MUXC::TabViewTabDroppedOutsideEventArgs& /*args*/)
    {
        // Intentionally empty — M8 (tear-out to a new window) is deferred.
    }

    // Workspaces M6a (#54, ADR-001): TabItems membership changed. When a reorder is
    // in flight, MUX mutates TabItems internally (ItemRemoved at the old index +
    // ItemInserted at the new) — record those as from/to (ports classic
    // TerminalPage::_OnTabItemsChanged's _rearrangeFrom/_rearrangeTo capture,
    // TabManagement.cpp:1037). Outside a rearrange this fires for our own
    // _rebuildProjection churn and is ignored (the rebuild is the projection; we do
    // NOT re-derive anything from the control here).
    void TabStripView::_onTabItemsChanged(const IInspectable& /*sender*/, const winrt::Windows::Foundation::Collections::IVectorChangedEventArgs& args)
    {
        if (!_rearranging)
        {
            return;
        }
        switch (args.CollectionChange())
        {
        case winrt::Windows::Foundation::Collections::CollectionChange::ItemRemoved:
            _rearrangeFrom = args.Index();
            break;
        case winrt::Windows::Foundation::Collections::CollectionChange::ItemInserted:
            _rearrangeTo = args.Index();
            break;
        default:
            break;
        }
    }

    // Workspaces M6a (#54, ADR-001): a within-leaf drag finished. If MUX actually
    // reordered (from≠to), translate the captured indices into the strip-level
    // MoveTabRequested(tabId, dstIdx) intent and let the page dispatch moveTab —
    // the resulting TabMoved diff re-projects TabItems from the model
    // (authoritative). We DO NOT write the reorder back into the model here: MUX's
    // optimistic visual reorder is accepted, then reconciled by the projection (the
    // common case — model agrees — is a structural no-op; a divergence is visually
    // corrected). Resolve the dragged VM by its Tag (resolve-at-fire-time — a
    // rebuild mid-drag could swap items), reading args.Tab() (the moved
    // TabViewItem). Ports classic TerminalPage::_TabDragCompleted
    // (TabManagement.cpp:1283), but raises an INTENT instead of mutating _tabs.
    void TabStripView::_onTabDragCompleted(const MUXC::TabView& /*sender*/, const MUXC::TabViewTabDragCompletedEventArgs& args)
    {
        const auto from = _rearrangeFrom;
        const auto to = _rearrangeTo;
        _rearranging = false;
        _rearrangeFrom = std::nullopt;
        _rearrangeTo = std::nullopt;

        if (!from.has_value() || !to.has_value() || *from == *to)
        {
            // No reorder (a click, or a drag that landed in place). Nothing to
            // dispatch — the projection is already correct.
            return;
        }

        // Resolve the dragged VM's stable Id from the moved TabViewItem's Tag. We
        // prefer args.Tab() (the dragged item the gesture carries) over indexing
        // TabItems[to] so a rebuild that swapped the items mid-drag cannot
        // mis-target. A null VM (a torn-down row) makes this a safe no-op.
        const auto item = args.Tab().try_as<MUXC::TabViewItem>();
        const auto vm = _vmFromItem(item);
        if (!vm)
        {
            return;
        }

        // Raise the strip-level intent; the destination leaf is THIS strip's own
        // LeafId (the page reads it at dispatch time). dstIdx = the new index MUX
        // settled on.
        MoveTabRequested.raise(vm.Id(), *to);
    }
}
