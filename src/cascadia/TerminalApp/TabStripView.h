// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// TabStripView — the per-leaf pane tab strip. Workspaces M1 (#54, ADR-001)
// rewrote it from a re-skinned WUX ListView into the REAL MUX TabView
// (Microsoft.UI.Xaml.Controls.TabView — the same control the classic per-window
// tab row uses), driven by WorkspaceModel as the single source of truth via
// MANUAL TabView.TabItems() projection (NOT TabItemsSource data-binding).
//
// The page sets ItemsSource to the leaf's PaneTabViewModel collection. The
// control subscribes to that vector's VectorChanged and, on the UI thread,
// projects each VM into a TabViewItem appended to TabView.TabItems() (mirroring
// classic WT's manual-TabItems shape: each TabViewItem.Content is a throwaway
// empty Border — the drag-identity bodge — and the real terminal is hosted
// separately in the page's leafContentHost, never inside the strip).
//
// Selection is a ONE-WAY projection of the model:
//   * The active tab is pushed into TabView.SelectedItem from each VM's
//     IsActive (we subscribe to every VM's PropertyChanged), so the
//     ActiveTabChanged diff arm drives the control.
//   * TabView.SelectionChanged is a USER INTENT only → it raises the selected
//     VM's RequestActivate (→ selectTab model action). The control never writes
//     model state. A reentrancy guard (_selectionPushDepth, a depth counter)
//     suppresses the SelectionChanged that the programmatic SelectedItem push
//     re-fires, so the push never loops back into an intent (this is
//     crash-avoidance — a re-entrant mutation inside a TabView callback is a
//     0xc000027b corner).
//   * TabCloseRequested → the VM's RequestClose (→ closeTab model action).
//
// Drag (Workspaces #55 — CUSTOM in-process pointer gesture): tab reorder is a
// hand-rolled pointer gesture, NOT MUX's built-in tab drag. All three MUX drag
// flags are OFF (CanReorderTabs / CanDragTabs / AllowDropTabs = false in XAML).
// MUX's built-in drag arms a Windows SHELL drag (CoreDragOperation) that returns
// E_ACCESSDENIED in this hosting context and escalates to an (uncatchable)
// 0xc000027b failfast on ANY tab drag (confirmed live 2026-05-26 via a symbolic
// dump: DataExchange.dll dragoperation.cpp:148 → ProcessUnhandledError →
// RoFailFast) — the shell tab drag simply does not work here (even production WT
// can't drag tabs in this environment). So reorder is reimplemented WITHOUT
// CoreDragOperation: pointer handlers on the TabView detect a press-then-threshold
// gesture, then a transparent overlay Canvas (above the TabView) CAPTURES the
// pointer (the overlay, NOT the TabViewItem — MUX's TabView internally
// CapturePointer()s its items un-refcounted and would steal/lose our capture; see
// TabManagement.cpp:1081) and hosts a 2px insertion-line adorner + a translucent
// drag-ghost in a Popup that follows the pointer (#56): a true RenderTargetBitmap
// snapshot of the LIVE dragged TabViewItem. RTB works in this XAML-island hosting as
// long as the source element is connected to the visible tree — the dragged tab always
// is, so the snapshot succeeds (an earlier attempt that captured a not-yet-connected
// strip threw a CATCHABLE E_INVALIDARG "content not connected", which looked like a
// "blank"; it is NOT a failfast and human-drag smoke confirms no crash). The MUX TabView
// only delegates its OWN drag image to the framework's shell drag (the CoreDragOperation
// that failfasts here), so an in-process RTB snapshot is how we get a pixel-exact ghost.
// On release the
// gesture raises the SAME MoveTabRequested(tabId, dstIdx) intent the page
// dispatches as moveTab; the model diff re-projects the strip. The gesture NEVER
// optimistically mutates TabItems — pure model-as-truth (no accept-then-reconcile).
// CROSS-leaf move + tear-out are #57/#60 (custom too; the monarch keeps tear-out
// reachable). Native TabViewItem chrome / icon / tooltip replace the old re-skin.

#pragma once

#include "TabStripView.g.h"

// Workspaces #55 (custom drag): _reorderEscRevoker below is a CoreWindow KeyDown
// revoker member, so the Core type must be complete in this header.
#include <winrt/Windows.UI.Core.h>

