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
                    else if (prop == L"Title" || prop == L"Icon" || prop == L"Background" || prop == L"BellIndicator")
                    {
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
        item.ContextFlyout(_makeContextFlyout(vm, header));

        _applyChrome(item, vm);

        // Seed the Pin/Unpin label from the VM's projected pinned state (a later
        // Pinned PropertyChanged refreshes it via _applyContextMenuPinLabel).
        _applyContextMenuPinLabel(item, vm);
        return item;
    }

    // Workspaces M5 (#54, ADR-001): build the per-tab context menu. Mirrors classic
    // Tab::_CreateContextMenu's construction (MenuFlyoutItem + FontIcon glyph +
    // tooltip), but routes every click as a model intent on the VM rather than
    // classic's _dispatch.DoAction reconcile. Entries (model-ready only):
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
    winrt::Windows::UI::Xaml::Controls::MenuFlyout TabStripView::_makeContextFlyout(const winrt::TerminalApp::PaneTabViewModel& vm, const winrt::TerminalApp::TabHeaderControl& header)
    {
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;
        namespace WUXMedia = winrt::Windows::UI::Xaml::Media;

        const auto fluentFont = WUXMedia::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" };

        WUXC::MenuFlyout flyout{};

        // "Rename tab" — reuse M2's renamer (the double-tap path). Weak-capture the
        // header so a late click on a torn-down tab is a safe no-op.
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

        // DEFERRED — "Change tab color…" lands here in M4 (reuse ColorPickupFlyout,
        // re-route ColorSelected/ColorCleared → setTabColor / reset). Left as a
        // clear spot per the M5 brief.

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
            closeItem.Text(L"Close tab");
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

        // Workspaces M1.2 (#54, ADR-001): tint the tab to track the LIVE terminal
        // background color (classic WT: the selected tab background == the terminal
        // background, so the selected tab merges into its content). vm.Background()
        // is a FRESH non-acrylic SolidColorBrush projected from the mounted
        // IPaneContent's BackgroundBrush() (seeded at ContentMounted, refreshed on
        // TitleChanged); nullptr until seeded → fall back to the native theme.
        //
        // This is the LIVE CONTENT color (runtime, like the live Title), NOT a
        // user-chosen override. A future slice (M4) adds the user-chosen tab color
        // as a MODEL override (TabRecord.runtimeColor / setTabColor) which will
        // take precedence here (like customTitle wins over liveTitle); when M4
        // lands it layers on top of this — read the model override first, fall back
        // to this live color. Only the live color is wired now.
        if (const auto bgBrush = vm.Background().try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>())
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
}
