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

// Workspaces #55 (custom drag): the in-process reorder pointer gesture — routed
// pointer events + pointer capture, an Escape-cancel CoreWindow key hook, and
// VisualTreeHelper to walk a pressed element up to its TabViewItem.
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.h>

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

        // Workspaces #55 (custom drag): the within-leaf reorder pointer gesture.
        // MUX's built-in tab drag is OFF (all three flags false in XAML — it arms a
        // shell CoreDragOperation that fails E_ACCESSDENIED → 0xc000027b here; see the
        // .h banner). Instead, pointer handlers on the TabView detect the gesture and a
        // transparent overlay captures it. We register the TabView handlers with
        // AddHandler(handledEventsToo=true) so the TabViewItem's own pointer handling
        // (selection visual states) can't swallow them before we see the press/move.
        // The overlay's own handlers (plain events — nothing else touches the overlay)
        // drive the captured phase. All wire ONCE; the TabView + overlay outlive every
        // projection rebuild, so `this` (raw — no refcount, no cycle) is safe: neither
        // element can outlive the strip that owns them.
        TabView().AddHandler(UIElement::PointerPressedEvent(),
                             winrt::box_value(Input::PointerEventHandler{ this, &TabStripView::_onStripPointerPressed }),
                             true);
        TabView().AddHandler(UIElement::PointerMovedEvent(),
                             winrt::box_value(Input::PointerEventHandler{ this, &TabStripView::_onStripPointerMoved }),
                             true);
        DragOverlay().PointerMoved({ this, &TabStripView::_onOverlayPointerMoved });
        DragOverlay().PointerReleased({ this, &TabStripView::_onOverlayPointerReleased });
        DragOverlay().PointerCaptureLost({ this, &TabStripView::_onOverlayPointerCaptureLost });
        DragOverlay().PointerCanceled({ this, &TabStripView::_onOverlayPointerCaptureLost });
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

    // ====================================================================== //
    // Workspaces #55 (custom drag): the within-leaf reorder pointer gesture.
    // No CoreDragOperation (the shell drag fails E_ACCESSDENIED → 0xc000027b here;
    // see the .h banner). A press-then-threshold on the TabView promotes to an
    // overlay-captured drag that draws an insertion line and, on release, raises the
    // MoveTabRequested intent. The gesture NEVER optimistically mutates TabItems —
    // the model diff re-projects, so there is nothing to reconcile (cleaner than the
    // retired MUX accept-then-reconcile path).
    // ====================================================================== //

    // A left/primary press over a tab arms a CANDIDATE drag (no capture, not handled
    // — a plain click must still select the tab and the close button must still
    // work; the threshold decides click-vs-drag). Resolve the dragged VM by walking
    // the pressed element up to its enclosing TabViewItem.
    void TabStripView::_onStripPointerPressed(const IInspectable& /*sender*/, const Input::PointerRoutedEventArgs& e)
    {
        if (!e.GetCurrentPoint(TabView()).Properties().IsLeftButtonPressed())
        {
            return;
        }

        winrt::TerminalApp::PaneTabViewModel vm{ nullptr };
        auto node = e.OriginalSource().try_as<DependencyObject>();
        while (node)
        {
            if (const auto item = node.try_as<MUXC::TabViewItem>())
            {
                vm = _vmFromItem(item);
                break;
            }
            node = Media::VisualTreeHelper::GetParent(node);
        }
        if (!vm)
        {
            return; // the press was not on a tab (strip background, etc.)
        }

        _reorderCandidate = true;
        _reorderVm = winrt::make_weak(vm);
        _reorderStart = e.GetCurrentPoint(TabView()).Position();
    }

    // While a candidate is armed, promote to an active drag once the pointer moves
    // past a small threshold. (If the button came up without us seeing a release —
    // we never captured during the candidate phase — drop the candidate.)
    void TabStripView::_onStripPointerMoved(const IInspectable& /*sender*/, const Input::PointerRoutedEventArgs& e)
    {
        if (!_reorderCandidate || _reorderActive)
        {
            return;
        }
        if (!e.GetCurrentPoint(TabView()).Properties().IsLeftButtonPressed())
        {
            _reorderCandidate = false;
            return;
        }

        const auto pos = e.GetCurrentPoint(TabView()).Position();
        const auto dx = pos.X - _reorderStart.X;
        const auto dy = pos.Y - _reorderStart.Y;
        constexpr double threshold = 4.0; // ~SM_CXDRAG: a click never starts a drag
        if ((dx * dx + dy * dy) >= (threshold * threshold))
        {
            _beginReorderDrag(e);
        }
    }

    // Promote the candidate to an active drag: show + capture the OVERLAY (not the
    // TabViewItem — MUX un-refcounted-CapturePointer()s its items internally and
    // would steal/lose our capture mid-drag; TabManagement.cpp:1081), hook Escape,
    // and draw the first insertion line.
    void TabStripView::_beginReorderDrag(const Input::PointerRoutedEventArgs& e)
    {
        _reorderActive = true;
        _reorderLastGap = UINT32_MAX;

        DragOverlay().Visibility(Visibility::Visible);
        DragOverlay().IsHitTestVisible(true);
        if (!DragOverlay().CapturePointer(e.Pointer()))
        {
            _endReorderDrag(); // never strand a half-armed drag
            return;
        }

        // A captured pointer does not capture the keyboard, so hook the window key
        // feed for the drag's lifetime (revoked in _endReorderDrag) to catch Escape.
        if (const auto coreWindow = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())
        {
            _reorderEscRevoker = coreWindow.KeyDown(winrt::auto_revoke, [this](auto&&, const winrt::Windows::UI::Core::KeyEventArgs& args) {
                if (_reorderActive && args.VirtualKey() == winrt::Windows::System::VirtualKey::Escape)
                {
                    args.Handled(true);
                    _endReorderDrag(); // cancel — no MoveTabRequested raised
                }
            });
        }

        _updateReorderAdorner(e);
    }

    // Reposition the insertion line at the current drop gap, only when the gap
    // changes (PointerMoved fires continuously). The line spans the tab row's height.
    void TabStripView::_updateReorderAdorner(const Input::PointerRoutedEventArgs& e)
    {
        double top = 0.0;
        double height = 0.0;
        const auto tabs = _currentTabLayout(top, height);
        if (tabs.empty())
        {
            return;
        }

        const auto pointerX = static_cast<double>(e.GetCurrentPoint(TabView()).Position().X);
        const auto gap = _dropGapFromGeometry(pointerX, tabs);
        if (gap == _reorderLastGap)
        {
            return;
        }
        _reorderLastGap = gap;

        const auto n = static_cast<uint32_t>(tabs.size());
        const auto lineX = (gap < n) ? tabs[gap].left : tabs[n - 1].left + tabs[n - 1].width;

        const auto line = InsertionIndicator();
        Canvas::SetLeft(line, lineX);
        Canvas::SetTop(line, top);
        line.Height(height);
        line.Visibility(Visibility::Visible);
    }

    // The OVERLAY owns the captured pointer for the active phase.
    void TabStripView::_onOverlayPointerMoved(const IInspectable& /*sender*/, const Input::PointerRoutedEventArgs& e)
    {
        if (_reorderActive)
        {
            _updateReorderAdorner(e);
        }
    }

    void TabStripView::_onOverlayPointerReleased(const IInspectable& /*sender*/, const Input::PointerRoutedEventArgs& e)
    {
        if (!_reorderActive)
        {
            return;
        }
        e.Handled(true);
        _finishReorderDrag(e); // raises MoveTabRequested unless the drop is a no-op
        _endReorderDrag();
    }

    // A lost/cancelled capture (Escape already releases via _endReorderDrag, app
    // deactivation, an external steal) ABORTS — no MoveTabRequested. Idempotent: the
    // release path clears _reorderActive before releasing capture, so the trailing
    // PointerCaptureLost is a no-op.
    void TabStripView::_onOverlayPointerCaptureLost(const IInspectable& /*sender*/, const Input::PointerRoutedEventArgs& /*e*/)
    {
        if (_reorderActive)
        {
            _endReorderDrag();
        }
    }

    // Resolve the dragged VM's CURRENT index (by identity — a rebuild could have
    // reordered TabItems mid-drag) and the live layout, map the pointer to a moveTab
    // dstIdx, and raise the intent. A no-op drop (back into its own slot) is skipped.
    void TabStripView::_finishReorderDrag(const Input::PointerRoutedEventArgs& e)
    {
        const auto vm = _reorderVm.get();
        if (!vm)
        {
            return; // the dragged tab was torn down mid-gesture
        }

        const auto items = TabView().TabItems();
        std::optional<uint32_t> srcIdx{ std::nullopt };
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            if (_vmFromItem(items.GetAt(i).try_as<MUXC::TabViewItem>()) == vm)
            {
                srcIdx = i;
                break;
            }
        }
        if (!srcIdx.has_value())
        {
            return;
        }

        double top = 0.0;
        double height = 0.0;
        const auto tabs = _currentTabLayout(top, height);
        if (tabs.empty())
        {
            return;
        }

        const auto pointerX = static_cast<double>(e.GetCurrentPoint(TabView()).Position().X);
        const auto gap = _dropGapFromGeometry(pointerX, tabs);
        const auto dst = _dstIndexFromGap(gap, *srcIdx);
        if (!dst.has_value())
        {
            return; // dropped in place — skip the dispatch (and a pointless re-project)
        }

        // The SAME intent the page dispatches as moveTab(state, tabId, LeafId, dst);
        // the TabMoved diff re-projects this strip in model order. We never mutated
        // TabItems, so there is nothing to reconcile.
        MoveTabRequested.raise(vm.Id(), *dst);
    }

    // Shared teardown for every terminal path (release, capture-lost, Escape, a
    // failed capture). Idempotent: clears the gesture state, hides the adorner +
    // overlay, drops the Escape hook, and releases any pointer capture (which fires
    // a PointerCaptureLost that no-ops because _reorderActive is already false).
    void TabStripView::_endReorderDrag()
    {
        _reorderActive = false;
        _reorderCandidate = false;
        _reorderLastGap = UINT32_MAX;
        _reorderVm = nullptr;
        _reorderEscRevoker.revoke();

        InsertionIndicator().Visibility(Visibility::Collapsed);
        DragOverlay().IsHitTestVisible(false);
        DragOverlay().Visibility(Visibility::Collapsed);
        DragOverlay().ReleasePointerCaptures();
    }

    // Gather every projected tab's strip-relative box, in projection order, in
    // TabView coordinates — the same reference the pointer reads use
    // (GetCurrentPoint(TabView())). The TabView is always laid out, whereas the
    // DragOverlay is Collapsed until a drag begins; the overlay shares the TabView's
    // origin (both fill the wrapping Grid cell), so a TabView-space X is also the
    // correct Canvas.Left for the insertion line. Also reports the tab row's top +
    // height for the line. Needs layout (runtime only); the PURE index math below is
    // what the unit tests drive. If any container is not realized the layout would be
    // misaligned, so bail to empty (a safe no-op).
    std::vector<TabStripView::TabExtent> TabStripView::_currentTabLayout(double& outTop, double& outHeight)
    {
        outTop = 0.0;
        outHeight = 0.0;

        std::vector<TabExtent> tabs;
        const auto tabView = TabView();
        const auto items = tabView.TabItems();
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            const auto container = tabView.ContainerFromIndex(i).try_as<MUXC::TabViewItem>();
            if (!container)
            {
                return {}; // an unrealized container would misalign indices
            }
            const auto topLeft = container.TransformToVisual(tabView).TransformPoint({ 0.0f, 0.0f });
            tabs.push_back(TabExtent{ static_cast<double>(topLeft.X), container.ActualWidth() });
            if (outHeight == 0.0)
            {
                outTop = static_cast<double>(topLeft.Y);
                outHeight = container.ActualHeight();
            }
        }
        return tabs;
    }

    // PURE: count the tabs whose horizontal MIDPOINT is at/left of pointerX — the
    // visual gap index in [0, N] the dragged tab would land in with every tab still
    // present. (Mirrors classic's left-half drop hit-test, stable layout.)
    uint32_t TabStripView::_dropGapFromGeometry(double pointerX, const std::vector<TabExtent>& tabs)
    {
        uint32_t gap = 0;
        for (const auto& t : tabs)
        {
            if (pointerX < t.left + t.width / 2.0)
            {
                break;
            }
            ++gap;
        }
        return gap;
    }

    // PURE: convert a visual gap (dragged tab still present) to the moveTab dstIdx,
    // which the model applies AFTER removing the dragged tab (Actions_Move.cpp
    // erase-then-insert): a gap right of the source loses one slot. nullopt when the
    // order would not change (dropping into the source's own slot or its right edge).
    std::optional<uint32_t> TabStripView::_dstIndexFromGap(uint32_t gap, uint32_t srcIdx)
    {
        if (gap == srcIdx || gap == srcIdx + 1)
        {
            return std::nullopt;
        }
        return gap > srcIdx ? gap - 1 : gap;
    }
}