#include <optional>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    struct TabStripView : TabStripViewT<TabStripView>
    {
        TabStripView();

        // The leaf's PaneTabViewModel collection. The setter rebuilds the
        // TabView.TabItems() projection and (re-)subscribes the VectorChanged +
        // per-VM PropertyChanged handlers. The getter returns the stored source.
        winrt::Windows::Foundation::IInspectable ItemsSource();
        void ItemsSource(const winrt::Windows::Foundation::IInspectable& value);

        // Test-only structural accessor (see the .idl): the inner MUX TabView so
        // headless TAEF can assert TabItems / SelectedItem / drag flags.
        winrt::Microsoft::UI::Xaml::Controls::TabView TabViewControl();

        // Workspaces #55 (custom drag): one tab's strip-relative horizontal box
        // (left edge + width), in projection order. The reorder hit-test consumes a
        // vector of these so its index logic is a PURE function of geometry —
        // headlessly unit-testable (we never lay out real pixels: the headless
        // std::clamp-resize trap). The production gesture gathers the live laid-out
        // extents (_currentTabLayout) and feeds the same core.
        struct TabExtent
        {
            double left;
            double width;
        };

        // Workspaces #57: one leaf strip's geometry for the cross-leaf drop hit-test —
        // the strip's TabView bounds in WINDOW coordinates (so points from different
        // strips compare in a common space) plus its tab boxes in STRIP-LOCAL X
        // (the same space _dropGapFromGeometry expects: localX = windowX - left).
        struct StripExtent
        {
            uint64_t leafId;
            double left;
            double top;
            double width;
            double height;
            std::vector<TabExtent> tabs;
        };

        // Test-only (impl-only — NOT in the .idl, no vtable change, no codegen;
        // reached via winrt::get_self / the impl type directly, like the WorkspaceTests
        // strip tests). Drive the two PURE pieces of the within-leaf reorder hit-test
        // with synthetic geometry: _dropGapFromGeometry (pointer X + tab boxes →
        // visual gap) and _dstIndexFromGap (gap + dragged index → moveTab dstIdx, or
        // nullopt for a no-op). Mirrors the retired 3c ResolveCrossLeafDropForTest seam.
        static uint32_t DropGapFromGeometryForTest(double pointerX, const std::vector<TabExtent>& tabs)
        {
            return _dropGapFromGeometry(pointerX, tabs);
        }
        static std::optional<uint32_t> DstIndexFromGapForTest(uint32_t gap, uint32_t srcIdx)
        {
            return _dstIndexFromGap(gap, srcIdx);
        }

        // Workspaces #57: test forwarder for the PURE cross-leaf drop resolver
        // (impl-only, reached via the impl type directly like the forwarders above).
        static std::optional<std::pair<uint64_t, uint32_t>> ResolveCrossLeafDropForTest(double px, double py, const std::vector<StripExtent>& strips)
        {
            return _resolveCrossLeafDrop(px, py, strips);
        }

        // Workspaces #57: this strip's current geometry (bounds in `relativeTo`'s
        // coordinate space + tab boxes) for the page's cross-leaf drop hit-test. The
        // page passes the shared pane-tree ancestor (_workspacePaneTreeRoot) so every
        // sibling strip's rect lives in ONE concrete space (NOT the unreliable nullptr
        // window root). Live (uses TransformToVisual + _currentTabLayout) so NOT
        // headless-testable; the PURE resolution it feeds is _resolveCrossLeafDrop,
        // which IS unit-tested. Public so the page can gather every sibling strip's
        // extent.
        StripExtent CurrentStripExtent(winrt::Windows::UI::Xaml::UIElement const& relativeTo);

        // Workspaces #57: map a point in THIS strip's TabView-local space into
        // `ancestor`'s space. The page calls this on the SOURCE strip to lift the
        // source-local release point into the shared _workspacePaneTreeRoot ancestor
        // space, so it compares against the sibling strips' CurrentStripExtent rects
        // in ONE concrete coordinate space (NOT the unreliable nullptr window root).
        // Public so the page can call it.
        winrt::Windows::Foundation::Point LocalPointToAncestor(winrt::Windows::UI::Xaml::UIElement const& ancestor, double x, double y);

        // Workspaces #57: PURE cross-leaf drop resolver — given a release point in
        // WINDOW coords and every leaf strip's geometry, return {dstLeafId, dstIdx} of
        // the strip under the point (dstIdx = the visual gap from _dropGapFromGeometry;
        // cross-leaf inserts into a DIFFERENT collection so there is NO erase-then-
        // insert adjustment — unlike within-leaf _dstIndexFromGap). nullopt if the
        // point is over no strip. First containing strip wins (strips never overlap).
        // Public static so the page can call it directly (also exposed via the
        // ForTest forwarder above).
        static std::optional<std::pair<uint64_t, uint32_t>> _resolveCrossLeafDrop(double px, double py, const std::vector<StripExtent>& strips);

        // Workspaces M6a (#54, ADR-001): the strip-level move INTENT (see the .idl).
        // A within-leaf reorder translates the captured drag from→to into this; the
        // page subscribes it and dispatches moveTab(state, tabId, LeafId(), dstIdx).
        // Declared BEFORE the WINRT_PROPERTY below: that macro closes with a
        // `protected:` section, so a member placed after it would be inaccessible to
        // the generated projection glue (the event raiser must stay public).
        til::event<winrt::TerminalApp::MoveTabRequestedEventArgs> MoveTabRequested;

        // Workspaces #57 (ADR-001): the cross-leaf move INTENT (see the .idl). On
        // release OUTSIDE this strip's own bounds the gesture raises this with the
        // dragged VM's Id, THIS strip's LeafId, and the release point in this strip's
        // OWN TabView-LOCAL coords; the page lifts that point into the shared
        // _workspacePaneTreeRoot ancestor space, hit-tests every leaf strip there, and
        // dispatches moveTab (or no-ops over no strip). Declared alongside
        // MoveTabRequested and BEFORE the WINRT_PROPERTY below (the raiser must stay
        // public — that macro closes with a `protected:` section).
        til::event<winrt::TerminalApp::MoveTabToPointRequestedEventArgs> MoveTabToPointRequested;

        // Workspaces #57 slice 2 (ADR-001): per-move cross-leaf hover INTENT. The
        // SOURCE strip raises DragHoverRequested on EVERY pointer move during a drag
        // (carrying its LeafId + the pointer in its OWN TabView-local coords); the page
        // hit-tests every strip and shows the insertion indicator on the strip under the
        // pointer. DragHoverEnded fires from the single _endReorderDrag funnel so the
        // page clears the target indicator on drop/cancel/capture-loss. Declared
        // alongside the other move intents and BEFORE the WINRT_PROPERTY below (the
        // raiser must stay public — that macro closes with a `protected:` section).
        til::event<winrt::TerminalApp::DragHoverRequestedEventArgs> DragHoverRequested;
        til::event<winrt::TerminalApp::DragHoverEndedEventArgs> DragHoverEnded;

        // Workspaces #57 slice 2: the page calls these on a NON-dragging strip to
        // preview a cross-leaf drop. Show/position THIS strip's InsertionIndicator at
        // `gap` (an insertion index in [0, tabCount]) WITHOUT capturing the pointer (the
        // source strip owns capture) — the DragOverlay is made Visible but stays
        // IsHitTestVisible=false so the target never steals capture. HideExternalInsertionIndicator
        // clears it. No-ops on the strip that is itself the active dragger (its own line
        // is managed locally by _updateReorderAdorner). Public so the page can drive it.
        void ShowExternalInsertionIndicator(uint32_t gap);
        void HideExternalInsertionIndicator();

        // Workspaces M6 Stage 0 (#54, ADR-001): the model PaneId (LeafId.v) of the
        // leaf this strip projects, set by the page in _projectLeafContainer. The
        // within-leaf reorder gesture (#55) raises MoveTabRequested with THIS strip's
        // own LeafId as the moveTab destination (the page reads it at dispatch time).
        // The cross-leaf move (#57) resolves its destination by hit-testing the
        // release point instead, so it does NOT use this LeafId. A plain scalar; not
        // observed.
        WINRT_PROPERTY(uint64_t, LeafId, 0);

    private:
        // The source VM collection the page set, and the live subscription to its
        // membership changes. Each membership change re-projects the TabItems.
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel> _source{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::TerminalApp::PaneTabViewModel>::VectorChanged_revoker _sourceChangedRevoker{};

        // Per-VM PropertyChanged subscriptions, parallel to _source's order, so a
        // VM's IsActive flip pushes TabView.SelectedItem (the model→control
        // selection projection). Rebuilt whenever the projection is rebuilt.
        std::vector<winrt::Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker> _vmPropertyChangedRevokers;

        // Reentrancy guard, a DEPTH COUNTER (not a bare bool). Non-zero while we
        // programmatically push SelectedItem from the model; suppresses the
        // SelectionChanged the control re-fires so the model push never loops back
        // into a RequestActivate intent. A counter (++ on entry / -- on scope_exit)
        // composes regardless of nesting order: _rebuildProjection guards the whole
        // rebuild AND calls _syncSelectionFromModel, which guards again — with a
        // bool the inner scope_exit would clear the outer guard early; the counter
        // keeps it raised until the OUTERMOST scope unwinds.
        int _selectionPushDepth{ 0 };

        // Workspaces #55 (custom drag): within-leaf reorder pointer-gesture state.
        // _reorderCandidate is set on a left-button press over a tab (threshold not
        // yet exceeded — a plain click stays a click); _reorderActive is set once the
        // pointer moves past the drag threshold and the overlay captures the pointer.
        // _reorderStart is the press point in DragOverlay coordinates; _reorderVm is
        // the dragged tab's VM (weak — a mid-gesture rebuild can swap the item, so we
        // re-resolve its live index at release by VM identity); _reorderLastGap caches
        // the last computed insertion gap so the adorner repositions only when it
        // changes (PointerMoved fires continuously). _reorderEscRevoker holds an
        // Escape-cancel CoreWindow.KeyDown subscription for the drag's duration.
        bool _reorderCandidate{ false };
        bool _reorderActive{ false };
        winrt::Windows::Foundation::Point _reorderStart{};
        winrt::weak_ref<winrt::TerminalApp::PaneTabViewModel> _reorderVm{ nullptr };
        // The dragged TabViewItem itself (weak), captured at press so the #56 ghost can
        // snapshot it at drag-start. Weak: a mid-gesture rebuild can replace the item.
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Controls::TabViewItem> _reorderItem{ nullptr };
        uint32_t _reorderLastGap{ UINT32_MAX };
        winrt::Windows::UI::Core::CoreWindow::KeyDown_revoker _reorderEscRevoker{};

        // Tear down + rebuild the whole TabView.TabItems() projection from
        // _source (append every VM as a TabViewItem; re-subscribe each VM's
        // PropertyChanged), then sync SelectedItem to the active VM. UI thread.
        void _rebuildProjection();

        // Build one TabViewItem for `vm` (header / icon / tooltip / Tag = vm /
        // empty-Border content). UI thread.
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem _makeTabViewItem(const winrt::TerminalApp::PaneTabViewModel& vm);

        // Refresh `item`'s native chrome (header text + icon + tooltip + color)
        // from `vm`. Used at build time and on the VM's Title/PropertyChanged.
        void _applyChrome(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Workspaces M5 (#54, ADR-001): build the per-tab right-click context menu
        // (a MenuFlyout set as the TabViewItem.ContextFlyout) whose items raise
        // TabId-scoped VM intents (NOT classic's _dispatch.DoAction reconcile). Owned
        // by the item, released on _rebuildProjection (weak captures, no use-after-free
        // on a torn-down tab). The `header` is captured so the Rename item reuses M2's
        // BeginRename() path. UI thread.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _makeContextFlyout(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm, const winrt::TerminalApp::TabHeaderControl& header);

        // Refresh the Pin/Unpin context-menu item's label from the VM's projected
        // Pinned state (the toggle text comes BACK from the model via the
        // TabDecorationUpdated diff arm). Used at build time and on the VM's Pinned
        // PropertyChanged. UI thread.
        void _applyContextMenuPinLabel(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Workspaces M4 (#54, ADR-001): open the reused ColorPickupFlyout anchored
        // to `item` (mirrors classic Tab::AttachColorPicker). The flyout's
        // ColorSelected → vm.RequestSetColor / ColorCleared → vm.RequestClearColor
        // (TabId-scoped model intents — NEVER a click-site write-back). The picker
        // is held in _tabColorPickup for its open lifetime and self-released on
        // Closed (so its handlers can't outlive a torn-down strip); the VM is
        // weak-captured so a commit after a re-projection is a safe no-op. UI thread.
        void _showColorPicker(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const winrt::TerminalApp::PaneTabViewModel& vm);

        // Workspaces M1.2 (#54, ADR-001): apply / clear the classic selected-tab
        // color treatment (ported from Tab::_ApplyTabColorOnUIThread /
        // _ClearTabBackgroundColor) — local per-TabViewItem theme-dictionary
        // resource overrides tracking the live terminal background color. UI thread.
        void _applyTabColor(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item, const til::color& color);
        void _clearTabColor(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item);

        // Push the active VM (the one with IsActive) into TabView.SelectedItem,
        // guarded so the re-fired SelectionChanged is not treated as an intent.
        void _syncSelectionFromModel();

        // The TabView's SelectionChanged handler: a user intent only. Resolves
        // the selected TabViewItem's Tag VM and raises its RequestActivate
        // (unless we are mid programmatic push). Never writes model state.
        void _onSelectionChanged(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);

        // TabCloseRequested → the VM's RequestClose intent (→ closeTab).
        void _onTabCloseRequested(const winrt::Microsoft::UI::Xaml::Controls::TabView& sender, const winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs& args);

        // Workspaces #55 (custom drag): the within-leaf reorder pointer gesture.
        // These two fire on the TabView (registered via AddHandler with
        // handledEventsToo, so the TabViewItem's own pointer handling can't hide
        // them): _onStripPointerPressed records a candidate drag (origin + dragged VM,
        // resolved by walking the pressed element up to its TabViewItem) WITHOUT
        // capturing — a plain click still selects; _onStripPointerMoved promotes the
        // candidate to an active drag once the pointer moves past the threshold
        // (_beginReorderDrag). From there the gesture is driven by the OVERLAY (it
        // owns the captured pointer): _onOverlayPointerMoved repositions the insertion
        // adorner; _onOverlayPointerReleased commits (_finishReorderDrag → the
        // MoveTabRequested intent); _onOverlayPointerCaptureLost / a CoreWindow Escape
        // abort. All terminal paths funnel through _endReorderDrag (release capture,
        // hide the adorner, drop the Escape hook, reset state) — idempotent so a
        // release-then-capture-lost (or Escape-then-capture-lost) double-fire is safe.
        void _onStripPointerPressed(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _onStripPointerMoved(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _onOverlayPointerMoved(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _onOverlayPointerReleased(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _onOverlayPointerCaptureLost(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        // Promote the candidate to an active drag: show + capture the overlay, hook
        // Escape, draw the first adorner. _updateReorderAdorner repositions the
        // insertion line at the current gap (only when the gap changes).
        // _finishReorderDrag resolves the dragged VM's live index + the drop gap,
        // maps them to a moveTab dstIdx (skipping a no-op), and raises
        // MoveTabRequested. _endReorderDrag is the shared teardown.
        void _beginReorderDrag(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _updateReorderAdorner(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _finishReorderDrag(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _endReorderDrag();

        // #56 drag ghost: snapshot the live dragged TabViewItem to a RenderTargetBitmap
        // (a true pixel image — RTB works here once the element is connected to the
        // visible tree, which the dragged item always is) and show it in the ghost Popup,
        // scaled to the tab's DIP size. Async (RenderAsync); the ghost fills in within a
        // frame or two of the drag starting. Failure is a non-crashing no-op (empty ghost).
        winrt::fire_and_forget _renderDragGhost(winrt::Microsoft::UI::Xaml::Controls::TabViewItem item);

        // Gather the live laid-out box of every projected tab, in projection order,
        // expressed in DragOverlay coordinates (so the X's line up with Canvas.Left
        // for the adorner and with the captured pointer's overlay-relative X). Also
        // reports the tab row's top + height for the insertion line's extent. Needs
        // layout (runtime only — empty headlessly); the PURE index math below is what
        // the unit tests drive.
        std::vector<TabExtent> _currentTabLayout(double& outTop, double& outHeight);

        // The PURE within-leaf reorder hit-test, split for headless unit testing.
        // _dropGapFromGeometry: pointer X (overlay-relative) + each tab's box →
        // the visual gap in [0, N] (the count of tabs whose horizontal midpoint is
        // at/left of the pointer; i.e. the slot the dragged tab would land in with
        // every tab still present). _dstIndexFromGap: convert that gap + the dragged
        // tab's current index to the moveTab destination index — which the model
        // applies AFTER removing the dragged tab (Actions_Move.cpp erase-then-insert),
        // so a gap right of the source loses one slot; returns nullopt when the order
        // would not change (gap == src or src+1), letting the caller skip a no-op.
        static uint32_t _dropGapFromGeometry(double pointerX, const std::vector<TabExtent>& tabs);
        static std::optional<uint32_t> _dstIndexFromGap(uint32_t gap, uint32_t srcIdx);

        // Resolve the VM carried in a TabViewItem's Tag (nullptr if none).
        static winrt::TerminalApp::PaneTabViewModel _vmFromItem(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& item);

        // Workspaces M4 (#54, ADR-001): the currently-open per-tab color picker (the
        // reused ColorPickupFlyout). Held for its open lifetime so its
        // ColorSelected/ColorCleared/Closed handlers stay alive while shown; released
        // back to nullptr on Closed (mirrors classic Tab::_tabColorPickup). Only one
        // is ever open at a time. Its handler tokens revoke on Closed.
        winrt::TerminalApp::ColorPickupFlyout _tabColorPickup{ nullptr };
        winrt::event_token _colorSelectedToken{};
        winrt::event_token _colorClearedToken{};
        winrt::event_token _colorPickerClosedToken{};
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(TabStripView);
}
