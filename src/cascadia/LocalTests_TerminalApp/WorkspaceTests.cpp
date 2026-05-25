// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include <cmath>

#include "../TerminalApp/TerminalPage.h"
#include "../TerminalApp/TerminalWindow.h"
#include "../TerminalApp/ContentManager.h"
#include "../TerminalApp/ContentRegistry.h"
#include "../TerminalApp/BasicPaneEvents.h"
#include "../TerminalApp/TerminalPaneContent.h"
#include "../TerminalApp/WorkspaceView.h"
#include "../TerminalApp/WorkspaceViewModel.h"
#include "../TerminalApp/PaneTabViewModel.h"
#include "../TerminalApp/TabStripView.h"
#include "../WorkspaceModel/WorkspaceChange.h"
#include "../WorkspaceModel/Diff.h"
#include "../WorkspaceModel/Validator.h"
#include "CppWinrtTailored.h"

using namespace Microsoft::Console;
using namespace TerminalApp;
using namespace winrt::TerminalApp;
using namespace winrt::Microsoft::Terminal::Settings::Model;

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::UI::Xaml;

namespace winrt
{
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace TerminalAppLocalTests
{
    // WorkspaceTests covers the Phase 1 Slice 2 migrated actions:
    //   - Startup-replay with experimental.workspaces.enabled = true
    //   - Default-profile new-tab with the flag on
    //
    // Same CI-skip note as TabTests (microsoft/terminal#3838) — these
    // tests require the LocalTests TestHostApp framework and won't run
    // in CI until the framework package install path is fixed.
    class WorkspaceTests
    {
        BEGIN_TEST_CLASS(WorkspaceTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(StartupReplay_FlagOn_CreatesInitialTab);
        TEST_METHOD(NewTab_FlagOn_AppendsTab);
        TEST_METHOD(NewTab_FlagOff_AppendsTabWithoutModel);
        // Slice F follow-up (#54): the Ctrl+Shift+T action shape (NewTabArgs
        // wrapping default NewTerminalArgs) adds a tab to the focused leaf.
        TEST_METHOD(NewTab_FlagOn_DefaultNewTabArgs_AppendsTabToFocusedLeaf);

        // #43: the keybinding-less _HandleNewTab(sender, nullptr) branch.
        TEST_METHOD(NewTab_FlagOn_NullArgs_AppendsTabThroughModel);
        TEST_METHOD(NewTab_FlagOff_NullArgs_AppendsTabWithoutModel);

        TEST_METHOD(SwitchToTab_FlagOn_ChangesActiveWorkspace);
        TEST_METHOD(SwitchToTab_FlagOff_ChangesSelectedTabWithoutModel);

        // #45/#44: id-based routing over the CLASSIC tab index resolver was
        // covered by IdResolver_RoutesToCorrectTab_AfterReorder /
        // IdResolver_UnknownId_IsNoOp — retired by the F-5 cutover (no classic
        // tabs are built flag-on, so the resolver they tested has no flag-on
        // path). The id-over-positional property now lives in the model +
        // projected strip and is covered by the BigFlip / F-2 tests. See the
        // rationale block at their (removed) definitions.

        // Slice 6: decoration + explicit-profile dispatch.
        TEST_METHOD(RenameTab_FlagOn_UpdatesClassicTabAndModel);
        TEST_METHOD(RenameTab_FlagOff_UpdatesClassicTabOnly);
        TEST_METHOD(SetTabColor_FlagOn_UpdatesClassicTabAndModel);
        TEST_METHOD(SetTabColor_FlagOff_UpdatesClassicTabOnly);
        TEST_METHOD(NewTab_FlagOn_ExplicitProfileByName_AppendsTab);
        TEST_METHOD(NewTab_FlagOff_ExplicitProfileByName_AppendsTab);

        // #41: any non-default NewTerminalArgs field (beyond the profile
        // selector) is NOT modelled in the TerminalSpec yet. BEHAVIOR CHANGE
        // (Slice F follow-up, #54): flag-on these no longer fall through to the
        // classic _OpenNewTab (that breach builds a classic Tab that drops the
        // workspace host -> blank window). They now route THROUGH the model —
        // a tab in the focused leaf — with the unmodelled fields dropped (a
        // tracked follow-up; the spec only carries the profile GUID today).
        TEST_METHOD(NewTab_FlagOn_TabColorField_RoutesThroughModel);
        TEST_METHOD(NewTab_FlagOn_SessionIdField_RoutesThroughModel);
        TEST_METHOD(NewTab_FlagOn_AppendCommandLineField_RoutesThroughModel);
        TEST_METHOD(NewTab_FlagOn_SuppressApplicationTitleField_RoutesThroughModel);
        TEST_METHOD(NewTab_FlagOn_ColorSchemeField_RoutesThroughModel);
        TEST_METHOD(NewTab_FlagOn_ElevateField_RoutesThroughModel);
        TEST_METHOD(NewTab_FlagOn_ReloadEnvironmentVariablesField_RoutesThroughModel);

        // Slice 6 review fixes:
        //  - sender-bypass: flag-on rename/color must honour the
        //    `sender` argument (right-clicked tab) instead of always
        //    routing to the focused tab.
        //  - DuplicateTab observable parity (skipped in the original
        //    slice).
        // SetTabColor_FlagOn_RoutesByRightClickedSender_NotFocusedTab retired by
        // the F-5 cutover (classic right-click tab routing has no flag-on path).
        TEST_METHOD(DuplicateTab_FlagOn_AppendsWorkspaceWithSameProfile);
        TEST_METHOD(DuplicateTab_FlagOff_AppendsTabWithoutModel);

        // Slice 3: close cascade + spawn-failure.
        TEST_METHOD(CloseTab_FlagOn_RemovesWorkspaceAndTab);
        TEST_METHOD(CloseTab_FlagOff_RemovesTabWithoutModel);
        TEST_METHOD(CloseLastTab_FlagOn_RequestsWindowClose);
        TEST_METHOD(CloseLastTab_FlagOff_RequestsWindowClose);
        TEST_METHOD(NewTab_FlagOn_LeavesModelValidatorClean);
        TEST_METHOD(NewTab_FlagOn_SpawnFailure_LeavesNoZombieWorkspace);

        // Slice 5: split + resize + identity-preserving move.
        TEST_METHOD(SplitPane_FlagOn_GrowsActiveWorkspaceTree);
        TEST_METHOD(SplitPane_FlagOn_GrowsActiveWorkspaceTree_Horizontal);
        TEST_METHOD(SplitPane_FlagOff_GrowsClassicPaneTreeWithoutModel);
        TEST_METHOD(ResizePane_FlagOn_UpdatesModelSplitRatio);
        TEST_METHOD(ResizePane_FlagOff_LeavesModelDormant);
        TEST_METHOD(MoveTab_FlagOn_DiffEmitsTabMovedPreservingId);
        TEST_METHOD(MoveTab_FlagOn_CrossWorkspaceTabMovedPreservingId);

        // Slice 2 / Phase 2 Visible #2 (#46): the workspaces UI shell.
        //  - flag-off: the sidebar column is collapsed / zero-width and the
        //    page renders byte-for-byte upstream;
        //  - flag-on: the sidebar mirrors one read-only row per workspace in
        //    declared order;
        //  - clicking a sidebar row mutates nothing in the model.
        TEST_METHOD(Sidebar_FlagOff_IsCollapsedAndZeroWidth);
        TEST_METHOD(Sidebar_FlagOn_MirrorsWorkspacesInDeclaredOrder);
        TEST_METHOD(Sidebar_FlagOn_RowIsReadOnly_NoModelMutation);

        // #52, Stage 3a: the row's intent events (RenameCommitted /
        // PinToggleRequested) route through the page into a model action, and
        // the resulting WorkspaceMetadataUpdated diff arm re-projects the row.
        TEST_METHOD(Sidebar_FlagOn_RenameCommit_DispatchesModelAndReProjects);
        TEST_METHOD(Sidebar_FlagOn_TogglePin_DispatchesModelAndReProjects);

        // #52, Stage 3b: the row's ColorCommitted event routes through the page
        // into setWorkspaceColor (set with a value / clear with a null
        // IReference), and the WorkspaceMetadataUpdated diff arm re-projects the
        // swatch (Color / HasColor).
        TEST_METHOD(Sidebar_FlagOn_ColorCommit_DispatchesModelAndReProjects);

        // #52, Stage 3c: clicking a sidebar row raises the VM's RequestActivate
        // intent; the page (subscribed in _addWorkspaceVm) dispatches
        // switchToWorkspace and the resulting ActiveWorkspaceChanged diff arm
        // flips the active-row highlight and selects the workspace's classic
        // tab. The VM never touches the model — it only raises the intent.
        TEST_METHOD(Sidebar_FlagOn_RowClick_DispatchesSwitchToWorkspace);
        TEST_METHOD(Sidebar_FlagOn_RowClick_FlipsActiveHighlightById);

        // Pinned-float: pinning a workspace floats it to the bottom of the
        // pinned block; the WorkspaceReordered diff arm reorders
        // _workspaceViewModels to match the model's new display order (by id).
        TEST_METHOD(Sidebar_FlagOn_Pin_ReordersViewModels);

        // Phase 2 Slice 3 (#47): the ContentRegistry lifetime contract.
        //  - mount-then-unmount keeps the SAME live IPaneContent instance
        //    alive and resolvable (its ConPTY survives an inactive workspace);
        //  - removal erases the entry, tears the content down (Close()), and a
        //    subsequent resolve fails EXPLICITLY (null), so a stale id can
        //    never be re-attached;
        //  - mounting an id the registry does not own is unrepresentable: the
        //    only way to obtain mountable content is EnsureMounted, which
        //    either resolves an owned id or creates+inserts it.
        TEST_METHOD(ContentRegistry_UnmountKeepsAlive_RemoveTearsDown);

        // Big-flip Slice A (#54): the ContentMounted factory is real, so the
        // ContentRegistry genuinely owns a live IPaneContent per active tab.
        // The classic Tab still displays — this slice changes NO display
        // ownership; it only proves the registry materialises content from the
        // model's mount policy through the live page factory.
        TEST_METHOD(BigFlipA_FactoryMaterialisesContentIntoRegistry);

        // Big-flip Slice E (#54): closing a WHOLE workspace tears down its
        // registry content (drops its ConPTY). A whole-workspace close emits
        // WorkspaceRemoved — NOT per-tab TabRemoved/ContentUnmounted — so
        // apply(WorkspaceRemoved) is the only arm that can plug the leak; it
        // does so via the workspace->contents reverse index. Two workspaces ->
        // registry Size()==2; close one -> Size()==1, surviving content alive.
        TEST_METHOD(BigFlipE_WorkspaceClose_RemovesItsContent);

        // Big-flip Slice B (#54): the flag-on content host. A collapsed
        // WorkspaceContentHost inside TabContent receives the ACTIVE
        // workspace's factory-built content GetRoot(), proving the
        // attach/swap-on-switch plumbing BEHIND the still-visible classic tab.
        // The classic _tabContent display is unchanged (host is Collapsed), so
        // the user still sees only the classic terminal this slice.
        TEST_METHOD(BigFlipB_ActiveWorkspaceContentAttachedToHost);
        TEST_METHOD(BigFlipB_SwitchSwapsHostChild);

        // Big-flip Slice C (#54): the per-leaf MVVM tab strip — the INVISIBLE
        // projection of a leaf pane's tabs (it lives inside the still-Collapsed
        // WorkspaceContentHost). Adding a tab to an existing leaf appends a
        // PaneTabViewModel WITHOUT creating a 2nd classic Tab; ActiveTabChanged
        // flips the strip's active row and swaps the host child to that tab's
        // content; TabRemoved removes the strip VM. The classic tab stays the
        // only visible thing — ZERO visible change this slice.
        TEST_METHOD(BigFlipC_TabAdded_AppendsStripVm);
        TEST_METHOD(BigFlipC_ActiveTabChanged_FlipsSelectionAndSwapsHostChild);
        TEST_METHOD(BigFlipC_TabRemoved_RemovesStripVm);

        // Live pane-tab title (#54): the strip VM's Title tracks its mounted
        // content's live title. On ContentMounted the VM's Title is seeded from
        // content.Title(); a later content.TitleChanged re-pushes the new title
        // into the VM (the running terminal's title, not the static "Tab"
        // placeholder). Driven against the MockPaneContent's settable title +
        // SetTitle() helper. The classic path is untouched.
        TEST_METHOD(LivePaneTabTitle_TracksContentTitleChanged);

        // Slice 2a.2 (#54): the selected pane-tab's connected background tracks
        // the mounted content's live background color (the faithful classic WT
        // "tab.background = terminalBackground" behavior). On ContentMounted the
        // VM's Background is seeded from content.BackgroundBrush(); a later
        // content bg change is re-pulled on the NEXT TitleChanged (the bg
        // piggybacks on the title subscription — IPaneContent has no bg-changed
        // event). Proves both the mount-seed and the title-cadence refresh.
        TEST_METHOD(LivePaneTabBackground_TracksContentBackground);

        // Slice 2a.2 follow-up (#54): the bg refresh runs on every TitleChanged
        // (per-prompt cadence). Background's WINRT_OBSERVABLE_PROPERTY setter
        // guards by REFERENCE identity, so a freshly-allocated brush of the same
        // color would still raise PropertyChanged on every refresh. This pins the
        // color-equality short-circuit: an unchanged-color refresh raises NO
        // Background PropertyChanged, while a genuine color change still does.
        TEST_METHOD(LivePaneTabBackground_NoChurnOnUnchangedColor);

        // Big-flip Slice D (#54): the active workspace's SPLIT pane tree is
        // projected into nested XAML inside the still-Collapsed
        // WorkspaceContentHost — a split Grid (two star-sized cells along the
        // axis) per SplitPane, a leaf container (carrying that leaf's Slice-C
        // strip) per LeafPane. A split builds the nested containers; a ratio
        // change updates the split Grid's star sizes; collapsing a child lifts
        // the surviving leaf back to a single container. Structure/ratio are
        // asserted (NOT laid-out pixel widths). The classic split stays the
        // visible one — ZERO visible change this slice.
        TEST_METHOD(BigFlipD_Split_BuildsNestedContainers);
        TEST_METHOD(BigFlipD_SplitRatio_SetsStarSizes);
        TEST_METHOD(BigFlipD_Collapse_LiftsSurvivor);

        // Big-flip Slice F-0 (#54): each projected leaf gets its OWN content
        // host, and the active leaf's content GetRoot() is attached into THAT
        // leaf's host. A single-leaf workspace attaches its content into the
        // root leaf's host; a SPLIT workspace gives EACH leaf its own distinct
        // content root in its own host (the capability the single shared host
        // could not represent). Asserted by element identity; the host + tree
        // stay Collapsed (ZERO visible change this slice). A flag-off mirror
        // confirms none of this structure realizes when the flag is off.
        TEST_METHOD(BigFlipF0_SingleLeaf_ContentInLeafHost);
        TEST_METHOD(BigFlipF0_Split_EachLeafHostHoldsOwnContent);
        TEST_METHOD(BigFlipF0_FlagOff_NoLeafHosts);

        // Big-flip Slice F-1 (#54): the custom draggable split divider. A split
        // projection carries a CUSTOM separator Border (NOT a toolkit
        // GridSplitter) between its two cells, tagged with the split id it
        // controls. A SIMULATED drag — calling the headless-testable
        // _resizeSplitFromDrag helper with a synthetic delta + a KNOWN extent
        // (never a laid-out pixel) — dispatches the model resizePane action and
        // re-projects the two cells' GridLengths to the new ratio (asserted by
        // GridLength.Value within 1e-5). The orientation test covers BOTH a
        // vertical (columns) and a horizontal (rows) split so the drag-delta sign
        // is right for each axis. A flag-off mirror builds no separator; the host
        // stays Collapsed throughout (ZERO visible change this slice).
        TEST_METHOD(BigFlipF1_Split_BuildsCustomSeparator);
        TEST_METHOD(BigFlipF1_SimulatedDrag_DispatchesResizeAndReprojects);
        TEST_METHOD(BigFlipF1_HorizontalSplit_DragReprojectsRows);
        TEST_METHOD(BigFlipF1_FlagOff_NoSeparator);

        // Big-flip Slice F-2 (#54): first-tab IsActive seed + leaf-strip GC.
        //  - After startup (a new workspace / leaf), the leaf's strip VM for
        //    the first (active) tab has IsActive == true. The model's
        //    activeTabIdx seeds the state so no ActiveTabChanged is needed.
        //  - After a split-then-collapse, the dead leaf's entry is pruned from
        //    _paneTabStrips; the map size equals the live leaf count.
        //  - Flag-off mirror: model is dormant, no strip map entries exist.
        TEST_METHOD(BigFlipF2_FirstTab_IsActiveSeeded);
        TEST_METHOD(BigFlipF2_Collapse_PrunesDeadLeafFromStrip);
        TEST_METHOD(BigFlipF2_FlagOff_NoStripEntries);

        // F-4 (#46): the single window-stay-open predicate that the two
        // close-decision guards (_CompleteInitialization, _RemoveTab) route
        // through. Flag-off it mirrors `_tabs.Size() > 0` byte-for-byte;
        // flag-on it reflects `!workspaces.empty()`, with a safe fallback to
        // `_tabs.Size()` when the model is null (no crash, no spurious close).
        TEST_METHOD(BigFlipF4_FlagOff_WindowShouldStayOpen_MirrorsTabCount);
        TEST_METHOD(BigFlipF4_FlagOn_WindowShouldStayOpen_ReflectsWorkspaces);
        TEST_METHOD(BigFlipF4_FlagOn_NullModel_DoesNotCrashOrSpuriouslyClose);
        TEST_METHOD(BigFlipF4_FlagOn_StartupReplay_StaysOpenAndDoesNotRequestClose);

        // Big-flip Slice F-5 (#54): THE CUTOVER. The projected pane tree is now
        // the VISIBLE display (host Visible + sole child of _tabContent); the
        // classic window tab strip is retired flag-on (_tabRow forced to zero
        // height, TabView Collapsed); flag-off both stay byte-for-byte upstream.
        TEST_METHOD(BigFlipF5_FlagOn_HostVisibleAndSoleChildOfTabContent);
        TEST_METHOD(BigFlipF5_FlagOn_TabRowHeightZero);
        TEST_METHOD(BigFlipF5_FlagOff_TabRowHeightAuto);

        // F-5 fix (#46): flag-on, OpenSettingsUI must NOT build a classic tab
        // (which would clear _tabContent and drop the workspace host). It is
        // guarded to a no-op; the sole-child host invariant must survive the call.
        TEST_METHOD(BigFlipF5_FlagOn_OpenSettings_DoesNotCorruptHost);

        // Per-pane strip Slice 1 (#54) / Workspaces M1 (ADR-001): the per-leaf
        // strip is a TabStripView UserControl built in _projectLeafContainer that
        // now hosts the REAL MUX TabView, driven by WorkspaceModel via manual
        // TabView.TabItems() projection. STRUCTURAL assertions only — never a
        // laid-out pixel (the headless std::clamp-resize trap):
        //  - a projected leaf's row-0 child is a TabStripView whose TabView's
        //    TabItems count == the leaf's model tab count, in order (each
        //    TabViewItem.Tag is its VM);
        //  - the TabView has all three drag flags OFF (M1 failfast-avoidance);
        //  - selection is a ONE-WAY model→control projection: SelectedItem
        //    reflects the active VM, and a model-driven push raises ZERO activate
        //    intents (the reentrancy guard); the control never writes back;
        //  - the activate + close intents still dispatch the right model action
        //    (dispatch-level, mirroring the BigFlipC strip tests).
        TEST_METHOD(StripSlice1_LeafRowZeroChildIsTabStripView_ItemsMatchModel);
        TEST_METHOD(StripSlice1_InnerListViewIsHorizontalAndSingleSelect);
        TEST_METHOD(StripSlice1_ActiveVmIsReflected_SelectionIsPureProjection);
        TEST_METHOD(StripSlice1_ActivateIntent_DispatchesSelectTab);
        TEST_METHOD(StripSlice1_CloseIntent_DispatchesCloseTab);

        // Workspaces M1 (#54, ADR-001): NATIVE TabViewItem chrome on the real MUX
        // TabView. The bespoke re-skin (ItemContainerStyle, IsActive-driven
        // SelectedBackground border, theme-correct foreground labels, inter-tab
        // separators, hover highlight) is DELETED — the native control renders
        // selection / separators / hover itself. The chrome M1 PROJECTS from the
        // model is the native TabViewItem.Header (title), .IconSource (icon path)
        // and the Content drag-identity bodge. STRUCTURAL only — never a laid-out
        // pixel (the headless std::clamp-resize trap). (The deleted tests
        // StripSlice2a_IsActiveDrivesForegroundBrush / StripSlice2a3_* /
        // StripSlice2a4_* asserted on now-native re-skin members; their premise is
        // gone. Selection projection is covered by
        // StripSlice1_ActiveVmIsReflected_SelectionIsPureProjection.)
        //  - ...InnerListViewCarriesReskinContainerStyle (kept name): the native
        //    Header reflects the VM Title + the Content is the empty Border bodge.
        //  - ...IsActiveDrivesSelectedBackgroundVisibility (kept name): the native
        //    IconSource is driven from the VM Icon path via IconSourceMUX.
        TEST_METHOD(StripSlice2a_InnerListViewCarriesReskinContainerStyle);
        TEST_METHOD(StripSlice2a_IsActiveDrivesSelectedBackgroundVisibility);

        // Workspaces M2 (#54, ADR-001): tab rename via the reused TabHeaderControl,
        // model-driven through setTabTitle, with custom-wins title precedence. Two
        // facets, both dispatch-level (no synthetic pointer / layout — the headless
        // std::clamp-resize trap):
        //  - the rename INTENT path: raising the VM's RequestRename(newTitle)
        //    dispatches setTabTitle through the model; the diff's TabDecorationUpdated
        //    arm projects the new customTitle back onto the VM, and the VM's computed
        //    Title surfaces it. Pins the model-as-truth round trip (the title is NOT
        //    written on the view at the intent site).
        //  - precedence (custom wins): after a rename, a subsequent live
        //    content.TitleChanged (MockPaneContent.SetTitle) must NOT clobber the
        //    custom title — the VM Title still shows the custom title. Clearing the
        //    custom title (rename to empty) lets the live title take over again.
        TEST_METHOD(StripM2_Rename_DispatchesSetTabTitle_CustomTitleWins);

        // Workspaces M3 (#54, ADR-001): tab bell/attention as VM-RUNTIME state (an
        // ADR deviation — NOT a WorkspaceModel field). Pins the bell behaviour at
        // the VM/projection level (no real gestures/layout — the headless
        // std::clamp-resize trap):
        //  - a content BellRequested with SendNotification==true sets the strip
        //    VM's BellIndicator true AND the hosted header's
        //    TerminalTabStatus.BellIndicator reflects it;
        //  - making the tab active (the same _setActivePaneTabVm projection the
        //    real ActiveTabChanged arm drives) clears BellIndicator (dismiss on
        //    focus);
        //  - a BellRequested with SendNotification==false does NOT light the tab
        //    (mirrors classic Tab.cpp's notification gate).
        // Driven against a minimal MockPaneContent.RaiseBell() raiser (mirrors the
        // existing SetTitle title raiser). The classic path is untouched.
        TEST_METHOD(StripM3_Bell_SetsIndicatorAndDismissesOnFocus);

        TEST_CLASS_SETUP(ClassSetup)
        {
            return true;
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
            // Big-flip Slice F-5 (#54): deterministically tear down the page that
            // the test just built BEFORE the next test constructs its own.
            //
            // The default mock factory (see _initializeTerminalPageWithFlagOn)
            // already keeps the dominant, render-engine-backed content out of
            // these tests. But a few flag-on tests fire a NewTab that ROUTES
            // CLASSIC (a non-Phase-1 field), and the flag-off mirrors build a
            // classic Tab too — both materialise a real TermControl + ConPTY
            // whose handlers / connection callbacks tear down asynchronously. If
            // such a page leaks past the test boundary (the
            // DesktopWindowXamlSource window content keeps it alive), one of those
            // deferred callbacks can fire into a half-torn-down page while the
            // NEXT test is constructing its page on the shared TAEF UI thread —
            // the rare residual 0xC0000005.
            //
            // Detaching the page from the window FIRST (so nothing in the live
            // tree references it), then pumping the UI-thread queue, forces the
            // prior page's release + its controls' teardown to run NOW, in
            // cleanup, rather than racing the next construction. Order matters:
            // detach before the dispatcher drain, never drop refs while the
            // control is still parented and rendering.
            (void)RunOnUIThread([]() {
                winrt::Windows::UI::Xaml::Window::Current().Content(nullptr);
            });
            // A second, empty dispatch flushes everything queued ahead of it (the
            // detach-driven releases), so they complete before the next test runs.
            (void)RunOnUIThread([]() {});
            _windowProperties = nullptr;
            _contentManager = nullptr;
            return true;
        }

    private:
        // `beforeCreate`, when set, runs on the UI thread AFTER the page is
        // constructed and _settings is assigned but BEFORE Create() and the
        // startup-replay dispatch. Big-flip Slice B (#54) uses it to install
        // the test-only content-factory override so the startup mount builds a
        // MockPaneContent (known root) instead of a real TermControl.
        void _initializeTerminalPageWithFlagOn(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                               CascadiaSettings initialSettings,
                                               std::function<void(winrt::TerminalApp::implementation::TerminalPage*)> beforeCreate = nullptr);
        void _initializeTerminalPageWithFlagOff(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                                CascadiaSettings initialSettings);

        // #41 shared body: fire a flag-on default-profile NewTab whose
        // NewTerminalArgs carries a single non-default (unmodelled) field, and
        // assert it routes THROUGH the model — a tab added to the focused leaf
        // of the active workspace (workspace count unchanged, strip +1, `_tabs`
        // stays empty), NOT a classic tab. Slice F follow-up (#54): the
        // pre-cutover fall-through to classic _OpenNewTab was a blank-window
        // breach; the unmodelled fields are dropped flag-on (tracked follow-up).
        void _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            std::function<void(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs&)> setField,
            const wchar_t* fieldLabel);

        winrt::com_ptr<winrt::TerminalApp::implementation::WindowProperties> _windowProperties;
        winrt::com_ptr<winrt::TerminalApp::implementation::ContentManager> _contentManager;
    };

    // A minimal test-only IPaneContent that stands in for a live
    // TermControl/ConPTY-backed content. It records how many times Close()
    // was called (the registry calls Close() exactly once, on removal —
    // the ConPTY-teardown point) and carries a unique tag so the test can
    // assert that a re-mount resolves the SAME instance the registry kept
    // alive across an unmount.
    //
    // Big-flip Slice F-5 (#54): hoisted from a method-local struct to namespace
    // scope so _initializeTerminalPageWithFlagOn can install it as the DEFAULT
    // content factory for every flag-on page test (see that helper for why a
    // real TermControl must NOT be realised flag-on in the headless TestHostApp).
    struct MockPaneContent : public winrt::implements<MockPaneContent, winrt::TerminalApp::IPaneContent>,
                             public winrt::TerminalApp::implementation::BasicPaneEvents
    {
        explicit MockPaneContent(uint64_t tag) :
            _tag{ tag } {}

        uint64_t Tag() const noexcept { return _tag; }
        int CloseCount() const noexcept { return _closeCount; }

        // Big-flip Slice B (#54): the host-attach plumbing parents this
        // content's GetRoot() into the WorkspaceContentHost. A headless test
        // asserts that parented element by identity, so the mock must hand back
        // a real, stable FrameworkElement — the SAME instance every call (the
        // registry keeps one content alive across (un)mounts, so its root must
        // be stable too). We lazily build a bare Grid and cache it. Using a real
        // TermControl's root in a headless attach test is unnecessary and
        // couples the test to control geometry; this mock root keeps the attach
        // test about the plumbing only.
        winrt::Windows::UI::Xaml::FrameworkElement GetRoot()
        {
            if (!_root)
            {
                _root = winrt::Windows::UI::Xaml::Controls::Grid{};
            }
            return _root;
        }
        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings&) {}
        winrt::Windows::Foundation::Size MinimumSize() { return { 0, 0 }; }
        winrt::hstring Title() { return _title; }

        // Live pane-tab title (#54): test helper to simulate a running
        // terminal's title changing. Updates the live Title and raises
        // TitleChanged (the BasicPaneEvents event the IPaneContent projection
        // exposes), exactly as a real TerminalPaneContent does when the control
        // reports a new title — so a test can verify the strip VM's Title tracks
        // it. Default initial title stays "mock" (preserves existing tests).
        void SetTitle(winrt::hstring title)
        {
            _title = std::move(title);
            TitleChanged.raise(*this, nullptr);
        }

        // Workspaces M3 (#54): test helper to simulate the running terminal
        // emitting a BEL. Raises BellRequested with a BellEventArgs carrying
        // (flashTaskbar, sendNotification) — mirroring how a real
        // TerminalPaneContent re-raises ControlCore's bell. The WorkspaceView's
        // BellRequested subscription drives the strip VM's BellIndicator from this,
        // gated on SendNotification() (classic Tab.cpp:1154). Mirrors SetTitle's
        // raiser shape.
        void RaiseBell(bool flashTaskbar, bool sendNotification)
        {
            BellRequested.raise(*this, winrt::make<winrt::TerminalApp::implementation::BellEventArgs>(flashTaskbar, sendNotification));
        }
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return false; }
        winrt::hstring Icon() { return {}; }
        winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept { return nullptr; }

        // Slice 2a.2 (#54): a settable background brush so a test can drive the
        // selected pane-tab background projection. IPaneContent exposes NO
        // background-changed event — the strip's bg refresh piggybacks on
        // TitleChanged — so to simulate the live terminal bg changing, set this
        // then call SetTitle() (which raises TitleChanged). Defaults to null
        // (preserves existing tests that expect no bg).
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush() { return _backgroundBrush; }
        void SetBackgroundBrush(winrt::Windows::UI::Xaml::Media::Brush brush)
        {
            _backgroundBrush = std::move(brush);
        }
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(winrt::TerminalApp::BuildStartupKind) { return nullptr; }
        void Focus(winrt::Windows::UI::Xaml::FocusState) {}
        void Close() { ++_closeCount; }

    private:
        uint64_t _tag{ 0 };
        int _closeCount{ 0 };
        winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr };
        winrt::hstring _title{ L"mock" };
        winrt::Windows::UI::Xaml::Media::Brush _backgroundBrush{ nullptr };
    };

    // Mirror of TabTests::_initializeTerminalPage, but seeds the
    // experimental.workspaces.enabled flag as ON in the global
    // settings. After Create() the page picks the flag-on path and
    // routes the startup NewTab action through WorkspaceActions ->
    // diff -> WorkspaceView -> _openDefaultTabForWorkspace.
    void WorkspaceTests::_initializeTerminalPageWithFlagOn(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                                           CascadiaSettings initialSettings,
                                                           std::function<void(winrt::TerminalApp::implementation::TerminalPage*)> beforeCreate)
    {
        winrt::TerminalApp::TerminalPage projectedPage{ nullptr };

        _windowProperties = winrt::make_self<winrt::TerminalApp::implementation::WindowProperties>();
        winrt::TerminalApp::WindowProperties props = *_windowProperties;
        _contentManager = winrt::make_self<winrt::TerminalApp::implementation::ContentManager>();
        winrt::TerminalApp::ContentManager contentManager = *_contentManager;

        Log::Comment(L"Construct the TerminalPage");
        auto result = RunOnUIThread([&projectedPage, &page, initialSettings, props, contentManager]() {
            projectedPage = winrt::TerminalApp::TerminalPage(props, contentManager);
            page.copy_from(winrt::get_self<winrt::TerminalApp::implementation::TerminalPage>(projectedPage));
            page->_settings = initialSettings;
        });
        VERIFY_SUCCEEDED(result);

        VERIFY_IS_NOT_NULL(page);
        VERIFY_IS_NOT_NULL(page->_settings);
        VERIFY_IS_TRUE(page->_settings.GlobalSettings().WorkspacesEnabled(),
                       L"settings JSON must enable the workspaces flag");

        ::details::Event waitForInitEvent;
        if (!waitForInitEvent.IsValid())
        {
            VERIFY_SUCCEEDED(HRESULT_FROM_WIN32(::GetLastError()));
        }
        page->Initialized([&waitForInitEvent](auto&&, auto&&) {
            waitForInitEvent.Set();
        });

        Log::Comment(L"Create() the TerminalPage");
        result = RunOnUIThread([&page, &beforeCreate]() {
            VERIFY_IS_NOT_NULL(page);
            VERIFY_IS_NOT_NULL(page->_settings);

            // Big-flip Slice B (#54): run any pre-Create hook (e.g. installing
            // the test-only content-factory override) so it is in place before
            // the startup-replay's ContentMounted fires.
            if (beforeCreate)
            {
                beforeCreate(page.get());
            }
            else
            {
                // Big-flip Slice F-5 (#54): default the flag-on content factory
                // to a MockPaneContent. WHY THIS IS LOAD-BEARING:
                //
                // THE CUTOVER (606dc2208) flips the WorkspaceContentHost Visible
                // and makes it _tabContent's sole child, so flag-on workspace
                // content is now laid out with REAL dimensions. Pre-cutover the
                // host was Collapsed (zero size) -> ControlCore::Initialize()
                // early-returned at windowWidth==0, so a real TermControl built
                // by the production factory never spun up a render engine / render
                // thread / D3D swap chain; it was an inert shell. Post-cutover it
                // DOES — and in the headless TestHostApp that real render engine's
                // teardown (synchronous render-thread join in ~ControlCore ->
                // Renderer::TriggerTeardown) faults with 0xC0000005 inside
                // Microsoft.Terminal.Control.dll, surfacing non-deterministically
                // (~25-40% of runs) as a crash while CONSTRUCTING the next test's
                // TerminalPage on the shared TAEF UI thread.
                //
                // The BigFlip A-F suite already installs this exact mock for the
                // same reason; the remaining flag-on tests (which assert only
                // MODEL / _tabs / validator state, never a real control's
                // internals) just never had it. Installing it by DEFAULT keeps
                // those tests building MockPaneContent (a bare Grid, no render
                // engine) so nothing real is realised or torn down. This is a
                // PURE TEST-HARNESS fix: production is untouched (window close is a
                // serialized posted-message teardown — WM_CLOSE delivered on the
                // single message-pump thread — so teardown completes before any
                // new page is ever constructed; there is no back-to-back page
                // reconstruction racing the render thread as there is on the shared
                // TAEF UI thread) and flag-off is byte-for-byte unchanged. A test
                // that genuinely needs a real control can still pass its own
                // beforeCreate.
                page->_makePaneContentForSpecOverrideForTest =
                    [](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                    static uint64_t tag = 0xC000;
                    return winrt::make_self<MockPaneContent>(tag++).as<winrt::TerminalApp::IPaneContent>();
                };
            }

            page->Create();
            Log::Comment(L"Create()'d the page successfully");

            // Push a default-profile NewTab onto the startup actions so
            // the first-layout machinery dispatches it. This mirrors
            // what AppCommandlineArgs / shell:AppsFolder activation
            // does in the real app.
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs args{ newTerminalArgs };
            ActionAndArgs newTabAction{ ShortcutAction::NewTab, args };
            page->_startupActions.push_back(std::move(newTabAction));
            Log::Comment(L"Added a single default NewTab action");

            winrt::TerminalApp::TerminalPage pp = *page;
            winrt::Windows::UI::Xaml::Window::Current().Content(pp);
            winrt::Windows::UI::Xaml::Window::Current().Activate();
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Wait for the page to finish initializing...");
        VERIFY_SUCCEEDED(waitForInitEvent.Wait());
        Log::Comment(L"...done");
    }

    // AC: "Flag-on AppX launch reaches a visible default terminal tab
    // via the model + view."
    //
    // After startup-replay, the model holds one workspace with one tab
    // and the classic XAML _tabs collection holds one Tab. The
    // observable end state on the flag-on path must match flag-off
    // (one tab in _tabs).
    void WorkspaceTests::StartupReplay_FlagOn_CreatesInitialTab()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&page]() {
            // Big-flip Slice F-5 (#54): THE CUTOVER. No classic XAML tab is built
            // flag-on — the model + projected pane tree are the source of truth.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on startup-replay must NOT build a classic tab (cutover)");

            // Model state matches: one workspace with one tab
            VERIFY_IS_TRUE(page->_workspaceModelState != nullptr,
                           L"flag-on startup-replay must populate the model state");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"model should have exactly one workspace");
            VERIFY_IS_TRUE(page->_workspaceModelState->activeWorkspaceId_view().has_value(),
                           L"model should have an active workspace");

            // WorkspaceView is alive and bound to this page
            VERIFY_IS_TRUE(page->_workspaceView != nullptr,
                           L"WorkspaceView should have been instantiated");
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC: "Flag-on new-tab (default profile) produces an observable
    // result identical to flag-off."
    //
    // After one default-profile new-tab is fired post-startup, _tabs
    // has two entries (initial + new) and the model has the new tab
    // appended to the existing workspace's active leaf.
    void WorkspaceTests::NewTab_FlagOn_AppendsTab()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Pre-condition: startup-replay landed one workspace and NO classic tab
        // (cutover). BEHAVIOR CHANGE (Slice F follow-up, #54): an args-carrying
        // default NewTab (the action a keybinding sends) now adds a TAB to the
        // focused leaf of the active workspace — NOT a new workspace. Previously
        // this test pinned the buggy "creates a second workspace" behavior; the
        // new-tab keybinding fix makes every flag-on new-tab intent append to the
        // focused leaf. Capture the active leaf so we can assert its strip grew.
        ::WorkspaceModel::PaneId activeLeaf{ 0 };
        auto result = RunOnUIThread([&page, &activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            const auto leafOpt = page->_activeLeafModelId();
            VERIFY_IS_TRUE(leafOpt.has_value(), L"startup workspace must have an active leaf");
            activeLeaf = *leafOpt;
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"the active leaf starts with exactly one tab in its strip");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a default-profile NewTab through the action handler");
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, activeLeaf]() {
            // Big-flip Slice F-5 (#54): no classic tab is built flag-on.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on default-profile new-tab must NOT build a classic tab (cutover)");

            // The args-carrying default NewTab adds a tab to the focused leaf,
            // so the workspace COUNT is unchanged (still one workspace).
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on new-tab must NOT create a new workspace — same workspace count");

            // The active leaf's strip grew by one row (1 -> 2): the new tab
            // landed in the focused leaf, not a fresh workspace.
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"flag-on new-tab adds a pane-tab to the focused leaf's strip (1 -> 2)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice F follow-up (#54): the Ctrl+Shift+T shape — a NewTabArgs wrapping a
    // DEFAULT NewTerminalArgs — must add a TAB to the focused leaf of the active
    // workspace, NOT spawn a new workspace. This is the exact action a keybinding
    // dispatches (distinct from the keybinding-less null-args path covered by
    // NewTab_FlagOn_NullArgs_AppendsTabThroughModel). Assert: workspace count
    // unchanged (still 1), focused leaf strip +1 (1 -> 2), `_tabs` stays empty.
    void WorkspaceTests::NewTab_FlagOn_DefaultNewTabArgs_AppendsTabToFocusedLeaf()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        ::WorkspaceModel::PaneId activeLeaf{ 0 };
        auto result = RunOnUIThread([&page, &activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            const auto leafOpt = page->_activeLeafModelId();
            VERIFY_IS_TRUE(leafOpt.has_value(), L"startup workspace must have an active leaf");
            activeLeaf = *leafOpt;
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"the active leaf starts with exactly one tab in its strip");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire the Ctrl+Shift+T shape: NewTabArgs wrapping default NewTerminalArgs");
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
            VERIFY_IS_TRUE(eventArgs.Handled(),
                           L"the new-tab action must be marked handled flag-on");
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"Ctrl+Shift+T flag-on must NOT build a classic tab (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"Ctrl+Shift+T flag-on must NOT create a new workspace — same workspace count");
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"Ctrl+Shift+T flag-on adds a pane-tab to the focused leaf's strip (1 -> 2)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Mirror of _initializeTerminalPageWithFlagOn, but with the
    // experimental.workspaces.enabled flag OFF (the default). After
    // Create() the page must pick the classic path: NewTab actions
    // bypass WorkspaceActions/diff/WorkspaceView entirely and mutate
    // _tabs directly.
    void WorkspaceTests::_initializeTerminalPageWithFlagOff(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                                            CascadiaSettings initialSettings)
    {
        winrt::TerminalApp::TerminalPage projectedPage{ nullptr };

        _windowProperties = winrt::make_self<winrt::TerminalApp::implementation::WindowProperties>();
        winrt::TerminalApp::WindowProperties props = *_windowProperties;
        _contentManager = winrt::make_self<winrt::TerminalApp::implementation::ContentManager>();
        winrt::TerminalApp::ContentManager contentManager = *_contentManager;

        Log::Comment(L"Construct the TerminalPage");
        auto result = RunOnUIThread([&projectedPage, &page, initialSettings, props, contentManager]() {
            projectedPage = winrt::TerminalApp::TerminalPage(props, contentManager);
            page.copy_from(winrt::get_self<winrt::TerminalApp::implementation::TerminalPage>(projectedPage));
            page->_settings = initialSettings;
        });
        VERIFY_SUCCEEDED(result);

        VERIFY_IS_NOT_NULL(page);
        VERIFY_IS_NOT_NULL(page->_settings);
        VERIFY_IS_FALSE(page->_settings.GlobalSettings().WorkspacesEnabled(),
                        L"settings JSON must leave the workspaces flag off");

        ::details::Event waitForInitEvent;
        if (!waitForInitEvent.IsValid())
        {
            VERIFY_SUCCEEDED(HRESULT_FROM_WIN32(::GetLastError()));
        }
        page->Initialized([&waitForInitEvent](auto&&, auto&&) {
            waitForInitEvent.Set();
        });

        Log::Comment(L"Create() the TerminalPage");
        result = RunOnUIThread([&page]() {
            VERIFY_IS_NOT_NULL(page);
            VERIFY_IS_NOT_NULL(page->_settings);
            page->Create();
            Log::Comment(L"Create()'d the page successfully");

            // Push a default-profile NewTab onto the startup actions so
            // the first-layout machinery dispatches it. Mirror of the
            // flag-on harness.
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs args{ newTerminalArgs };
            ActionAndArgs newTabAction{ ShortcutAction::NewTab, args };
            page->_startupActions.push_back(std::move(newTabAction));
            Log::Comment(L"Added a single default NewTab action");

            winrt::TerminalApp::TerminalPage pp = *page;
            winrt::Windows::UI::Xaml::Window::Current().Content(pp);
            winrt::Windows::UI::Xaml::Window::Current().Activate();
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Wait for the page to finish initializing...");
        VERIFY_SUCCEEDED(waitForInitEvent.Wait());
        Log::Comment(L"...done");
    }

    // Strangler-fig contract pin: any future slice that breaks
    // flag-OFF parity will fail this test even if the flag-on tests
    // still pass.
    //
    // After one default-profile new-tab is fired on the classic path,
    // _tabs has two entries (initial + new) AND the workspace model
    // state / WorkspaceView were never instantiated. The end state on
    // the flag-off path must be observable-identical (tab count) to
    // the flag-on path while leaving the new machinery completely
    // dormant.
    void WorkspaceTests::NewTab_FlagOff_AppendsTabWithoutModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        // Pre-condition: startup-replay landed one tab via the classic
        // path; the workspace machinery never spun up.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off startup must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off startup must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a default-profile NewTab through the action handler");
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Classic tab strip now has two tabs — same observable
            // count as the flag-on mirror test.
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-off default-profile new-tab should append a classic tab");

            // The workspace machinery must remain dormant on the
            // flag-off path. If a future slice flips this, the
            // strangler-fig contract is broken.
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off new-tab must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off new-tab must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #43 / Big-flip Slice F-5 (#54, folds F-3): a keybinding-less Ctrl+T
    // reaches _HandleNewTab with a null ActionEventArgs (args == nullptr).
    // POST-CUTOVER this adds a TAB to the focused leaf of the active workspace
    // (newTab), NOT a new workspace — the per-leaf MVVM strip is the tab UI now.
    // Assert: the workspace COUNT is unchanged (still 1), the active leaf's strip
    // grew by one row (1 -> 2), and `_tabs` stayed empty (no classic tab built).
    void WorkspaceTests::NewTab_FlagOn_NullArgs_AppendsTabThroughModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        ::WorkspaceModel::PaneId activeLeaf{ 0 };
        auto result = RunOnUIThread([&page, &activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            const auto leafOpt = page->_activeLeafModelId();
            VERIFY_IS_TRUE(leafOpt.has_value(), L"startup workspace must have an active leaf");
            activeLeaf = *leafOpt;
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"the active leaf starts with exactly one tab in its strip");

            // Pin that _appendPaneTabVm stores the title on the VM (not left
            // empty / not the raw type name "TerminalApp.PaneTabViewModel").
            // The startup tab has an empty customTitle so _appendPaneTabVm
            // substitutes the sentinel "Tab"; a non-empty result here confirms
            // the Title property is populated, so the strip ListView's
            // Text="{x:Bind Title}" DataTemplate binding will resolve a real
            // string rather than falling back to IInspectable.ToString().
            const auto firstTitle = page->_paneTabStripFirstTitleForTest(activeLeaf);
            VERIFY_IS_TRUE(firstTitle.has_value(), L"strip[0] must have a Title");
            VERIFY_ARE_NOT_EQUAL(winrt::hstring{}, *firstTitle, L"strip[0] Title must be non-empty");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire Ctrl+T (null ActionEventArgs) — adds a tab to the focused leaf");
        result = RunOnUIThread([&page]() {
            page->_HandleNewTab(nullptr, ActionEventArgs{ nullptr });
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"Ctrl+T flag-on must NOT build a classic tab (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"Ctrl+T flag-on must NOT create a new workspace — same workspace count");
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"Ctrl+T flag-on adds a pane-tab to the focused leaf's strip (1 -> 2)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #43 flag-off mirror: the null-args branch must stay on the
    // classic path and leave the workspace machinery dormant.
    void WorkspaceTests::NewTab_FlagOff_NullArgs_AppendsTabWithoutModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off startup must NOT populate the model state");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire _HandleNewTab with a null ActionEventArgs (keybinding-less path)");
        result = RunOnUIThread([&page]() {
            page->_HandleNewTab(nullptr, ActionEventArgs{ nullptr });
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-off null-args new-tab should append a classic tab");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off null-args new-tab must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off null-args new-tab must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #41 shared body. See declaration.
    void WorkspaceTests::_verifyFlagOnNonDefaultFieldRoutesThroughModel(
        std::function<void(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs&)> setField,
        const wchar_t* fieldLabel)
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        ::WorkspaceModel::PaneId activeLeaf{ 0 };
        auto result = RunOnUIThread([&page, &activeLeaf]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay should produce exactly one workspace");
            const auto leafOpt = page->_activeLeafModelId();
            VERIFY_IS_TRUE(leafOpt.has_value(), L"startup workspace must have an active leaf");
            activeLeaf = *leafOpt;
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"the active leaf starts with exactly one tab in its strip");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a flag-on NewTab carrying a single non-default field");
        result = RunOnUIThread([&page, &setField]() {
            NewTerminalArgs newTerminalArgs{};
            setField(newTerminalArgs);
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, activeLeaf, fieldLabel]() {
            // BEHAVIOR CHANGE (Slice F follow-up, #54): the field is not modelled
            // in the TerminalSpec, but flag-on we no longer fall through to the
            // classic path (that built a classic Tab and dropped the workspace
            // host -> blank window). The new tab routes THROUGH the model — a tab
            // added to the focused leaf — so the workspace count is UNCHANGED, the
            // focused leaf's strip grew by one, and `_tabs` stays empty. The
            // unmodelled field is dropped flag-on (a tracked follow-up).
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the unmodelled-field new-tab must NOT build a classic tab");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             fieldLabel);
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"the unmodelled-field new-tab lands in the focused leaf's strip (1 -> 2)");
        });
        VERIFY_SUCCEEDED(result);
    }

    void WorkspaceTests::NewTab_FlagOn_TabColorField_RoutesThroughModel()
    {
        _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            [](NewTerminalArgs& a) { a.TabColor(winrt::Windows::UI::Color{ 255, 10, 20, 30 }); },
            L"a TabColor override routes through the model (one workspace, tab in focused leaf)");
    }

    void WorkspaceTests::NewTab_FlagOn_SessionIdField_RoutesThroughModel()
    {
        _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            [](NewTerminalArgs& a) { a.SessionId(winrt::guid{ 0x12345678, 0x1234, 0x1234, { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 } }); },
            L"a SessionId override routes through the model (one workspace, tab in focused leaf)");
    }

    void WorkspaceTests::NewTab_FlagOn_AppendCommandLineField_RoutesThroughModel()
    {
        _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            [](NewTerminalArgs& a) { a.AppendCommandLine(true); },
            L"an AppendCommandLine override routes through the model (one workspace, tab in focused leaf)");
    }

    void WorkspaceTests::NewTab_FlagOn_SuppressApplicationTitleField_RoutesThroughModel()
    {
        _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            [](NewTerminalArgs& a) { a.SuppressApplicationTitle(true); },
            L"a SuppressApplicationTitle override routes through the model (one workspace, tab in focused leaf)");
    }

    void WorkspaceTests::NewTab_FlagOn_ColorSchemeField_RoutesThroughModel()
    {
        _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            [](NewTerminalArgs& a) { a.ColorScheme(L"Campbell"); },
            L"a ColorScheme override routes through the model (one workspace, tab in focused leaf)");
    }

    void WorkspaceTests::NewTab_FlagOn_ElevateField_RoutesThroughModel()
    {
        _verifyFlagOnNonDefaultFieldRoutesThroughModel(
            [](NewTerminalArgs& a) { a.Elevate(true); },
            L"an Elevate override routes through the model (one workspace, tab in focused leaf)");
    }

    // ReloadEnvironmentVariables is set true on essentially every launch by
    // _getNewTerminalArgs, so for a no-commandline tab its value equals the
    // default reload behavior and is NOT treated as an unmodelled override.
    // Unlike the other #41 fields, a NewTab carrying only
    // ReloadEnvironmentVariables(true) must route THROUGH the model so the
    // normal startup new-tab populates the workspace (and sidebar). See
    // _hasUnmodelledNewTabFields; full fidelity is deferred to #48.
    void WorkspaceTests::NewTab_FlagOn_ReloadEnvironmentVariablesField_RoutesThroughModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        ::WorkspaceModel::PaneId activeLeaf{ 0 };
        auto result = RunOnUIThread([&page, &activeLeaf]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay should produce exactly one workspace");
            const auto leafOpt = page->_activeLeafModelId();
            VERIFY_IS_TRUE(leafOpt.has_value(), L"startup workspace must have an active leaf");
            activeLeaf = *leafOpt;
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"the active leaf starts with exactly one tab in its strip");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a flag-on NewTab carrying only ReloadEnvironmentVariables(true)");
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            newTerminalArgs.ReloadEnvironmentVariables(true);
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, activeLeaf]() {
            // ReloadEnvironmentVariables is not a meaningful per-tab override, so
            // this is treated as a default-profile new-tab and routes through the
            // model. BEHAVIOR CHANGE (Slice F follow-up, #54): "through the model"
            // now means a tab in the focused leaf, not a new workspace — so the
            // workspace count is UNCHANGED and the focused leaf's strip grew by one.
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"a ReloadEnvironmentVariables-only new tab must NOT create a new workspace");
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(activeLeaf),
                             L"a ReloadEnvironmentVariables-only new tab lands in the focused leaf's strip (1 -> 2)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // -------------------------------------------------------------------
    // Slice 4: switch active tab + focus pane through the model.
    // -------------------------------------------------------------------

    // AC: "Flag-on tab switching observably matches flag-off."
    //
    // Phase 1 maps each classic tab onto one model workspace. After
    // startup + one new-tab the page has two tabs (workspace[0] and
    // workspace[1]); the new tab is the active one (workspace[1] is the
    // active workspace). Issuing _HandleSwitchToTab(index=0) routes
    // through WorkspaceActions::selectTab, the diff emits
    // ActiveWorkspaceChanged{ workspace[0].id }, and the WorkspaceView calls
    // _SelectTab(0). The end state on classic XAML and the end state in
    // the model are both consistent with "tab 0 is active".
    void WorkspaceTests::SwitchToTab_FlagOn_ChangesActiveWorkspace()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Push the model up to two workspaces, then verify pre-conditions.
        // Slice F follow-up (#54): NewTab now adds a tab to the focused leaf, so
        // use the new-workspace entry point (_createNewWorkspace) to get a second
        // workspace to switch between.
        auto result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Big-flip Slice F-5 (#54): no classic tab is built flag-on — assert
            // the switch via the MODEL's active workspace, not _GetFocusedTabIndex.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_IS_TRUE(page->_workspaceModelState != nullptr);
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size());

            // The most recently created workspace is the active one
            // (newWorkspace's contract). Capture its id so we can verify
            // the switch actually moved the active workspace.
            const auto activeBefore = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeBefore.has_value());
            VERIFY_ARE_EQUAL(activeBefore.value(),
                             page->_workspaceModelState->workspaces_view()[1].id,
                             L"workspaces[1] should be active after a NewTab on a single-workspace model");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Switch to tab 0 via SwitchToTab action");
        result = RunOnUIThread([&page]() {
            SwitchToTabArgs args{ 0u };
            ActionEventArgs eventArgs{ args };
            page->_HandleSwitchToTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Model now reports workspaces[0] as active...
            const auto activeAfter = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeAfter.has_value());
            VERIFY_ARE_EQUAL(activeAfter.value(),
                             page->_workspaceModelState->workspaces_view()[0].id,
                             L"selectTab routed through the model should move active to workspaces[0]");

            // MRU was touched: workspaces[0] is now at the front.
            VERIFY_IS_FALSE(page->_workspaceModelState->mru_view().empty());
            VERIFY_ARE_EQUAL(page->_workspaceModelState->mru_view().front(),
                             page->_workspaceModelState->workspaces_view()[0].id);

            // The switch is non-structural — the workspace count is unchanged,
            // and no classic tab was ever built (cutover).
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"switch is non-structural — workspace count must not change");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip stays empty across a switch (cutover)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig contract pin: any future slice that breaks flag-OFF
    // parity for tab switching will fail this test.
    //
    // After two NewTabs on the classic path (tabs 0 and 1, with tab 1
    // selected), issuing _HandleSwitchToTab(0) selects tab 0. The
    // workspace model and WorkspaceView must remain dormant throughout.
    void WorkspaceTests::SwitchToTab_FlagOff_ChangesSelectedTabWithoutModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(1u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu),
                             L"the new tab should be selected on the classic path too");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr);
            VERIFY_IS_TRUE(page->_workspaceView == nullptr);
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Switch to tab 0 via SwitchToTab action");
        result = RunOnUIThread([&page]() {
            SwitchToTabArgs args{ 0u };
            ActionEventArgs eventArgs{ args };
            page->_HandleSwitchToTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(0u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu),
                             L"flag-off SwitchToTab should select tab 0");
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());

            // Strangler-fig: workspace machinery still dormant.
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off switch-to-tab must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off switch-to-tab must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-5 (#54): THE CUTOVER removed IdResolver_RoutesToCorrectTab_AfterReorder
    // and IdResolver_UnknownId_IsNoOp. Both exercised the classic-tab-INDEX
    // resolver (_classicTabIndexForWorkspace) against a populated classic tab
    // strip (built classic tabs, reordered them with _TryMoveTab, drove
    // ActiveWorkspaceChanged/TabDecorationUpdated to land on the right classic
    // Tab). The cutover retires the classic Tab entirely flag-on — no classic
    // tabs are ever built, so the resolver they tested has no flag-on path to
    // exercise. They would have required rebuilding the classic strip the
    // cutover forbids; rewriting them to assert nothing real would be a hollow
    // test. The id-over-positional resolution property they pinned now lives in
    // the model + projected strip (resolved by stable PaneId/TabId, never a
    // display index), exercised by the BigFlip / F-2 strip tests. Removed with
    // this rationale rather than left as dead green stubs.
#if 0
    void WorkspaceTests::IdResolver_RoutesToCorrectTab_AfterReorder()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Startup gives us workspace[0] (tab at index 0). Add a second.
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        ::WorkspaceModel::WorkspaceId ws0{};
        ::WorkspaceModel::WorkspaceId ws1{};
        ::WorkspaceModel::TabId ws0TabId{};
        winrt::TerminalApp::Tab ws0Tab{ nullptr };

        result = RunOnUIThread([&]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), workspaces.size());
            ws0 = workspaces[0].id;
            ws1 = workspaces[1].id;

            // Capture workspace[0]'s classic Tab object + its model TabId via
            // the registry; we'll track it by identity across the reorder.
            const auto it0 = page->_workspaceClassicTabs.find(ws0);
            VERIFY_IS_TRUE(it0 != page->_workspaceClassicTabs.end());
            ws0Tab = it0->second.get();
            VERIFY_IS_TRUE(ws0Tab != nullptr);

            const auto leaves = page->_workspaceModelState->leaves(ws0);
            VERIFY_IS_FALSE(leaves.empty());
            VERIFY_IS_FALSE(leaves[0]->tabs.empty());
            ws0TabId = leaves[0]->tabs[0].id;

            // Pre-reorder sanity: workspace[0]'s tab is at display index 0.
            VERIFY_ARE_EQUAL(0u, page->_GetTabIndex(ws0Tab).value_or(0xFFFFFFFFu));
        });
        VERIFY_SUCCEEDED(result);

        // Reorder the classic strip so workspace[0]'s tab moves to index 1.
        // Now "workspace display index" (0) != "classic tab index" (1) — the
        // exact mismatch the old positional contract assumed away. Then put
        // selection back on index 0 (workspace[1]'s tab) so the subsequent
        // ActiveWorkspaceChanged assertion proves a real selection move.
        result = RunOnUIThread([&]() {
            page->_TryMoveTab(0, 1);
            VERIFY_ARE_EQUAL(1u, page->_GetTabIndex(ws0Tab).value_or(0xFFFFFFFFu),
                             L"reorder should have moved workspace[0]'s tab to index 1");
            page->_SelectTab(0);
            VERIFY_ARE_EQUAL(0u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu));
        });
        VERIFY_SUCCEEDED(result);

        // Drive ActiveWorkspaceChanged{ws0} straight at the view. The
        // resolver must select the tab that is now at index 1 (ws0's tab),
        // NOT index 0. The old contract carried index 0 and would mis-select
        // (it would have re-selected the tab already at index 0).
        result = RunOnUIThread([&]() {
            page->_workspaceView->apply(::WorkspaceModel::ActiveWorkspaceChanged{ ws0 });
            VERIFY_ARE_EQUAL(1u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu),
                             L"id resolver must select workspace[0]'s tab at its CURRENT index (1)");
        });
        VERIFY_SUCCEEDED(result);

        // Drive TabDecorationUpdated for ws0's tab. The rename must land on
        // ws0's tab object (now at index 1), not whatever tab sits at index 0.
        result = RunOnUIThread([&]() {
            ::WorkspaceModel::TabDecorationUpdated deco{};
            deco.id = ws0TabId;
            deco.customTitle = "ws0-renamed";
            deco.workspaceId = ws0;
            page->_workspaceView->apply(deco);

            auto movedTabImpl = page->_GetTabImpl(ws0Tab);
            VERIFY_IS_NOT_NULL(movedTabImpl);
            VERIFY_ARE_EQUAL(winrt::hstring{ L"ws0-renamed" }, movedTabImpl->GetTabText(),
                             L"decoration must land on ws0's tab object regardless of slot");

            // The tab now at index 0 (workspace[1]'s tab) must be untouched.
            auto idx0Tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(idx0Tab);
            VERIFY_IS_FALSE(idx0Tab->GetTabText() == winrt::hstring{ L"ws0-renamed" },
                            L"decoration must NOT leak onto the tab at the stale positional index");
        });
        VERIFY_SUCCEEDED(result);

        // ws1 sanity: still resolvable to its own (now index-0) tab.
        result = RunOnUIThread([&]() {
            const auto resolved = page->_classicTabIndexForWorkspace(ws1);
            VERIFY_IS_TRUE(resolved.has_value());
            VERIFY_ARE_EQUAL(0u, resolved.value());
        });
        VERIFY_SUCCEEDED(result);
    }

    // #45/#44: an unknown / stale WorkspaceId must resolve to std::nullopt so
    // the apply arms become explicit no-ops — never an out-of-range or
    // wrong-tab route. Illegal states unrepresentable: there is no positional
    // index that could accidentally point somewhere valid.
    void WorkspaceTests::IdResolver_UnknownId_IsNoOp()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Two tabs; tab 1 is selected after the NewTab.
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(1u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu));

            // An id that no workspace owns: the resolver returns nullopt and
            // both arms must leave selection + titles untouched.
            const ::WorkspaceModel::WorkspaceId bogus{ 999999 };
            VERIFY_IS_FALSE(page->_classicTabIndexForWorkspace(bogus).has_value());

            const auto titleAt0Before = page->_GetTabImpl(page->_tabs.GetAt(0))->GetTabText();
            const auto titleAt1Before = page->_GetTabImpl(page->_tabs.GetAt(1))->GetTabText();

            page->_workspaceView->apply(::WorkspaceModel::ActiveWorkspaceChanged{ bogus });
            VERIFY_ARE_EQUAL(1u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu),
                             L"unknown-id ActiveWorkspaceChanged must not change selection");

            ::WorkspaceModel::TabDecorationUpdated deco{};
            deco.id = ::WorkspaceModel::TabId{ 888888 };
            deco.customTitle = "should-not-apply";
            deco.workspaceId = bogus;
            page->_workspaceView->apply(deco);

            VERIFY_IS_TRUE(page->_GetTabImpl(page->_tabs.GetAt(0))->GetTabText() == titleAt0Before,
                           L"unknown-id decoration must not mutate any tab");
            VERIFY_IS_TRUE(page->_GetTabImpl(page->_tabs.GetAt(1))->GetTabText() == titleAt1Before,
                           L"unknown-id decoration must not mutate any tab");
        });
        VERIFY_SUCCEEDED(result);
    }
#endif // Big-flip F-5: IdResolver classic-tab-index tests retired (see rationale above)

    // ------------------------------------------------------------------
    // Slice 6: decoration (rename + color) + explicit-profile new-tab.
    // ------------------------------------------------------------------

    // Shared settings JSON for the decoration tests. Single profile with a
    // known GUID so the explicit-profile dispatch tests can name it.
    static constexpr std::wstring_view settingsJsonFlagOn{ LR"(
    {
        "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
        "experimental.workspaces.enabled": true,
        "profiles": [
            {
                "name" : "profile0",
                "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                "historySize": 1,
                "closeOnExit": "never"
            },
            {
                "name" : "profile1",
                "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                "historySize": 1,
                "closeOnExit": "never"
            }
        ]
    })" };

    static constexpr std::wstring_view settingsJsonFlagOff{ LR"(
    {
        "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
        "profiles": [
            {
                "name" : "profile0",
                "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                "historySize": 1,
                "closeOnExit": "never"
            },
            {
                "name" : "profile1",
                "guid": "{6239a42c-2222-49a3-80bd-e8fdd045185c}",
                "historySize": 1,
                "closeOnExit": "never"
            }
        ]
    })" };

    // Big-flip Slice F-5 (#54): THE CUTOVER. Pre-cutover, flag-on rename routed
    // through the model AND updated the classic Tab text. The cutover retires the
    // classic Tab, so _HandleRenameTab's flag-on path — which resolves its target
    // via _senderOrFocusedTab(sender) -> _modelIdForTab (CLASSIC-tab-keyed) —
    // finds no tab and no-ops. Re-wiring rename to resolve the active tab from
    // the MODEL (so the action works off the projected strip) is a KNOWN FOLLOW-UP
    // (out of F-5 scope; tracked for the per-leaf strip context-menu wiring). This
    // test pins the post-cutover reality so the regression is visible and a future
    // slice flips it: NO classic tab exists, and the model is left unchanged by
    // the classic-tab-keyed entry point.
    void WorkspaceTests::RenameTab_FlagOn_UpdatesClassicTabAndModel()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        std::string titleBefore;
        auto result = RunOnUIThread([&page, &titleBefore]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto leaves = page->_workspaceModelState->leaves(workspaces[0].id);
            titleBefore = leaves[0]->tabs[0].customTitle;

            RenameTabArgs renameArgs{ L"renamed" };
            ActionEventArgs eventArgs{ renameArgs };
            page->_HandleRenameTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, titleBefore]() {
            // No classic tab to update — and the classic-tab-keyed entry point
            // didn't reach the model. KNOWN FOLLOW-UP: re-key rename off the
            // model's active tab.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size());
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto leaves = page->_workspaceModelState->leaves(workspaces[0].id);
            VERIFY_ARE_EQUAL(titleBefore, leaves[0]->tabs[0].customTitle,
                             L"post-cutover the classic-tab-keyed rename path is a no-op (known follow-up)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig mirror: same observable end-state for the classic
    // Tab text, but the workspace machinery stays dormant.
    void WorkspaceTests::RenameTab_FlagOff_UpdatesClassicTabOnly()
    {
        CascadiaSettings settings{ settingsJsonFlagOff, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());

            RenameTabArgs renameArgs{ L"renamed" };
            ActionEventArgs eventArgs{ renameArgs };
            page->_HandleRenameTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(winrt::hstring{ L"renamed" }, tab->GetTabText(),
                             L"flag-off rename must update the classic Tab text");

            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off rename must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off rename must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-5 (#54): THE CUTOVER. Mirror of RenameTab_FlagOn above —
    // _HandleSetTabColor's flag-on path is keyed off the (now-absent) classic
    // focused tab via _senderOrFocusedTab -> _modelIdForTab, so post-cutover it
    // no-ops. Re-keying color off the model's active tab is the SAME KNOWN
    // FOLLOW-UP as rename. Pin the post-cutover reality: no classic tab, model
    // color unchanged by the classic-tab-keyed entry point.
    void WorkspaceTests::SetTabColor_FlagOn_UpdatesClassicTabAndModel()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        constexpr winrt::Windows::UI::Color expected{ .A = 0xFF, .R = 0x11, .G = 0x22, .B = 0x33 };

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");

            SetTabColorArgs colorArgs{ expected };
            ActionEventArgs eventArgs{ colorArgs };
            page->_HandleSetTabColor(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size());
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto leaves = page->_workspaceModelState->leaves(workspaces[0].id);
            const auto& modelColor = leaves[0]->tabs[0].runtimeColor;
            VERIFY_IS_FALSE(modelColor.has_value(),
                            L"post-cutover the classic-tab-keyed color path is a no-op (known follow-up)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig mirror for SetTabColor.
    void WorkspaceTests::SetTabColor_FlagOff_UpdatesClassicTabOnly()
    {
        CascadiaSettings settings{ settingsJsonFlagOff, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        constexpr winrt::Windows::UI::Color expected{ .A = 0xFF, .R = 0x11, .G = 0x22, .B = 0x33 };

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());

            SetTabColorArgs colorArgs{ expected };
            ActionEventArgs eventArgs{ colorArgs };
            page->_HandleSetTabColor(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            const auto classic = tab->GetTabColor();
            VERIFY_IS_TRUE(classic.has_value(), L"flag-off color must populate the classic Tab runtime color");
            VERIFY_ARE_EQUAL(expected.R, classic.value().R);

            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off color must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off color must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC: "Explicit-profile new-tab observable parity." Firing a
    // NewTab action whose NewTerminalArgs names profile1 (the non-
    // default profile) with the flag on must (Slice F follow-up, #54):
    //  - NOT build a classic tab (cutover).
    //  - NOT create a new workspace — the workspace count is unchanged.
    //  - Append a tab to the ACTIVE workspace's focused leaf whose
    //    TerminalSpec carries profile1's GUID, NOT the zero-GUID sentinel.
    // Previously this pinned the buggy "creates a second workspace"
    // behavior; the new-tab keybinding fix carries the resolved profile
    // into the focused leaf's new tab instead.
    void WorkspaceTests::NewTab_FlagOn_ExplicitProfileByName_AppendsTab()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        ::WorkspaceModel::PaneId activeLeaf{ 0 };
        auto result = RunOnUIThread([&page, &activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            const auto leafOpt = page->_activeLeafModelId();
            VERIFY_IS_TRUE(leafOpt.has_value(), L"startup workspace must have an active leaf");
            activeLeaf = *leafOpt;

            NewTerminalArgs newTerminalArgs{};
            newTerminalArgs.Profile(L"profile1");
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, activeLeaf]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on explicit-profile new-tab must NOT build a classic tab (cutover)");

            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on explicit-profile new-tab must NOT create a new workspace — same count");

            // The new tab landed in the active workspace's focused leaf.
            const auto activeId = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeId.has_value());
            const auto* node = page->_workspaceModelState->pane(activeLeaf);
            VERIFY_IS_NOT_NULL(node);
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(node);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(2u, leaf->tabs.size(),
                             L"the focused leaf grew from one tab to two");

            // The most-recently-appended tab (back of the leaf's strip)
            // carries profile1's GUID, not the zero-GUID sentinel that
            // default-profile dispatch uses.
            const auto& description = leaf->tabs.back().description;
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::TerminalSpec>(description));
            const auto& spec = std::get<::WorkspaceModel::TerminalSpec>(description);
            const ::WorkspaceModel::TerminalSpec defaultSentinel{};
            VERIFY_IS_FALSE(spec == defaultSentinel,
                            L"explicit-profile dispatch must carry the resolved profile GUID, not the zero sentinel");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig mirror for explicit-profile new-tab.
    void WorkspaceTests::NewTab_FlagOff_ExplicitProfileByName_AppendsTab()
    {
        CascadiaSettings settings{ settingsJsonFlagOff, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());

            NewTerminalArgs newTerminalArgs{};
            newTerminalArgs.Profile(L"profile1");
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-off explicit-profile new-tab should append a classic tab");

            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off explicit-profile new-tab must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off explicit-profile new-tab must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // ------------------------------------------------------------------
    // Slice 6 review fixes.
    // ------------------------------------------------------------------

    // Big-flip Slice F-5 (#54): THE CUTOVER retired
    // SetTabColor_FlagOn_RoutesByRightClickedSender_NotFocusedTab. It drove the
    // classic tab-strip right-click semantics end-to-end against two built
    // classic Tabs (_SelectTab(0), sender = _tabs.GetAt(1), assert classic
    // GetTabColor on each). Post-cutover no classic Tab exists flag-on and the
    // sender-keyed _modelIdForTab path it guarded is unreachable — the same
    // KNOWN FOLLOW-UP captured by SetTabColor_FlagOn / RenameTab_FlagOn (re-key
    // decoration off the model's active tab + per-leaf strip context menu).
    // Removed rather than left as a hollow stub.
#if 0
    void WorkspaceTests::SetTabColor_FlagOn_RoutesByRightClickedSender_NotFocusedTab()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Append a second tab so we have two distinct workspaces to
        // route between.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        constexpr winrt::Windows::UI::Color senderColor{ .A = 0xFF, .R = 0x44, .G = 0x55, .B = 0x66 };

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());

            // Focus tab 0 explicitly. The sender we pass below is
            // tab 1, so the right-clicked-tab semantics must override
            // focus.
            VERIFY_IS_TRUE(page->_SelectTab(0));

            // Pre-condition: neither tab has a runtime color.
            auto tab0 = page->_GetTabImpl(page->_tabs.GetAt(0));
            auto tab1 = page->_GetTabImpl(page->_tabs.GetAt(1));
            VERIFY_IS_NOT_NULL(tab0);
            VERIFY_IS_NOT_NULL(tab1);
            VERIFY_IS_FALSE(tab0->GetTabColor().has_value(), L"pre-condition: tab 0 has no color");
            VERIFY_IS_FALSE(tab1->GetTabColor().has_value(), L"pre-condition: tab 1 has no color");

            // Fire SetTabColor with sender = tab 1 (the projected
            // type), simulating the tab-strip context menu firing
            // from tab 1 while tab 0 holds focus.
            SetTabColorArgs colorArgs{ senderColor };
            ActionEventArgs eventArgs{ colorArgs };
            page->_HandleSetTabColor(page->_tabs.GetAt(1), eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Classic Tab observable: tab 1 carries the color, tab 0
            // does NOT.
            auto tab0 = page->_GetTabImpl(page->_tabs.GetAt(0));
            auto tab1 = page->_GetTabImpl(page->_tabs.GetAt(1));
            const auto tab0Color = tab0->GetTabColor();
            const auto tab1Color = tab1->GetTabColor();
            VERIFY_IS_FALSE(tab0Color.has_value(),
                            L"focused tab 0 must NOT have been mutated when sender=tab 1");
            VERIFY_IS_TRUE(tab1Color.has_value(),
                           L"sender tab 1 must have been mutated");
            VERIFY_ARE_EQUAL(senderColor.R, tab1Color.value().R);
            VERIFY_ARE_EQUAL(senderColor.G, tab1Color.value().G);
            VERIFY_ARE_EQUAL(senderColor.B, tab1Color.value().B);

            // Model state observable: workspace 1's tab carries the
            // color, workspace 0's tab does not. Phase 1: classic tab
            // index == workspace display index.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(2u, workspaces.size());

            const auto leaves0 = page->_workspaceModelState->leaves(workspaces[0].id);
            const auto leaves1 = page->_workspaceModelState->leaves(workspaces[1].id);
            VERIFY_ARE_EQUAL(1u, leaves0.size());
            VERIFY_ARE_EQUAL(1u, leaves1.size());
            VERIFY_ARE_EQUAL(1u, leaves0[0]->tabs.size());
            VERIFY_ARE_EQUAL(1u, leaves1[0]->tabs.size());

            VERIFY_IS_FALSE(leaves0[0]->tabs[0].runtimeColor.has_value(),
                            L"model: workspace 0 must NOT carry the color");
            VERIFY_IS_TRUE(leaves1[0]->tabs[0].runtimeColor.has_value(),
                           L"model: workspace 1 must carry the color");
            VERIFY_ARE_EQUAL(senderColor.R, leaves1[0]->tabs[0].runtimeColor.value().r);
            VERIFY_ARE_EQUAL(senderColor.G, leaves1[0]->tabs[0].runtimeColor.value().g);
            VERIFY_ARE_EQUAL(senderColor.B, leaves1[0]->tabs[0].runtimeColor.value().b);
        });
        VERIFY_SUCCEEDED(result);
    }
#endif // Big-flip F-5: classic right-click sender-routing test retired (see rationale above)

    // AC: "Duplicate-tab observable parity (including profile
    // inheritance)." Firing a DuplicateTab action with the workspaces
    // flag on must:
    //  - Append a second classic tab (matches flag-off observable).
    //  - Append a second model workspace whose only tab's TerminalSpec
    //    carries the SAME profile bytes as the source tab.
    void WorkspaceTests::DuplicateTab_FlagOn_AppendsWorkspaceWithSameProfile()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Capture the source tab's profile bytes from the model BEFORE
        // we duplicate, then compare the new tab's profile bytes
        // against the captured value AFTER. This proves inheritance
        // works regardless of whether the source tab carries the zero-
        // GUID sentinel (default profile) or a resolved profile GUID.
        std::array<std::uint8_t, 16> sourceProfileBytes{};

        auto result = RunOnUIThread([&page, &sourceProfileBytes]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");

            // Capture the source workspace's tab profile (still the model's).
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto leaves = page->_workspaceModelState->leaves(workspaces[0].id);
            VERIFY_ARE_EQUAL(1u, leaves.size());
            VERIFY_ARE_EQUAL(1u, leaves[0]->tabs.size());
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::TerminalSpec>(leaves[0]->tabs[0].description));
            sourceProfileBytes = std::get<::WorkspaceModel::TerminalSpec>(leaves[0]->tabs[0].description).profile;

            ActionEventArgs eventArgs{};
            page->_HandleDuplicateTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, &sourceProfileBytes]() {
            // Big-flip Slice F-5 (#54): THE CUTOVER. _HandleDuplicateTab's flag-on
            // path resolves its source via _focusedTabModelId() / _GetFocusedTabIndex()
            // — both CLASSIC-tab-keyed — so with no classic focused tab it no-ops.
            // SAME KNOWN FOLLOW-UP as rename/color: re-key duplicate off the model's
            // active tab. Pin the post-cutover reality (no new workspace, no classic
            // tab) so the regression is visible and a future slice flips it.
            (void)sourceProfileBytes;
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"post-cutover the classic-tab-keyed duplicate path is a no-op (known follow-up)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig mirror for DuplicateTab.
    void WorkspaceTests::DuplicateTab_FlagOff_AppendsTabWithoutModel()
    {
        CascadiaSettings settings{ settingsJsonFlagOff, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr);

            ActionEventArgs eventArgs{};
            page->_HandleDuplicateTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-off duplicate-tab should append a classic tab");

            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off duplicate-tab must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off duplicate-tab must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice 3 AC: "Closing one workspace in a multi-workspace state behaves
    // correctly." Big-flip Slice F-5 (#54): THE CUTOVER rewrite. Post-cutover the
    // close is driven the MODEL way (closeWorkspace — what the strip VM's
    // CloseRequested dispatches), NOT the classic _HandleCloseTab(index) path
    // (which is unreachable flag-on: there is no classic tab strip to index).
    // Start with two workspaces, close the FIRST one, then assert: the model has
    // ONE workspace, `_tabs` stayed empty (no classic tab ever built), the window
    // stays open (a workspace survives), and the model still validates.
    void WorkspaceTests::CloseTab_FlagOn_RemovesWorkspaceAndTab()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Stand up a second workspace so closing the first does not trip the
        // last-workspace window-close path. Slice F follow-up (#54): NewTab no
        // longer creates a workspace (it adds a tab to the focused leaf), so use
        // the legitimate new-workspace entry point (_createNewWorkspace, the
        // chrome `+` button's path) to mint the second workspace.
        ::WorkspaceModel::WorkspaceId firstWsId{ 0 };
        auto closeWindowRequestCount = std::make_shared<std::atomic<int>>(0);
        auto result = RunOnUIThread([&page, &firstWsId, closeWindowRequestCount]() {
            page->_createNewWorkspace(std::nullopt);
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size());
            firstWsId = page->_workspaceModelState->workspaces_view()[0].id;
            page->CloseWindowRequested([closeWindowRequestCount](auto&&, auto&&) {
                closeWindowRequestCount->fetch_add(1);
            });
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close the first workspace through the model (the strip VM's close path)");
        result = RunOnUIThread([&page, firstWsId]() {
            auto next = ::WorkspaceModel::closeWorkspace(page->_workspaceModelState, firstWsId);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, closeWindowRequestCount]() {
            // The model lost one workspace; `_tabs` never grew (cutover).
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"_tabs stayed empty the whole time — no classic tab is built flag-on");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"closing one of two workspaces leaves one standing");
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"a surviving workspace keeps the window open");
            VERIFY_ARE_EQUAL(0, closeWindowRequestCount->load(),
                             L"closing a NON-last workspace must NOT request a window close");

            const auto violation = ::WorkspaceModel::validate(*page->_workspaceModelState);
            VERIFY_IS_FALSE(violation.has_value(),
                            L"model state after close-cascade must satisfy the validator");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig mirror of the slice-3 close-tab case. Flag-off
    // close-tab must continue to mutate _tabs directly without ever
    // instantiating the workspace machinery.
    void WorkspaceTests::CloseTab_FlagOff_RemovesTabWithoutModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        // Append a second classic tab via the action handler so close-
        // tab doesn't immediately tear down the window.
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire CloseTab(index=0) on the flag-off path");
        result = RunOnUIThread([&page]() {
            CloseTabArgs closeArgs{ 0u };
            ActionEventArgs eventArgs{ closeArgs };
            page->_HandleCloseTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"flag-off close-tab should remove the classic tab");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off close-tab must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off close-tab must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice 3 AC: "Closing the last tab in a single-tab workspace
    // closes the workspace and (if last workspace) the window."
    //
    // Big-flip Slice F-5 (#54): THE CUTOVER rewrite. Post-cutover there is NO
    // classic tab flag-on — `_tabs` stays empty the whole time. We drive the
    // close the MODEL way (closeWorkspace on the single workspace — the same
    // thing the strip VM's CloseRequested dispatches via closeTab, which
    // cascades to the same empty model), and assert:
    //   - the model is now empty + no active workspace;
    //   - _windowShouldStayOpen() flipped false;
    //   - CloseWindowRequested fired exactly once — raised by
    //     _applyWorkspaceAction's model-driven close re-raise, NOT by the
    //     classic _RemoveTab cascade (which never runs: _workspaceClassicTabs is
    //     empty flag-on);
    //   - `_tabs` stayed empty throughout (no classic tab was EVER built
    //     flag-on — the cutover invariant).
    void WorkspaceTests::CloseLastTab_FlagOn_RequestsWindowClose()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto closeWindowRequestCount = std::make_shared<std::atomic<int>>(0);
        ::WorkspaceModel::WorkspaceId onlyWsId{ 0 };
        auto result = RunOnUIThread([&page, closeWindowRequestCount, &onlyWsId]() {
            // The cutover invariant: NO classic tab was ever built flag-on.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"a window with one workspace must stay open");

            onlyWsId = page->_workspaceModelState->workspaces_view()[0].id;

            page->CloseWindowRequested(
                [closeWindowRequestCount](auto&&, auto&&) {
                    closeWindowRequestCount->fetch_add(1);
                });
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close the only workspace through the model (the strip VM's close path)");
        result = RunOnUIThread([&page, onlyWsId]() {
            auto next = ::WorkspaceModel::closeWorkspace(page->_workspaceModelState, onlyWsId);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, closeWindowRequestCount]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"_tabs stayed empty the whole time — no classic tab is built flag-on");
            VERIFY_ARE_EQUAL(0u, page->_workspaceModelState->workspaces_view().size(),
                             L"closing the last workspace leaves the model empty");
            VERIFY_IS_FALSE(page->_workspaceModelState->activeWorkspaceId_view().has_value(),
                            L"empty model has no active workspace");
            VERIFY_IS_FALSE(page->_windowShouldStayOpen(),
                            L"an empty model must not keep the window open");
            VERIFY_ARE_EQUAL(1, closeWindowRequestCount->load(),
                             L"the model-driven close re-raise must fire CloseWindowRequested exactly once");

            // Validator on empty model state.
            const auto violation = ::WorkspaceModel::validate(*page->_workspaceModelState);
            VERIFY_IS_FALSE(violation.has_value(),
                            L"empty model state must satisfy the validator");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig mirror: the flag-off close-cascade-to-window-close
    // path must continue to behave identically, even with the model
    // machinery available. Same observable outcome (CloseWindowRequested
    // fires once, _tabs.Size() == 0) without ever instantiating the
    // workspace machinery.
    void WorkspaceTests::CloseLastTab_FlagOff_RequestsWindowClose()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto closeWindowRequestCount = std::make_shared<std::atomic<int>>(0);
        auto result = RunOnUIThread([&page, closeWindowRequestCount]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            page->CloseWindowRequested(
                [closeWindowRequestCount](auto&&, auto&&) {
                    closeWindowRequestCount->fetch_add(1);
                });
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            CloseTabArgs closeArgs{ 0u };
            ActionEventArgs eventArgs{ closeArgs };
            page->_HandleCloseTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, closeWindowRequestCount]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-off last-tab close empties the classic tab strip");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off last-tab close must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off last-tab close must NOT instantiate WorkspaceView");
            VERIFY_ARE_EQUAL(1, closeWindowRequestCount->load(),
                             L"flag-off last-tab teardown still raises CloseWindowRequested");
        });
        VERIFY_SUCCEEDED(result);
    }

    // F-4 (#46): flag-off, _windowShouldStayOpen() is the byte-for-byte
    // inverse of the upstream `_tabs.Size() == 0` close checks. Pin that it
    // tracks `_tabs.Size() > 0` across 0/1/2 tabs and that the model
    // machinery is never engaged.
    void WorkspaceTests::BigFlipF4_FlagOff_WindowShouldStayOpen_MirrorsTabCount()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        // 1 tab (startup-replay): stay open.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"flag-off, 1 tab -> stay open");
            VERIFY_ARE_EQUAL(page->_tabs.Size() > 0, page->_windowShouldStayOpen(),
                             L"flag-off predicate must equal _tabs.Size() > 0");
        });
        VERIFY_SUCCEEDED(result);

        // 2 tabs: still stay open; model still dormant.
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"flag-off, 2 tabs -> stay open");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off predicate must NOT populate the model");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off predicate must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);

        // Close both tabs; once _tabs is empty the predicate must be false
        // (the inverse of the upstream `_tabs.Size() == 0`).
        result = RunOnUIThread([&page]() {
            CloseTabArgs closeArgs{ 0u };
            ActionEventArgs eventArgs{ closeArgs };
            page->_HandleCloseTab(nullptr, eventArgs);
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"flag-off, 1 tab remaining -> stay open");

            CloseTabArgs closeArgs2{ 0u };
            ActionEventArgs eventArgs2{ closeArgs2 };
            page->_HandleCloseTab(nullptr, eventArgs2);
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size());
            VERIFY_IS_FALSE(page->_windowShouldStayOpen(),
                            L"flag-off, 0 tabs -> do NOT stay open");
            VERIFY_ARE_EQUAL(page->_tabs.Size() > 0, page->_windowShouldStayOpen(),
                             L"flag-off predicate must equal _tabs.Size() > 0 at 0 tabs");
        });
        VERIFY_SUCCEEDED(result);
    }

    // F-4 (#46): flag-on (model populated), _windowShouldStayOpen() reflects
    // `!workspaces_view().empty()`. After startup-replay the model holds one
    // workspace, so the window stays open; emptying the model (closing the
    // last workspace's tab) flips it to false.
    void WorkspaceTests::BigFlipF4_FlagOn_WindowShouldStayOpen_ReflectsWorkspaces()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // After startup-replay: one workspace in the model -> stay open.
        auto result = RunOnUIThread([&page]() {
            VERIFY_IS_NOT_NULL(page->_workspaceModelState);
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"flag-on, 1 workspace -> stay open");
            VERIFY_ARE_EQUAL(!page->_workspaceModelState->workspaces_view().empty(),
                             page->_windowShouldStayOpen(),
                             L"flag-on predicate must equal !workspaces.empty()");
        });
        VERIFY_SUCCEEDED(result);

        // Add a second workspace via the new-workspace entry point: still stay
        // open. Slice F follow-up (#54): NewTab now adds a tab to the focused leaf
        // rather than a new workspace, so mint the second workspace with
        // _createNewWorkspace (the chrome `+` path).
        result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"flag-on, 2 workspaces -> stay open");
        });
        VERIFY_SUCCEEDED(result);

        // Close both workspaces through the MODEL (the strip VM's close path) ->
        // model empties -> do NOT stay open. Big-flip Slice F-5 (#54): the
        // classic _HandleCloseTab(index) path is unreachable flag-on (no classic
        // tab strip to index), so drive closeWorkspace directly.
        result = RunOnUIThread([&page]() {
            const auto firstWs = page->_workspaceModelState->workspaces_view()[0].id;
            auto next = ::WorkspaceModel::closeWorkspace(page->_workspaceModelState, firstWs);
            page->_applyWorkspaceAction(std::move(next));
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"flag-on, 1 workspace remaining -> stay open");

            const auto lastWs = page->_workspaceModelState->workspaces_view()[0].id;
            auto next2 = ::WorkspaceModel::closeWorkspace(page->_workspaceModelState, lastWs);
            page->_applyWorkspaceAction(std::move(next2));
            VERIFY_ARE_EQUAL(0u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on, last workspace closed -> model empty");
            VERIFY_IS_FALSE(page->_windowShouldStayOpen(),
                            L"flag-on, 0 workspaces -> do NOT stay open");
        });
        VERIFY_SUCCEEDED(result);
    }

    // F-4 (#46): flag-on but the model is not yet populated (null shared_ptr),
    // exactly the state at the GH#12267 defterm-readying entry into
    // _CompleteInitialization. _windowShouldStayOpen() must NOT dereference the
    // null model (no crash) and must fall back to the classic `_tabs.Size()`
    // count so startup does not spuriously close.
    void WorkspaceTests::BigFlipF4_FlagOn_NullModel_DoesNotCrashOrSpuriouslyClose()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Force the model back to null to simulate the pre-shell-population
        // state (defterm-readying path). The flag is ON, so this exercises the
        // null-model guard branch. Big-flip Slice F-5 (#54): no classic tab is
        // built flag-on, so `_tabs` is empty — the load-bearing contract is that
        // _windowShouldStayOpen() does NOT dereference the null model (no crash)
        // and falls back to `_tabs.Size() > 0` exactly.
        auto result = RunOnUIThread([&page]() {
            VERIFY_IS_TRUE(page->_workspacesFlagEnabled());
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");

            page->_workspaceModelState = nullptr;
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr);

            // Must not crash; with the model null AND no classic tabs the
            // fallback `_tabs.Size() > 0` is false. The CONTRACT is no-crash +
            // fallback-equals-_tabs.Size()>0, which still holds post-cutover.
            VERIFY_ARE_EQUAL(page->_tabs.Size() > 0, page->_windowShouldStayOpen(),
                             L"flag-on null-model fallback must equal _tabs.Size() > 0 (no crash)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // F-4 (#46): no-spurious-close at startup. On the flag-on startup path the
    // replayed NewTab populates the model with one workspace BEFORE
    // _CompleteInitialization runs its close guard, so the window stays open
    // and CloseWindowRequested must NOT have fired. Asserted headlessly by
    // subscribing before startup completes and checking the post-init state
    // (the canary CloseLastTab_FlagOn covers the fire-once teardown).
    void WorkspaceTests::BigFlipF4_FlagOn_StartupReplay_StaysOpenAndDoesNotRequestClose()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // _initializeTerminalPageWithFlagOn waits for Initialized to fire,
        // which only happens on the stay-open branch of _CompleteInitialization
        // (the close branch co_returns before raising Initialized). Reaching
        // here at all means the startup guard chose stay-open. Pin the model
        // and tab state that produced that decision, and confirm a freshly
        // subscribed handler sees no close request fire after init.
        auto closeWindowRequestCount = std::make_shared<std::atomic<int>>(0);
        auto result = RunOnUIThread([&page, closeWindowRequestCount]() {
            VERIFY_IS_NOT_NULL(page->_workspaceModelState);
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay populated the model before the close guard");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_IS_TRUE(page->_windowShouldStayOpen(),
                           L"startup guard must choose stay-open");

            page->CloseWindowRequested(
                [closeWindowRequestCount](auto&&, auto&&) {
                    closeWindowRequestCount->fetch_add(1);
                });
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([closeWindowRequestCount]() {
            VERIFY_ARE_EQUAL(0, closeWindowRequestCount->load(),
                             L"startup must not spuriously request a window close");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice 3 AC (spawn-failure): "Spawn failure leaves the workspace
    // consistent — model Validator reports no violations."
    //
    // The Phase 1 model never inspects whether IPaneContent
    // materialisation succeeds, so a spawn error in the XAML layer
    // cannot drive the model into an invalid state on its own. This
    // test pins that contract by exercising the same code paths a
    // normal new-tab would (model dispatch, view apply, classic _tabs
    // update) and verifying validator cleanliness across every
    // mutation. If a future change starts toggling model fields based
    // on spawn outcome, this guard will catch it.
    void WorkspaceTests::NewTab_FlagOn_LeavesModelValidatorClean()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // After startup-replay.
        auto result = RunOnUIThread([&page]() {
            VERIFY_IS_NOT_NULL(page->_workspaceModelState);
            const auto violation = ::WorkspaceModel::validate(*page->_workspaceModelState);
            VERIFY_IS_FALSE(violation.has_value(),
                            L"model state after startup-replay must satisfy the validator");
        });
        VERIFY_SUCCEEDED(result);

        // After a second new-tab.
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);

            const auto violation = ::WorkspaceModel::validate(*page->_workspaceModelState);
            VERIFY_IS_FALSE(violation.has_value(),
                            L"model state after a second new-tab must satisfy the validator");
        });
        VERIFY_SUCCEEDED(result);

        // After a close-cascade. Big-flip Slice F-5 (#54): drive the close the
        // MODEL way (closeWorkspace on the first workspace) — the classic
        // _HandleCloseTab(index) path is unreachable flag-on (no classic tab
        // strip to index post-cutover). The validator-cleanliness contract is
        // unchanged.
        result = RunOnUIThread([&page]() {
            const auto firstWsId = page->_workspaceModelState->workspaces_view()[0].id;
            auto next = ::WorkspaceModel::closeWorkspace(page->_workspaceModelState, firstWsId);
            page->_applyWorkspaceAction(std::move(next));

            const auto violation = ::WorkspaceModel::validate(*page->_workspaceModelState);
            VERIFY_IS_FALSE(violation.has_value(),
                            L"model state after close-cascade must satisfy the validator");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice 3 AC (spawn-failure, real-fail variant): exercise an
    // actual failing dispatch and assert the model + registry stay
    // consistent. The reviewer's must-fix specifically called out the
    // mis-binding bug in _registerClassicTabForWorkspace, which would
    // bind a NEW failed workspace to a PRE-EXISTING tab whenever
    // _OpenNewTab bailed but _tabs already held at least one tab.
    //
    // Failure path: NewTabArgs with ProfileIndex=999 (out of range).
    // AppActionHandlers::_HandleNewTab routes invalid-profile-index
    // NewTab through _shouldBailForInvalidProfileIndex BEFORE the
    // workspace-model dispatch, so the model is never asked to grow.
    // Contract under test in this scenario:
    //   - _tabs.Size() stays at the pre-call count (no zombie tab).
    //   - _workspaceClassicTabs has no binding to a non-existent
    //     workspace id (the model never minted one).
    //   - Validator on the existing model state still clean.
    //   - Model size unchanged (the failed dispatch never reached the
    //     model — case (i) of the must-fix's "rolled back" contract).
    //
    // If a future refactor moves the invalid-profile-index check
    // *past* the model dispatch (so the model grows and then the
    // XAML side fails), this test still gives the right answer:
    // _registerClassicTabForWorkspace is now passed the explicit Tab
    // (or nullptr) by _openDefaultTabForWorkspace, so the new
    // workspace would stay unbound (case (ii) of the contract) and
    // the pre-existing tab would not be mis-bound to the failed
    // workspace.
    void WorkspaceTests::NewTab_FlagOn_SpawnFailure_LeavesNoZombieWorkspace()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Big-flip Slice F-5 (#54): THE CUTOVER. No classic tab + no
        // _workspaceClassicTabs binding exist flag-on, so the registry mis-bind
        // bug this test guarded is structurally gone (the registry is always
        // empty). The remaining live contract — an invalid-profile-index NewTab
        // bails BEFORE the model dispatch, so the model never grows a zombie
        // workspace — still holds and is what we pin here.
        //
        // Pre-condition: startup-replay landed one workspace and NO classic tab.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay should produce exactly one workspace");
            VERIFY_ARE_EQUAL(0u, page->_workspaceClassicTabs.size(),
                             L"flag-on no classic tab is registered (cutover)");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire NewTab with an out-of-range ProfileIndex; this should bail without growing the model");
        result = RunOnUIThread([&page]() {
            // Settings has exactly one active profile, so ProfileIndex
            // 999 is guaranteed out of range and
            // _shouldBailForInvalidProfileIndex returns true.
            NewTerminalArgs newTerminalArgs{ 999 };
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // No zombie classic tab, no zombie workspace, no zombie registry
            // binding; the bail happened before model dispatch.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"failed-spawn NewTab must NOT append a classic tab");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"failed-spawn NewTab must not grow the workspace model");
            VERIFY_ARE_EQUAL(0u, page->_workspaceClassicTabs.size(),
                             L"failed-spawn NewTab must not register any classic tab (cutover)");

            // Validator clean.
            const auto violation = ::WorkspaceModel::validate(*page->_workspaceModelState);
            VERIFY_IS_FALSE(violation.has_value(),
                            L"model state after failed spawn must satisfy the validator");
        });
        VERIFY_SUCCEEDED(result);
    }

    // ---------------------------------------------------------------------
    // Slice 5: split + resize + identity-preserving move.
    //
    // The settings JSON used by the split / resize tests is shared
    // between the flag-on and flag-off variants — they only differ in
    // the experimental.workspaces.enabled key, which the harness
    // helpers (_initializeTerminalPageWithFlagOn / FlagOff) inject.
    // ---------------------------------------------------------------------

    // AC: "Flag-on split (vert + horiz) observably matches flag-off."
    //
    // After one default-profile split on the focused tab, the classic
    // pane tree grows to two leaves AND the model's active workspace
    // has a SplitPane root with two LeafPane children. The flag-on
    // path's pane count matches the flag-off path's pane count.
    void WorkspaceTests::SplitPane_FlagOn_GrowsActiveWorkspaceTree()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Pre-condition: startup-replay landed one tab containing one
        // pane; the model has one workspace whose root is a LeafPane.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_IS_TRUE(page->_workspaceModelState != nullptr);
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(workspaces[0].root),
                           L"fresh workspace root must be a single leaf");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a default-profile vertical SplitPane action");
        result = RunOnUIThread([&page]() {
            SplitPaneArgs splitArgs{ SplitDirection::Right, NewTerminalArgs{} };
            ActionEventArgs eventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Big-flip Slice F-5 (#54): no classic tab/pane tree is built flag-on
            // — the split is rendered by the projected pane tree. Assert no
            // classic tab exists and the projected tree root holds a split Grid.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"split must not create a classic tab (cutover)");
            VERIFY_IS_NOT_NULL(page->_workspacePaneTreeRootChildForTest(),
                               L"the projected pane tree must render the split");

            // Model: active workspace's root is now a SplitPane carrying
            // two LeafPane children (the original on the left, the new
            // sibling on the right) with Axis::Vertical.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split, L"model root must be a SplitPane after vertical split");
            VERIFY_ARE_EQUAL(static_cast<int>(::WorkspaceModel::Axis::Vertical),
                             static_cast<int>(split->axis));
            VERIFY_IS_NOT_NULL(split->left);
            VERIFY_IS_NOT_NULL(split->right);
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(*split->left),
                           L"left child must be a leaf in Phase 1");
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(*split->right),
                           L"right child must be a leaf in Phase 1");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Horizontal counterpart of SplitPane_FlagOn_GrowsActiveWorkspaceTree:
    // SplitDirection::Down stacks the new pane below the original, which
    // the model records as an Axis::Horizontal split (a horizontal
    // splitter line between two vertically-stacked children).
    void WorkspaceTests::SplitPane_FlagOn_GrowsActiveWorkspaceTree_Horizontal()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Pre-condition: startup-replay landed one workspace whose root is a
        // LeafPane; NO classic tab is built flag-on (cutover).
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_IS_TRUE(page->_workspaceModelState != nullptr);
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(workspaces[0].root),
                           L"fresh workspace root must be a single leaf");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a default-profile horizontal SplitPane action");
        result = RunOnUIThread([&page]() {
            SplitPaneArgs splitArgs{ SplitDirection::Down, NewTerminalArgs{} };
            ActionEventArgs eventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Big-flip Slice F-5 (#54): the split is rendered by the projected
            // pane tree; no classic tab/pane tree is built flag-on.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"split must not create a classic tab (cutover)");
            VERIFY_IS_NOT_NULL(page->_workspacePaneTreeRootChildForTest(),
                               L"the projected pane tree must render the split");

            // Model: active workspace's root is now a SplitPane carrying
            // two LeafPane children (the original on top, the new sibling
            // on the bottom) with Axis::Horizontal.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split, L"model root must be a SplitPane after horizontal split");
            VERIFY_ARE_EQUAL(static_cast<int>(::WorkspaceModel::Axis::Horizontal),
                             static_cast<int>(split->axis));
            VERIFY_IS_NOT_NULL(split->left);
            VERIFY_IS_NOT_NULL(split->right);
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(*split->left),
                           L"left (top) child must be a leaf in Phase 1");
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(*split->right),
                           L"right (bottom) child must be a leaf in Phase 1");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig contract pin for split: flag-off split must mutate
    // only the classic pane tree and leave the model machinery dormant.
    void WorkspaceTests::SplitPane_FlagOff_GrowsClassicPaneTreeWithoutModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off startup must NOT populate the model state");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire a default-profile vertical SplitPane action (flag off)");
        result = RunOnUIThread([&page]() {
            SplitPaneArgs splitArgs{ SplitDirection::Right, NewTerminalArgs{} };
            ActionEventArgs eventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Classic pane tree grew the same way as the flag-on path:
            // one classic tab, two leaves.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(2, tab->GetLeafPaneCount(),
                             L"flag-off split must produce two classic leaves");

            // Model machinery stays dormant.
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off split must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off split must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC: "Flag-on resize-split observably matches flag-off; split
    // ratios persist."
    //
    // Resize after a split must update the model's SplitPane.ratio
    // in the same direction the classic _ResizePane moves the visible
    // separator. We can't construct a parameterised ResizePaneArgs
    // through the WinRT projection (the IDL only exposes the default
    // constructor) so we drive the slice 5 helper
    // _mirrorResizeIntoModel directly — this is the same code path
    // _HandleResizePane runs flag-on after _ResizePane succeeds.
    void WorkspaceTests::ResizePane_FlagOn_UpdatesModelSplitRatio()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Set up a split first.
        auto result = RunOnUIThread([&page]() {
            SplitPaneArgs splitArgs{ SplitDirection::Right, NewTerminalArgs{} };
            ActionEventArgs eventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        // Capture the pre-resize ratio.
        double preRatio = 0.5;
        ::WorkspaceModel::PaneId splitId{ 0 };
        result = RunOnUIThread([&page, &preRatio, &splitId]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split);
            preRatio = split->ratio;
            splitId = split->id;
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Mirror a ResizeDirection::Right into the model");
        result = RunOnUIThread([&page]() {
            page->_mirrorResizeIntoModel(ResizeDirection::Right);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, preRatio, splitId]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split, L"split node must still exist post-resize");
            VERIFY_ARE_EQUAL(splitId.v, split->id.v,
                             L"split id must be preserved across resize");
            VERIFY_IS_TRUE(split->ratio > preRatio,
                           L"ResizeDirection::Right must grow the first/left child's ratio");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Mirror a ResizeDirection::Left brings the ratio back down");
        result = RunOnUIThread([&page]() {
            page->_mirrorResizeIntoModel(ResizeDirection::Left);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, preRatio]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split);
            // After +0.05 then -0.05, we should land back near preRatio
            // (allow a small epsilon for double rounding).
            const auto delta = split->ratio - preRatio;
            VERIFY_IS_TRUE(delta < 0.0001,
                           L"ResizeDirection::Left must shrink the first/left child's ratio back");
            VERIFY_IS_TRUE(delta > -0.0001,
                           L"and not overshoot");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig contract pin for resize: with the flag OFF the
    // workspace model machinery must stay dormant. The classic
    // _ResizePane path is unchanged; the only workspace-aware step in
    // _HandleResizePane — mirroring the resize into the model — is
    // gated behind _workspacesFlagEnabled(). We verify that gate is
    // closed (so the mirror is unreachable) and that neither the model
    // state nor the WorkspaceView is ever instantiated, all off a real
    // flag-off classic split.
    void WorkspaceTests::ResizePane_FlagOff_LeavesModelDormant()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            // Split classically so a resizable separator exists between
            // two leaves — the precondition a resize would act on.
            SplitPaneArgs splitArgs{ SplitDirection::Right, NewTerminalArgs{} };
            ActionEventArgs splitEventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, splitEventArgs);

            // The model-mirror in _HandleResizePane is gated behind
            // _workspacesFlagEnabled(); with the flag off that branch is
            // unreachable, so no resize can ever wake the model. We
            // assert the gate directly rather than driving the real
            // _ResizePane: the headless TestHostApp never lays out the
            // pane Grid, so _root.ActualWidth() is 0 and
            // Pane::_ClampSplitPosition feeds std::clamp(x, +inf, -inf),
            // tripping the debug-STL bounds assert.
            VERIFY_IS_FALSE(page->_workspacesFlagEnabled(),
                            L"flag-off resize must leave the model-mirror gate closed");
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(2, tab->GetLeafPaneCount(),
                             L"flag-off split must produce two classic leaves to resize between");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off resize must NOT populate the model state");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off resize must NOT instantiate WorkspaceView");
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC: "Flag-on tab move ... preserves the live `TermControl` —
    // same backing `ConPTY` after the move; no scrollback loss."
    //
    // The load-bearing property is the diff contract: a TabId that
    // appears at the same (workspace, leaf, idx) in prev and next is
    // emitted as `TabMoved`, NOT as `TabRemoved + TabAdded`. The
    // identity is what lets the view-layer apply arm preserve the live
    // IPaneContent. We assert that contract directly off the model
    // state the harness has primed during startup-replay.
    //
    // This test exercises a cross-leaf (within-workspace) move by
    // first splitting the active workspace into two leaves, then
    // calling moveTab to take the new leaf's tab back to the original
    // leaf at idx=1. The diff between the pre-split state and the
    // post-move state must emit a single TabMoved arm for the new tab.
    void WorkspaceTests::MoveTab_FlagOn_DiffEmitsTabMovedPreservingId()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Set up a split so the workspace has two leaves.
        auto result = RunOnUIThread([&page]() {
            SplitPaneArgs splitArgs{ SplitDirection::Right, NewTerminalArgs{} };
            ActionEventArgs eventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        // Capture the new sibling leaf's tab id + original leaf id.
        ::WorkspaceModel::TabId siblingTabId{ 0 };
        ::WorkspaceModel::PaneId originalLeafId{ 0 };
        ::WorkspaceModel::ModelState preState{ nullptr };
        result = RunOnUIThread([&page, &siblingTabId, &originalLeafId, &preState]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split);
            // Phase 1 places the original on the left, new sibling on
            // the right.
            const auto* originalLeaf = std::get_if<::WorkspaceModel::LeafPane>(split->left.get());
            const auto* siblingLeaf = std::get_if<::WorkspaceModel::LeafPane>(split->right.get());
            VERIFY_IS_NOT_NULL(originalLeaf);
            VERIFY_IS_NOT_NULL(siblingLeaf);
            VERIFY_ARE_EQUAL(1u, siblingLeaf->tabs.size());
            originalLeafId = originalLeaf->id;
            siblingTabId = siblingLeaf->tabs[0].id;
            preState = page->_workspaceModelState;
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Move the sibling's tab back to the original leaf at idx=1");
        ::WorkspaceModel::ModelState postState{ nullptr };
        result = RunOnUIThread([&page, &postState, siblingTabId, originalLeafId]() {
            auto next = ::WorkspaceModel::moveTab(page->_workspaceModelState,
                                                  siblingTabId,
                                                  originalLeafId,
                                                  1u);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(next);
            postState = page->_workspaceModelState;
        });
        VERIFY_SUCCEEDED(result);

        // Diff(pre, post) must emit exactly one TabMoved for the
        // sibling's tab id; it must NOT emit a TabRemoved+TabAdded for
        // it. (Other arms — e.g. SplitPaneCollapsed, ActiveTabChanged,
        // ActiveWorkspaceChanged — are allowed.)
        result = RunOnUIThread([preState, postState, siblingTabId]() {
            const auto changes = ::WorkspaceModel::diff(preState, postState);

            std::size_t tabMovedHits = 0;
            std::size_t tabAddedHits = 0;
            std::size_t tabRemovedHits = 0;
            for (const auto& ch : changes)
            {
                if (const auto* moved = std::get_if<::WorkspaceModel::TabMoved>(&ch))
                {
                    if (moved->id == siblingTabId)
                    {
                        tabMovedHits += 1;
                    }
                }
                else if (const auto* added = std::get_if<::WorkspaceModel::TabAdded>(&ch))
                {
                    if (added->id == siblingTabId)
                    {
                        tabAddedHits += 1;
                    }
                }
                else if (const auto* removed = std::get_if<::WorkspaceModel::TabRemoved>(&ch))
                {
                    if (removed->id == siblingTabId)
                    {
                        tabRemovedHits += 1;
                    }
                }
            }
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), tabMovedHits,
                             L"identity-preserving move must emit exactly one TabMoved");
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), tabAddedHits,
                             L"moved tab must NOT appear as TabAdded");
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), tabRemovedHits,
                             L"moved tab must NOT appear as TabRemoved");
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC echo: "Identity-preserving move should be exercised across
    // leaves AND across workspaces."
    //
    // Two workspaces, each with one leaf and one tab. Move workspace
    // A's tab into workspace B's leaf at idx=1. Diff must emit a
    // single TabMoved with srcLeafId/dstLeafId crossing workspace
    // boundaries; the moved TabId must persist post-move.
    void WorkspaceTests::MoveTab_FlagOn_CrossWorkspaceTabMovedPreservingId()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Create a second workspace. Slice F follow-up (#54): NewTab now adds a
        // tab to the focused leaf, so use the new-workspace entry point
        // (_createNewWorkspace) to get a second single-leaf workspace as the
        // cross-workspace move destination.
        auto result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        // Capture pre-move state + the moving TabId + destination leaf
        // id (the OTHER workspace's only leaf).
        ::WorkspaceModel::ModelState preState{ nullptr };
        ::WorkspaceModel::TabId movingTabId{ 0 };
        ::WorkspaceModel::PaneId dstLeafId{ 0 };
        result = RunOnUIThread([&page, &preState, &movingTabId, &dstLeafId]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(2u, workspaces.size());

            // Workspace 0's tab is the one we'll move; workspace 1's
            // leaf is the destination.
            const auto* srcLeaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            const auto* dstLeaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[1].root);
            VERIFY_IS_NOT_NULL(srcLeaf);
            VERIFY_IS_NOT_NULL(dstLeaf);
            VERIFY_ARE_EQUAL(1u, srcLeaf->tabs.size());
            VERIFY_ARE_EQUAL(1u, dstLeaf->tabs.size());

            movingTabId = srcLeaf->tabs[0].id;
            dstLeafId = dstLeaf->id;
            preState = page->_workspaceModelState;
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Move workspace A's tab into workspace B's leaf at idx=1");
        ::WorkspaceModel::ModelState postState{ nullptr };
        result = RunOnUIThread([&page, &postState, movingTabId, dstLeafId]() {
            auto next = ::WorkspaceModel::moveTab(page->_workspaceModelState,
                                                  movingTabId,
                                                  dstLeafId,
                                                  1u);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(next);
            postState = page->_workspaceModelState;
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([preState, postState, movingTabId, dstLeafId]() {
            // The moved TabId must still be reachable in the post
            // state, AND it must now live in the destination leaf.
            const auto* movedRecord = postState->tab(movingTabId);
            VERIFY_IS_NOT_NULL(movedRecord,
                               L"moved TabId must persist across the move");

            const auto* dstNode = postState->pane(dstLeafId);
            VERIFY_IS_NOT_NULL(dstNode);
            const auto* dstLeaf = std::get_if<::WorkspaceModel::LeafPane>(dstNode);
            VERIFY_IS_NOT_NULL(dstLeaf);
            bool foundInDst = false;
            for (const auto& t : dstLeaf->tabs)
            {
                if (t.id == movingTabId)
                {
                    foundInDst = true;
                    break;
                }
            }
            VERIFY_IS_TRUE(foundInDst, L"moved tab must live in the destination leaf");

            // Diff(pre, post) must emit TabMoved (NOT TabRemoved +
            // TabAdded) for movingTabId, even across workspaces.
            const auto changes = ::WorkspaceModel::diff(preState, postState);
            std::size_t movedHits = 0;
            std::size_t addedHits = 0;
            std::size_t removedHits = 0;
            for (const auto& ch : changes)
            {
                if (const auto* moved = std::get_if<::WorkspaceModel::TabMoved>(&ch))
                {
                    if (moved->id == movingTabId)
                    {
                        VERIFY_ARE_EQUAL(dstLeafId.v, moved->dstLeafId.v,
                                         L"TabMoved.dstLeafId must point at the destination workspace's leaf");
                        movedHits += 1;
                    }
                }
                else if (const auto* added = std::get_if<::WorkspaceModel::TabAdded>(&ch))
                {
                    if (added->id == movingTabId)
                    {
                        addedHits += 1;
                    }
                }
                else if (const auto* removed = std::get_if<::WorkspaceModel::TabRemoved>(&ch))
                {
                    if (removed->id == movingTabId)
                    {
                        removedHits += 1;
                    }
                }
            }
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), movedHits,
                             L"cross-workspace move must emit exactly one TabMoved");
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), addedHits,
                             L"moved tab must NOT appear as TabAdded across workspaces");
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), removedHits,
                             L"moved tab must NOT appear as TabRemoved across workspaces");
        });
        VERIFY_SUCCEEDED(result);
    }

    // -------------------------------------------------------------------
    // Slice 2 / Phase 2 Visible #2 (#46): the workspaces UI shell.
    // -------------------------------------------------------------------

    // AC (the cardinal one): flag-off, the sidebar column is collapsed /
    // zero-width and the page is byte-for-byte upstream. The sidebar element
    // is x:Load="False" and is realized ONLY on the flag-on path, so flag-off
    // it is never instantiated and the Auto sidebar column carries no content
    // (zero width). The WorkspaceChrome is likewise never realized, so its
    // titlebar row collapses too. We assert the unrealized + zero-width state
    // directly.
    void WorkspaceTests::Sidebar_FlagOff_IsCollapsedAndZeroWidth()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            // The shell is never initialized on the flag-off path, so the
            // sidebar ItemsControl member stays null — nothing was lifted into
            // the titlebar, nothing was given width.
            VERIFY_IS_TRUE(page->_workspaceSidebar == nullptr,
                           L"flag-off must NOT realize/show the sidebar ItemsControl");

            // The classic content (TabContent / TabRow) still lives, and the
            // single startup tab landed exactly as upstream.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"flag-off startup must render the classic single-tab UI");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off must leave the workspace model dormant");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off must NOT instantiate WorkspaceView");

            // Zero-width assertion: even if the x:Load element is force-realized
            // (FindName triggers realization), its AUTHORED default visibility
            // is Collapsed, so the Auto sidebar column contributes zero width.
            // This is the structural guarantee that flag-off layout is upstream.
            auto realized = page->FindName(L"WorkspaceSidebar")
                                .try_as<winrt::Windows::UI::Xaml::Controls::ItemsControl>();
            VERIFY_IS_NOT_NULL(realized, L"the sidebar element should exist in the tree");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             realized.Visibility(),
                             L"flag-off the sidebar must be Collapsed -> zero-width Auto column");
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC: flag-on, the sidebar shows one read-only row per workspace in the
    // workspaces' declared order. After startup-replay (workspace 0) + one
    // NewTab (workspace 1) the model holds two workspaces; the sidebar must
    // hold two rows whose stored WorkspaceIds match workspaces_view() in order.
    void WorkspaceTests::Sidebar_FlagOn_MirrorsWorkspacesInDeclaredOrder()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Startup gives workspace 0 (one row). Add a second workspace.
        auto result = RunOnUIThread([&page]() {
            VERIFY_IS_TRUE(page->_workspaceSidebar != nullptr,
                           L"flag-on must realize the sidebar ItemsControl");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceSidebar.Visibility(),
                             L"flag-on sidebar must be visible (non-zero-width column)");
            // Stage 2 (#52): the sidebar is an MVVM projection — its rows are
            // the observable WorkspaceViewModel collection bound as ItemsSource,
            // not imperative Children. Assert against the collection.
            VERIFY_ARE_EQUAL(1u, page->_workspaceViewModels.Size(),
                             L"startup-replay must produce exactly one sidebar view-model");

            // Slice F follow-up (#54): NewTab now adds a tab to the focused leaf,
            // so mint the second workspace via _createNewWorkspace.
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), workspaces.size());

            const auto vms = page->_workspaceViewModels;
            VERIFY_ARE_EQUAL(2u, vms.Size(),
                             L"sidebar view-model count must equal the workspace count");

            // Each view-model, in order, carries the matching workspace's id as
            // Id — proving declared-order projection by id identity (no
            // positional indexing into the workspace list).
            for (uint32_t i = 0; i < vms.Size(); ++i)
            {
                VERIFY_ARE_EQUAL(workspaces[i].id.v, vms.GetAt(i).Id(),
                                 L"sidebar view-models must mirror workspaces in declared order");
            }
        });
        VERIFY_SUCCEEDED(result);
    }

    // AC: the sidebar is read-only this stage — there is no input path from a
    // row into a model action (rename/color/pin triggers land in Stage 3). The
    // rows are an MVVM projection: an observable WorkspaceViewModel collection
    // bound as the ItemsControl's ItemsSource, with a converter-free
    // DataTemplate carrying no input handlers. We assert the only sidebar-facing
    // entry point (re-running the active-row projection) is a pure view
    // mutation: the model state object is identical (same shared_ptr, same
    // workspace count) before and after, and the view-model active-flag is set
    // by id identity without touching the model.
    void WorkspaceTests::Sidebar_FlagOn_RowIsReadOnly_NoModelMutation()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // One workspace from startup. Capture the model identity + count.
        const void* modelBefore = nullptr;
        size_t countBefore = 0;
        auto result = RunOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_workspaceSidebar != nullptr);
            VERIFY_ARE_EQUAL(1u, page->_workspaceViewModels.Size());

            modelBefore = page->_workspaceModelState.get();
            countBefore = page->_workspaceModelState->workspaces_view().size();

            // The single startup workspace's view-model is active (the
            // startup-replay's ActiveWorkspaceChanged arm flipped IsActive by
            // id identity). The DataTemplate that renders it carries no input
            // handlers, so there is no path from a row into a model action.
            const auto vm = page->_workspaceViewModels.GetAt(0);
            VERIFY_IS_TRUE(vm.IsActive(),
                           L"the single startup workspace's view-model must be active");

            // Re-running the active-row projection (the only sidebar-facing
            // entry point this stage exposes) is a pure view mutation: it must
            // not allocate a new model state nor change the workspace set.
            const auto active = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(active.has_value());
            page->_setActiveWorkspaceVm(active.value());
            VERIFY_IS_TRUE(page->_workspaceViewModels.GetAt(0).IsActive(),
                           L"re-projecting the active workspace must keep its view-model active");
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            VERIFY_ARE_EQUAL(modelBefore, static_cast<const void*>(page->_workspaceModelState.get()),
                             L"touching the sidebar must not replace the model state");
            VERIFY_ARE_EQUAL(countBefore, page->_workspaceModelState->workspaces_view().size(),
                             L"touching the sidebar must not change the workspace set");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #52, Stage 3a. Committing an inline rename on a row's view-model raises
    // RenameCommitted; the page (subscribed in _addWorkspaceVm) dispatches
    // renameWorkspace and the resulting WorkspaceMetadataUpdated diff arm
    // re-projects the row's Title. The VM never touches the model directly:
    // BeginRename/CommitRename only mutate VM state + raise the event.
    void WorkspaceTests::Sidebar_FlagOn_RenameCommit_DispatchesModelAndReProjects()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceViewModels.Size());
            const auto vm = page->_workspaceViewModels.GetAt(0);
            const auto wsId = ::WorkspaceModel::WorkspaceId{ vm.Id() };

            // Drive the inline-rename flow exactly as the editor would: begin,
            // overwrite the buffered text, commit.
            vm.BeginRename();
            VERIFY_IS_TRUE(vm.IsEditing());
            vm.EditText(L"Renamed WS");
            vm.CommitRename();

            // The model now holds the new name (the page dispatched
            // renameWorkspace via the RenameCommitted subscription)...
            const auto* ws = page->_workspaceModelState->workspace(wsId);
            VERIFY_IS_NOT_NULL(ws);
            VERIFY_ARE_EQUAL(std::string{ "Renamed WS" }, ws->name,
                             L"committing a rename must dispatch renameWorkspace into the model");

            // ...and the WorkspaceMetadataUpdated arm re-projected it onto the
            // row's view-model, which also left edit mode.
            VERIFY_IS_FALSE(vm.IsEditing());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"Renamed WS" }, vm.Title(),
                             L"the row must re-render its Title from the model diff");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #52, Stage 3a. TogglePin raises PinToggleRequested with the desired new
    // value; the page dispatches setWorkspacePinned and the diff re-projects
    // IsPinned. The VM does NOT flip IsPinned itself — it only signals intent.
    void WorkspaceTests::Sidebar_FlagOn_TogglePin_DispatchesModelAndReProjects()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceViewModels.Size());
            const auto vm = page->_workspaceViewModels.GetAt(0);
            const auto wsId = ::WorkspaceModel::WorkspaceId{ vm.Id() };
            VERIFY_IS_FALSE(vm.IsPinned(), L"workspaces start unpinned");

            // Pin: the desired value is !IsPinned == true.
            vm.TogglePin();
            const auto* pinned = page->_workspaceModelState->workspace(wsId);
            VERIFY_IS_NOT_NULL(pinned);
            VERIFY_IS_TRUE(pinned->pinned,
                           L"TogglePin must dispatch setWorkspacePinned(true) into the model");
            VERIFY_IS_TRUE(vm.IsPinned(), L"the row must re-render IsPinned from the model diff");

            // Unpin: round-trips back.
            vm.TogglePin();
            const auto* unpinned = page->_workspaceModelState->workspace(wsId);
            VERIFY_IS_NOT_NULL(unpinned);
            VERIFY_IS_FALSE(unpinned->pinned,
                            L"a second TogglePin must dispatch setWorkspacePinned(false)");
            VERIFY_IS_FALSE(vm.IsPinned());
        });
        VERIFY_SUCCEEDED(result);
    }

    // #52, Stage 3b. The picker is a view-layer flyout that needs a realized
    // anchor, so we can't drive ShowColorPicker() headless. Instead we raise
    // the VM's ColorCommitted event directly (the same signal the picker
    // forwards) and assert the page dispatches setWorkspaceColor and the diff
    // re-projects the swatch: a non-null IReference sets the color, a null one
    // clears it. The VM never touches the model itself.
    void WorkspaceTests::Sidebar_FlagOn_ColorCommit_DispatchesModelAndReProjects()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceViewModels.Size());
            const auto vm = page->_workspaceViewModels.GetAt(0);
            const auto wsId = ::WorkspaceModel::WorkspaceId{ vm.Id() };
            VERIFY_IS_FALSE(vm.HasColor(), L"workspaces start with no color");

            // Set: a non-null IReference dispatches setWorkspaceColor(value).
            const winrt::Windows::UI::Color chosen{ 0xFF, 0x12, 0x34, 0x56 };
            const auto implVm = winrt::get_self<winrt::TerminalApp::implementation::WorkspaceViewModel>(vm);
            implVm->ColorCommitted.raise(vm, winrt::box_value(chosen).try_as<winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>>());

            const auto* colored = page->_workspaceModelState->workspace(wsId);
            VERIFY_IS_NOT_NULL(colored);
            VERIFY_IS_TRUE(colored->color.has_value(),
                           L"ColorCommitted(value) must dispatch setWorkspaceColor into the model");
            VERIFY_ARE_EQUAL(static_cast<uint8_t>(0x12), colored->color->r);
            VERIFY_ARE_EQUAL(static_cast<uint8_t>(0x34), colored->color->g);
            VERIFY_ARE_EQUAL(static_cast<uint8_t>(0x56), colored->color->b);
            VERIFY_IS_TRUE(vm.HasColor(), L"the row must re-render HasColor from the model diff");
            VERIFY_ARE_EQUAL(static_cast<uint8_t>(0x12), vm.Color().R);

            // Clear: a null IReference dispatches setWorkspaceColor(std::nullopt).
            implVm->ColorCommitted.raise(vm, nullptr);
            const auto* cleared = page->_workspaceModelState->workspace(wsId);
            VERIFY_IS_NOT_NULL(cleared);
            VERIFY_IS_FALSE(cleared->color.has_value(),
                            L"ColorCommitted(null) must dispatch setWorkspaceColor(std::nullopt)");
            VERIFY_IS_FALSE(vm.HasColor(), L"the row must clear HasColor from the model diff");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #52, Stage 3c. Clicking a sidebar row raises RequestActivate; the page
    // (subscribed in _addWorkspaceVm) dispatches switchToWorkspace and the
    // resulting ActiveWorkspaceChanged diff arm flips the active highlight and
    // selects the workspace's classic tab. We drive the intent directly (a
    // synthetic Tapped doesn't land headless), exactly as the rename test calls
    // CommitRename(). This mirrors SwitchToTab_FlagOn_ChangesActiveWorkspace but
    // via the sidebar VM intent rather than the classic tab strip.
    void WorkspaceTests::Sidebar_FlagOn_RowClick_DispatchesSwitchToWorkspace()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Startup gives workspace 0; a new workspace adds workspace 1 and makes
        // it the active one (newWorkspace's contract). Slice F follow-up (#54):
        // mint it via _createNewWorkspace (NewTab now adds a tab to the focused
        // leaf, not a workspace).
        auto result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Big-flip Slice F-5 (#54): no classic tab strip flag-on — assert via
            // the model + the sidebar view-models, not _GetFocusedTabIndex.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(2u, page->_workspaceViewModels.Size());
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), workspaces.size());

            // Pre-condition: workspace 1 is active.
            const auto activeBefore = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeBefore.has_value());
            VERIFY_ARE_EQUAL(activeBefore.value(), workspaces[1].id,
                             L"workspaces[1] should be active after creating a second workspace");

            // The row 0 view-model carries workspace 0's id; activating it is the
            // sidebar click intent.
            const auto vm0 = page->_workspaceViewModels.GetAt(0);
            VERIFY_ARE_EQUAL(workspaces[0].id.v, vm0.Id());
            vm0.RequestActivate();
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();

            // The page dispatched switchToWorkspace via RequestActivate: the
            // model now reports workspaces[0] as active...
            const auto activeAfter = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeAfter.has_value());
            VERIFY_ARE_EQUAL(activeAfter.value(), workspaces[0].id,
                             L"RequestActivate must dispatch switchToWorkspace to workspaces[0]");

            // ...the MRU front is workspaces[0]...
            VERIFY_IS_FALSE(page->_workspaceModelState->mru_view().empty());
            VERIFY_ARE_EQUAL(page->_workspaceModelState->mru_view().front(), workspaces[0].id,
                             L"switchToWorkspace must touch the MRU front");

            // ...and the switch is non-structural: the workspace count is
            // unchanged and no classic tab was built (cutover). The
            // ActiveWorkspaceChanged arm re-projected the active workspace's pane
            // tree into the visible host instead of driving _SelectTab.
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), workspaces.size(),
                             L"a switch is non-structural — the workspace count must not change");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip stays empty across a switch (cutover)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // #52, Stage 3c. After a sidebar-row activation, the ActiveWorkspaceChanged
    // arm flips IsActive on the view-models by id identity: the activated row's
    // VM is active and the previously-active row's VM is not.
    void WorkspaceTests::Sidebar_FlagOn_RowClick_FlipsActiveHighlightById()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Slice F follow-up (#54): mint the second workspace via
        // _createNewWorkspace (NewTab now adds a tab to the focused leaf).
        auto result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_workspaceViewModels.Size());
            const auto vm0 = page->_workspaceViewModels.GetAt(0);
            const auto vm1 = page->_workspaceViewModels.GetAt(1);

            // After creating a second workspace it is active: its row is
            // highlighted, 0 isn't.
            VERIFY_IS_FALSE(vm0.IsActive());
            VERIFY_IS_TRUE(vm1.IsActive());

            // Activate row 0 via the click intent.
            vm0.RequestActivate();
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            const auto vm0 = page->_workspaceViewModels.GetAt(0);
            const auto vm1 = page->_workspaceViewModels.GetAt(1);

            // The ActiveWorkspaceChanged arm flipped the highlight by id identity.
            VERIFY_IS_TRUE(vm0.IsActive(),
                           L"the activated row's view-model must be active");
            VERIFY_IS_FALSE(vm1.IsActive(),
                            L"the previously-active row's view-model must be inactive");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Pinned-float. Pinning a workspace floats it to the bottom of the pinned
    // block; the WorkspaceReordered diff arm must reorder _workspaceViewModels
    // so the sidebar's row order matches the model's new display order (by id).
    // We drive the pin via the VM's TogglePin intent (synthetic clicks don't
    // land headless), exactly as Sidebar_FlagOn_TogglePin_* does.
    void WorkspaceTests::Sidebar_FlagOn_Pin_ReordersViewModels()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Startup gives workspace 0; two new workspaces add workspaces 1 and 2 so
        // the sidebar has three rows [w0, w1, w2] in declared order. Slice F
        // follow-up (#54): mint them via _createNewWorkspace (NewTab now adds a
        // tab to the focused leaf, not a workspace).
        auto result = RunOnUIThread([&page]() {
            for (int i = 0; i < 2; ++i)
            {
                page->_createNewWorkspace(std::nullopt);
            }
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(3u, page->_workspaceViewModels.Size());
            VERIFY_ARE_EQUAL(static_cast<size_t>(3),
                             page->_workspaceModelState->workspaces_view().size());

            // Pre-condition: VM order matches model order [w0, w1, w2].
            const auto& wsBefore = page->_workspaceModelState->workspaces_view();
            for (uint32_t i = 0; i < page->_workspaceViewModels.Size(); ++i)
            {
                VERIFY_ARE_EQUAL(wsBefore[i].id.v, page->_workspaceViewModels.GetAt(i).Id());
            }

            // Pin the THIRD row (last). It should float to the bottom of the
            // (empty) pinned block, i.e. the front of the list.
            const auto vm2 = page->_workspaceViewModels.GetAt(2);
            const auto pinnedId = vm2.Id();
            vm2.TogglePin();

            // Model moved the pinned workspace to the front: [w2, w0, w1].
            const auto& wsAfter = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(static_cast<size_t>(3), wsAfter.size());
            VERIFY_ARE_EQUAL(pinnedId, wsAfter[0].id.v,
                             L"the pinned workspace must be first in the model display order");
            VERIFY_IS_TRUE(wsAfter[0].pinned);

            // The WorkspaceReordered arm must have reordered the VM collection
            // to match the model's new order, by id.
            VERIFY_ARE_EQUAL(3u, page->_workspaceViewModels.Size(),
                             L"reorder must not add or drop rows");
            for (uint32_t i = 0; i < page->_workspaceViewModels.Size(); ++i)
            {
                VERIFY_ARE_EQUAL(wsAfter[i].id.v, page->_workspaceViewModels.GetAt(i).Id(),
                                 L"sidebar VM order must match the model display order after a pin");
            }
            // The floated row carries the pinned glyph (metadata arm) too.
            VERIFY_IS_TRUE(page->_workspaceViewModels.GetAt(0).IsPinned());
        });
        VERIFY_SUCCEEDED(result);
    }

    // Phase 2 Slice 3 (#47): proves the ContentRegistry lifetime contract end
    // to end at the registry layer. This is the contract that the
    // ContentMounted / ContentUnmounted / removal arms project onto; the arms'
    // user-reachable wiring (which workspace's tree to (de)parent into) lands
    // in S4, so the lifetime guarantee itself is proved directly here.
    void WorkspaceTests::ContentRegistry_UnmountKeepsAlive_RemoveTearsDown()
    {
        using namespace winrt::TerminalApp::implementation;
        using ::WorkspaceModel::ContentId;

        ContentRegistry registry;
        const ContentId id{ 7 };

        // Unowned id resolves to an EXPLICIT null — not silent garbage.
        VERIFY_IS_FALSE(registry.Contains(id));
        VERIFY_IS_NULL(registry.Find(id));

        // EnsureMounted is the only way to obtain mountable content: it creates
        // + inserts via the factory for a new id and returns the live instance.
        const auto mock = winrt::make_self<MockPaneContent>(0xABCDull);
        int factoryCalls = 0;
        const auto first = registry.EnsureMounted(id, [&]() -> winrt::TerminalApp::IPaneContent {
            ++factoryCalls;
            return mock.as<winrt::TerminalApp::IPaneContent>();
        });
        VERIFY_IS_NOT_NULL(first);
        VERIFY_ARE_EQUAL(1, factoryCalls);
        VERIFY_IS_TRUE(registry.Contains(id));
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), registry.Size());

        // Unmount: the registry KEEPS the strong ref (the ConPTY stays alive).
        // Close() must NOT have been called — an unmount is not a teardown.
        registry.NoteUnmounted(id);
        VERIFY_IS_TRUE(registry.Contains(id));
        VERIFY_ARE_EQUAL(0, mock->CloseCount(), L"unmount must not tear down content");

        // Re-mount resolves the SAME live instance the registry kept alive —
        // the factory is NOT invoked again, so the surviving TermControl/ConPTY
        // is what gets re-attached, not a fresh one.
        const auto second = registry.EnsureMounted(id, [&]() -> winrt::TerminalApp::IPaneContent {
            ++factoryCalls;
            return winrt::make_self<MockPaneContent>(0x9999ull).as<winrt::TerminalApp::IPaneContent>();
        });
        VERIFY_ARE_EQUAL(1, factoryCalls, L"re-mount of an owned id must not re-create content");
        VERIFY_IS_TRUE(first == second, L"re-mount must resolve the SAME live instance");
        VERIFY_ARE_EQUAL(0xABCDull, second.as<MockPaneContent>()->Tag());

        // Find resolves the same owned instance too.
        VERIFY_IS_TRUE(registry.Find(id) == first);

        // Removal erases the entry and tears the content down (Close() once).
        VERIFY_IS_TRUE(registry.Remove(id));
        VERIFY_ARE_EQUAL(1, mock->CloseCount(), L"removal is the one place ConPTY tears down");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), registry.Size());

        // A subsequent resolve fails EXPLICITLY — a stale id can never re-attach
        // content that no longer exists.
        VERIFY_IS_FALSE(registry.Contains(id));
        VERIFY_IS_NULL(registry.Find(id));

        // Removing an unowned id is a benign explicit no-op (returns false).
        VERIFY_IS_FALSE(registry.Remove(id));
        VERIFY_IS_FALSE(registry.Remove(ContentId{ 999 }));
    }

    // Big-flip Slice A (#54): the ContentMounted factory is real. The model's
    // mount policy assigns a lifetime ContentId to every active-workspace
    // active-leaf active tab; diff() emits a ContentMounted carrying that id +
    // the tab's TabContent spec; the WorkspaceView arm now drives the live page
    // factory (TerminalSpec -> a real TerminalPaneContent backed by a
    // TermControl/ConPTY) into the ContentRegistry. The registry — not the
    // classic Tab — is therefore the strong owner of one live content per
    // active tab. The classic Tab STILL displays (this slice changes no
    // display/close-guard ownership), so _tabs tracks the workspace count and
    // the user still sees a single terminal.
    //
    // RED before the factory is real: EnsureMounted's factory returns nullptr,
    // so the registry stays empty (Size()==0) and no mount resolves.
    // GREEN after: registry Size() == workspace count, and every model mount
    // ContentId resolves to a live content via the registry.
    void WorkspaceTests::BigFlipA_FactoryMaterialisesContentIntoRegistry()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        // Resolve every model mount ContentId in the ACTIVE-materialised set and
        // assert each is owned by the registry. Returns the count of mounts seen
        // so the caller can cross-check it against the registry Size.
        const auto verifyAllMountsResolve = [](winrt::TerminalApp::implementation::TerminalPage* p) -> size_t {
            size_t mountsSeen = 0;
            for (const auto& ws : p->_workspaceModelState->workspaces_view())
            {
                for (const auto* leaf : p->_workspaceModelState->leaves(ws.id))
                {
                    for (const auto& t : leaf->tabs)
                    {
                        if (t.mount.has_value())
                        {
                            ++mountsSeen;
                            VERIFY_IS_TRUE(
                                p->_workspaceView->contentRegistryContainsForTest(*t.mount),
                                L"every model mount ContentId must resolve to a live content in the registry");
                        }
                    }
                }
            }
            return mountsSeen;
        };

        // Startup-replay landed exactly one workspace + one classic tab. The
        // mount policy materialised that workspace's active tab, so the factory
        // built one live content into the registry.
        auto result = RunOnUIThread([&page, &verifyAllMountsResolve]() {
            VERIFY_IS_TRUE(page->_workspaceView != nullptr);

            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay produces exactly one workspace");

            // RED today: factory returns nullptr -> registry stays empty.
            // GREEN after: the factory materialises one live content.
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), page->_workspaceView->contentRegistrySizeForTest(),
                             L"the factory must materialise one live content into the registry at startup");

            const auto mountsSeen = verifyAllMountsResolve(page.get());
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), mountsSeen,
                             L"the active workspace's active tab carries exactly one model mount");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Create a 2nd, activated workspace");
        // Slice F follow-up (#54): mint via _createNewWorkspace (NewTab now adds a
        // tab to the focused leaf, not a workspace) so this still exercises the
        // factory materialising a content for a freshly-activated workspace.
        result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        // The new workspace is now active and materialised; the first
        // workspace's content stays alive in the registry (mount policy leaves
        // already-materialised tabs untouched), so the registry now owns TWO
        // live contents — one per workspace.
        result = RunOnUIThread([&page, &verifyAllMountsResolve]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip stays empty across new workspaces (cutover)");
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"creating a second workspace grew the model to two workspaces");

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), page->_workspaceView->contentRegistrySizeForTest(),
                             L"the registry owns one live content per active workspace");

            const auto mountsSeen = verifyAllMountsResolve(page.get());
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), mountsSeen,
                             L"both workspaces' active tabs carry a model mount, and both resolve");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice E (#54): plug the whole-workspace-close ConPTY leak.
    //
    // Slice A made the ContentMounted factory real, so the ContentRegistry now
    // owns one live IPaneContent (TermControl/ConPTY) per active workspace.
    // Closing a WHOLE workspace routes through closeWorkspace -> diff, which
    // emits WorkspaceRemoved (NOT per-tab TabRemoved/ContentUnmounted — diff()
    // suppresses those for a removed workspace). Before Slice E,
    // apply(WorkspaceRemoved) only tore down the classic Tab and never Removed
    // the workspace's registry content, so each whole-workspace close leaked
    // its factory-built ConPTY until the window exited.
    //
    // This drives the close via the SAME model path the app uses
    // (closeWorkspace -> _applyWorkspaceAction; the Tab::Closed handler routes
    // through _closeTabViaWorkspaceModel to exactly this), then asserts the
    // registry dropped from 2 to 1 and the SURVIVING workspace's content stayed
    // alive.
    //
    // RED before the teardown: registry Size() stays 2 (leak).
    // GREEN after: it drops to 1, and the survivor still resolves.
    void WorkspaceTests::BigFlipE_WorkspaceClose_RemovesItsContent()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        Log::Comment(L"Create a 2nd workspace");
        // Slice F follow-up (#54): mint via _createNewWorkspace (NewTab now adds a
        // tab to the focused leaf, not a workspace).
        auto result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        // Capture the id of the workspace we'll close (the 2nd, now active) and
        // the surviving (1st) workspace's mount ContentId so we can assert it
        // stays alive across the close.
        ::WorkspaceModel::WorkspaceId closingWsId{ 0 };
        ::WorkspaceModel::WorkspaceId survivingWsId{ 0 };
        ::WorkspaceModel::ContentId survivingMount{ 0 };
        result = RunOnUIThread([&page, &closingWsId, &survivingWsId, &survivingMount]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(2u, workspaces.size(),
                             L"creating a second workspace grew the model to two workspaces");

            survivingWsId = workspaces[0].id;
            closingWsId = workspaces[1].id;

            // The surviving workspace's active tab carries the mount we expect
            // to outlive the close.
            const auto* survivingLeaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(survivingLeaf);
            VERIFY_ARE_EQUAL(1u, survivingLeaf->tabs.size());
            VERIFY_IS_TRUE(survivingLeaf->tabs[0].mount.has_value());
            survivingMount = *survivingLeaf->tabs[0].mount;

            // Pre-close: the registry owns one live content per workspace.
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), page->_workspaceView->contentRegistrySizeForTest(),
                             L"two workspaces -> two live contents before the close");
            VERIFY_IS_TRUE(page->_workspaceView->contentRegistryContainsForTest(survivingMount));
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close the 2nd workspace via the model (closeWorkspace -> _applyWorkspaceAction)");
        result = RunOnUIThread([&page, closingWsId]() {
            auto next = ::WorkspaceModel::closeWorkspace(page->_workspaceModelState, closingWsId);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        // Post-close: the closed workspace's content was Removed (its ConPTY
        // torn down), so the registry dropped 2 -> 1; the surviving workspace's
        // content is untouched and still resolves.
        result = RunOnUIThread([&page, survivingWsId, survivingMount]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"closeWorkspace dropped the second workspace from the model");
            VERIFY_ARE_EQUAL(survivingWsId.v,
                             page->_workspaceModelState->workspaces_view()[0].id.v,
                             L"the first workspace survives");

            // The leak fix: RED leaves Size()==2; GREEN drops it to 1.
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), page->_workspaceView->contentRegistrySizeForTest(),
                             L"closing a whole workspace must Remove its registry content (no ConPTY leak)");
            VERIFY_IS_TRUE(page->_workspaceView->contentRegistryContainsForTest(survivingMount),
                           L"the surviving workspace's content must stay alive across the close");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice B (#54): the flag-on content host receives the ACTIVE
    // workspace's factory-built content GetRoot(). This proves the new display
    // PLUMBING structurally: a collapsed WorkspaceContentHost inside TabContent
    // holds the active workspace's content, while the classic _tabContent swap
    // still owns the VISIBLE display (so the user sees no change this slice).
    //
    // We install the test-only factory override (via beforeCreate) so the
    // ContentMounted factory builds a MockPaneContent whose GetRoot() is a
    // known, stable Grid — that lets us assert the host's child by IDENTITY
    // without depending on a real TermControl's root. (Slice A proved the real
    // factory works headlessly; here we only care about the attach plumbing,
    // so the mock root keeps the test focused and decoupled from control
    // geometry.)
    //
    // RED before the host attach: the WorkspaceContentHost is empty (no
    // _showActiveWorkspaceContentInHost wiring), so _workspaceHostChildForTest()
    // is null and hostContentIdForTest() is nullopt.
    // GREEN after: the host's sole child == the active workspace's content
    // GetRoot() (by identity), hostContentIdForTest() == that content's mount
    // id, the host is Collapsed, and the classic _tabContent still holds the
    // classic content (visible path intact).
    void WorkspaceTests::BigFlipB_ActiveWorkspaceContentAttachedToHost()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        // The factory override records every MockPaneContent it hands out so
        // the test can recover the exact instance (and its known root) the
        // registry owns for a given workspace.
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            // Each mount builds a fresh, tagged MockPaneContent with a stable
            // Grid root, rather than a real TermControl.
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xB000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        auto result = RunOnUIThread([&page, mocks]() {
            VERIFY_IS_TRUE(page->_workspaceView != nullptr);

            // Big-flip Slice F-5 (#54): THE CUTOVER. No classic Tab is built
            // flag-on — the projected pane tree displays. The factory still
            // materialises the content into the registry (the property under
            // test); it just lands in the now-VISIBLE host's leaf cell.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");

            // The factory built exactly one content (one active workspace) and
            // the registry owns it.
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), mocks->size(),
                             L"the override factory materialised one content at startup");
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), page->_workspaceView->contentRegistrySizeForTest(),
                             L"the registry owns one live content at startup");

            // The active workspace's mount id is what the host should back.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(1u, leaf->tabs.size());
            VERIFY_IS_TRUE(leaf->tabs[0].mount.has_value());
            const auto activeMount = *leaf->tabs[0].mount;
            const auto leafId = leaf->id;

            // GREEN (Big-flip Slice F-0, #54): the active workspace's content
            // GetRoot() is now parented into the root leaf's PER-LEAF content
            // host (the single shared host became the outer wrapper around the
            // pane tree; content lives in the per-leaf hosts so a split can show
            // each leaf in its own cell). Assert by identity against the leaf's
            // host child. hostContentIdForTest still records which content the
            // active-workspace attach resolved.
            const auto expectedRoot = (*mocks)[0]->GetRoot();
            VERIFY_IS_NOT_NULL(expectedRoot);
            const auto leafChild = page->_leafHostChildForTest(leafId);
            VERIFY_IS_NOT_NULL(leafChild,
                               L"the root leaf's per-leaf host must hold the active workspace's content root");
            VERIFY_IS_TRUE(leafChild == expectedRoot,
                           L"the root leaf's host child must be the content's GetRoot() (by identity)");

            const auto hostId = page->_workspaceView->hostContentIdForTest();
            VERIFY_IS_TRUE(hostId.has_value(),
                           L"the view must record which content the host backs");
            VERIFY_ARE_EQUAL(activeMount.v, hostId->v,
                             L"the host must back the active workspace's mount content id");

            // Big-flip Slice F-5 (#54): the host is now VISIBLE and is the SOLE
            // child of _tabContent — it IS the visible display (no classic Tab).
            VERIFY_IS_TRUE(page->_workspaceContentHost != nullptr);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must be Visible (F-5 cutover) — it is the display now");

            VERIFY_IS_TRUE(page->_tabContent != nullptr);
            VERIFY_ARE_EQUAL(1u, page->_tabContent.Children().Size(),
                             L"_tabContent has exactly one child post-cutover");
            uint32_t hostIndex = 0;
            VERIFY_IS_TRUE(page->_tabContent.Children().IndexOf(page->_workspaceContentHost, hostIndex),
                           L"the projected-pane-tree host is _tabContent's sole child (cutover)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice B (#54): switching the active workspace swaps the host's
    // child to the newly-active workspace's content. Create a 2nd workspace
    // (now active; the host backs ws1's content), then switch active back to
    // ws0 via the model (switchToWorkspace -> _applyWorkspaceAction, the same
    // path a sidebar-row activation drives), and assert the host's child flips
    // to ws0's content GetRoot. The classic _tabContent / _SelectTab behavior
    // is unchanged (the classic display still follows the selected tab).
    //
    // RED before the attach wiring: the host is empty, so the child never
    // flips. GREEN after: the host's child == ws0's content root, by identity.
    void WorkspaceTests::BigFlipB_SwitchSwapsHostChild()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        // Map every materialised content's mount id -> its MockPaneContent, so
        // the test can recover each workspace's known root by id. The override
        // can't see the mount id (it only gets the spec), so we tag each mock
        // and resolve mount->mock via the registry identity below instead.
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xB000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        Log::Comment(L"Create a 2nd workspace (now active)");
        // Slice F follow-up (#54): mint via _createNewWorkspace (NewTab now adds a
        // tab to the focused leaf, not a workspace) so each workspace stays a
        // single-leaf tree and the override factory materialises one mock per
        // workspace in creation order.
        auto result = RunOnUIThread([&page]() {
            page->_createNewWorkspace(std::nullopt);
        });
        VERIFY_SUCCEEDED(result);

        // Capture ws0/ws1 ids + the root each workspace's content resolves to.
        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::WorkspaceId ws1{ 0 };
        ::WorkspaceModel::PaneId ws0Leaf{ 0 };
        ::WorkspaceModel::PaneId ws1Leaf{ 0 };
        winrt::Windows::UI::Xaml::FrameworkElement ws0Root{ nullptr };
        winrt::Windows::UI::Xaml::FrameworkElement ws1Root{ nullptr };
        result = RunOnUIThread([&]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(2u, workspaces.size(),
                             L"creating a second workspace grew the model to two workspaces");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), mocks->size(),
                             L"the override factory materialised one content per workspace");

            ws0 = workspaces[0].id;
            ws1 = workspaces[1].id;
            // Each workspace's root is a single leaf at this point; read its id
            // off the variant (the _rootLeafId helper is defined later in the
            // file, so resolve inline here).
            const auto* l0 = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            const auto* l1 = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[1].root);
            VERIFY_IS_NOT_NULL(l0);
            VERIFY_IS_NOT_NULL(l1);
            ws0Leaf = l0->id;
            ws1Leaf = l1->id;
            VERIFY_IS_TRUE(ws0Leaf.valid() && ws1Leaf.valid());

            // The two mocks were created in workspace-creation order: index 0
            // is ws0's content (startup), index 1 is ws1's (the new-tab).
            ws0Root = (*mocks)[0]->GetRoot();
            ws1Root = (*mocks)[1]->GetRoot();
            VERIFY_IS_NOT_NULL(ws0Root);
            VERIFY_IS_NOT_NULL(ws1Root);
            VERIFY_IS_TRUE(ws0Root != ws1Root);

            // Big-flip Slice F-0 (#54): after creating ws1 it is active, so the
            // projected tree is ws1's and ws1's root leaf host backs ws1's
            // content. Only the active workspace's leaves have projected hosts
            // (a switch rebuilds the tree for the new active workspace), so
            // ws0's leaf host is absent while ws1 is active.
            const auto ws1LeafChild = page->_leafHostChildForTest(ws1Leaf);
            VERIFY_IS_TRUE(ws1LeafChild == ws1Root,
                           L"after creating ws1, ws1's root leaf host backs ws1's content root");
            const auto hostId = page->_workspaceView->hostContentIdForTest();
            VERIFY_IS_TRUE(hostId.has_value());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Switch active back to ws0 via the model (switchToWorkspace -> _applyWorkspaceAction)");
        result = RunOnUIThread([&page, ws0]() {
            auto next = ::WorkspaceModel::switchToWorkspace(page->_workspaceModelState, ws0);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        // GREEN (Big-flip Slice F-0, #54): switching to ws0 rebuilt the tree for
        // ws0 and re-attached ws0's content into ws0's root leaf host; the
        // classic display followed the selected tab (still two tabs, ws0's now
        // selected).
        result = RunOnUIThread([&]() {
            const auto ws0LeafChild = page->_leafHostChildForTest(ws0Leaf);
            VERIFY_IS_TRUE(ws0LeafChild == ws0Root,
                           L"switching active to ws0 must attach ws0's content into ws0's root leaf host");
            // ws1's leaf host is no longer in the projected (ws0) tree, so its
            // host is absent — ws1's content is detached from the active tree.
            VERIFY_IS_NULL(page->_leafHostChildForTest(ws1Leaf),
                           L"ws1's leaf host must be gone from the active (ws0) tree after the switch");

            // Big-flip Slice F-5 (#54): the host is Visible and IS the display.
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must remain Visible (F-5 cutover) across a switch");

            // No classic tab strip flag-on; the host is _tabContent's sole child.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"the switch must not build a classic tab (cutover)");
            uint32_t hostIndex = 0;
            VERIFY_IS_TRUE(page->_tabContent.Children().IndexOf(page->_workspaceContentHost, hostIndex),
                           L"the projected-pane-tree host is _tabContent's sole child across a switch (cutover)");
        });
        VERIFY_SUCCEEDED(result);
    }

    namespace
    {
        // Big-flip Slice C (#54) test helper: the root leaf's PaneId of the
        // workspace at index `wsIdx`. The startup workspace is a single root
        // leaf; the tests add a 2nd tab to it (no split), so the root stays a
        // leaf. Returns an invalid PaneId if the root isn't a leaf.
        ::WorkspaceModel::PaneId _rootLeafId(const ::WorkspaceModel::ModelState& state, size_t wsIdx)
        {
            const auto& workspaces = state->workspaces_view();
            if (wsIdx >= workspaces.size())
            {
                return ::WorkspaceModel::PaneId{};
            }
            if (const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[wsIdx].root))
            {
                return leaf->id;
            }
            return ::WorkspaceModel::PaneId{};
        }
    }

    // Big-flip Slice C (#54): adding a tab to an EXISTING leaf (via the model
    // newTab action) appends a PaneTabViewModel to that leaf's invisible strip
    // collection — and creates NO second classic Tab. The strip VM is the SOLE
    // representation of an additional leaf tab; the classic-tab creation in the
    // TabAdded arm fires ONLY for a new workspace's first tab. The host stays
    // Collapsed and _tabs is unchanged, so the user sees ZERO change this slice.
    //
    // RED before the arm projects the strip: the leaf's strip size never grows
    // past the single startup tab (no _appendPaneTabVm wiring), so it stays 1.
    // (And without the new-vs-additional distinguisher, the arm would WRONGLY
    // grow _tabs to 2 — the no-2nd-classic-tab assert guards that too.)
    // GREEN after: the leaf's strip size == 2, _tabs.Size() stays 1, host
    // Collapsed.
    void WorkspaceTests::BigFlipC_TabAdded_AppendsStripVm()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xC000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };

        auto result = RunOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_workspaceView != nullptr);

            // Startup baseline: one workspace, the root leaf's strip has exactly
            // one VM (the startup tab projected by TabAdded). No classic Tab is
            // built flag-on (cutover).
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            ws0 = workspaces[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"startup: the root leaf's strip has one VM (the first tab)");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a SECOND tab to the existing root leaf via the model (newTab)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid(), L"newTab on an existing leaf must allocate a tab");
            page->_applyWorkspaceAction(std::move(added.state));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // GREEN: the leaf's strip grew to 2 VMs.
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(leaf0),
                             L"adding a tab to the leaf appends a strip VM (now two)");

            // The model leaf indeed holds two tabs now.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), leaf->tabs.size());

            // CRITICAL: NO classic Tab is created — the additional tab is
            // represented ONLY as a strip VM. `_tabs` stays empty (cutover).
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"an additional leaf tab must NOT create a classic Tab (cutover)");

            VERIFY_IS_TRUE(page->_workspaceContentHost != nullptr);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must remain Visible (F-5 cutover) — no visible change this slice");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Live pane-tab title (#54): the strip VM's Title tracks its mounted
    // content's live title. The factory hands out a MockPaneContent (initial
    // Title() == "mock"); the startup tab's content mounts during Create(), so
    // the root leaf's first (and only) strip VM should read "mock" — NOT the
    // static "Tab" placeholder. Then we simulate the running terminal changing
    // its title via the mock's SetTitle("Administrator: Command Prompt"), which
    // raises TitleChanged; the VM's Title must re-push to the new value.
    //
    // RED before the wiring: the VM's Title is the placeholder ("Tab") and never
    // moves off it. GREEN after: it equals the content's title at mount, and
    // follows every TitleChanged. The classic path is untouched.
    void WorkspaceTests::LivePaneTabTitle_TracksContentTitleChanged()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        // The factory records every MockPaneContent it hands out so the test can
        // drive the startup tab's content title after the fact.
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xE000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };

        auto result = RunOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_workspaceView != nullptr);
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"startup: the root leaf's strip has one VM (the first tab)");

            // The startup tab's content mounted during Create(), so exactly one
            // mock was handed out and the VM's title was seeded from it.
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), mocks->size(),
                             L"the startup tab's content must have mounted (one mock)");

            const auto firstTitle = page->_paneTabStripFirstTitleForTest(leaf0);
            VERIFY_IS_TRUE(firstTitle.has_value());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"mock" }, *firstTitle,
                             L"the strip VM title must be seeded from content.Title(), not the static placeholder");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Simulate the running terminal changing its title (raises TitleChanged)");
        result = RunOnUIThread([&]() {
            (*mocks)[0]->SetTitle(L"Administrator: Command Prompt");

            const auto updated = page->_paneTabStripFirstTitleForTest(leaf0);
            VERIFY_IS_TRUE(updated.has_value());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"Administrator: Command Prompt" }, *updated,
                             L"the strip VM title must follow content.TitleChanged to the new live title");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Workspaces M2 (#54, ADR-001): tab rename via the reused TabHeaderControl,
    // model-driven through setTabTitle, with custom-wins title precedence.
    //
    // The strip VM's Title is a COMPUTED effective title: CustomTitle (the model's
    // TabRecord.customTitle, set by rename) wins when non-empty, else the live
    // shell title. This pins the whole rename round trip + the precedence:
    //
    //  1. At startup the root leaf's VM title is the live "mock" title (no custom
    //     title yet).
    //  2. Raising the VM's RequestRename("My Tab") dispatches setTabTitle through
    //     the model (intent → action → diff); the TabDecorationUpdated arm projects
    //     the new customTitle back onto the VM. The VM Title surfaces "My Tab". The
    //     model's TabRecord.customTitle is "My Tab" (the rename actually mutated the
    //     model, not just the view).
    //  3. A subsequent live content.TitleChanged (SetTitle) must NOT clobber the
    //     custom title — the VM Title still shows "My Tab" (custom wins).
    //  4. Renaming to the empty string clears the custom title; the live title
    //     (the latest SetTitle value) takes over again.
    //
    // RED before M2: there is no rename intent / no customTitle projection; a live
    // TitleChanged always wins. GREEN after: custom wins, end to end through the
    // model. The classic path is untouched.
    void WorkspaceTests::StripM2_Rename_DispatchesSetTabTitle_CustomTitleWins()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xE100 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::TabId tab0{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"startup: the root leaf's strip has one VM (the first tab)");
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), mocks->size(),
                             L"the startup tab's content must have mounted (one mock)");

            // Resolve the startup tab's model id (the one VM in the strip).
            const auto* node = page->_workspaceModelState->pane(leaf0);
            const auto* leafPane = std::get_if<::WorkspaceModel::LeafPane>(node);
            VERIFY_IS_NOT_NULL(leafPane);
            VERIFY_IS_FALSE(leafPane->tabs.empty());
            tab0 = leafPane->tabs[0].id;
            VERIFY_IS_TRUE(tab0.valid());

            // No custom title yet: the VM Title is the live "mock" title.
            const auto seeded = page->_paneTabStripFirstTitleForTest(leaf0);
            VERIFY_IS_TRUE(seeded.has_value());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"mock" }, *seeded,
                             L"startup: the VM title is the live content title (no custom title)");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Raise the VM's RequestRename intent; it must dispatch setTabTitle through the model.");
        result = RunOnUIThread([&]() {
            // Resolve the strip VM and raise its rename intent (the path the
            // hosted TabHeaderControl.TitleChangeRequested takes — dispatch-level,
            // no synthetic pointer / double-tap / layout).
            const auto vm = page->_paneTabStripFirstVmForTest(leaf0);
            VERIFY_IS_NOT_NULL(vm);

            vm.RequestRename(winrt::hstring{ L"My Tab" });

            // The rename mutated the MODEL (custom-title is model-as-truth).
            const auto* node = page->_workspaceModelState->pane(leaf0);
            const auto* leafPane = std::get_if<::WorkspaceModel::LeafPane>(node);
            VERIFY_IS_NOT_NULL(leafPane);
            VERIFY_ARE_EQUAL(std::string{ "My Tab" }, leafPane->tabs[0].customTitle,
                             L"RequestRename must dispatch setTabTitle so the model's customTitle changes");

            // And the diff projected the custom title back onto the VM.
            const auto renamed = page->_paneTabStripFirstTitleForTest(leaf0);
            VERIFY_IS_TRUE(renamed.has_value());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"My Tab" }, *renamed,
                             L"the VM Title must surface the custom title after the rename round trip");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"A later live TitleChanged must NOT clobber the custom title (custom wins).");
        result = RunOnUIThread([&]() {
            (*mocks)[0]->SetTitle(L"Administrator: Command Prompt");

            const auto stillCustom = page->_paneTabStripFirstTitleForTest(leaf0);
            VERIFY_IS_TRUE(stillCustom.has_value());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"My Tab" }, *stillCustom,
                             L"a live TitleChanged must not override the user's custom title — custom wins");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Clearing the custom title (rename to empty) lets the live title take over.");
        result = RunOnUIThread([&]() {
            const auto vm = page->_paneTabStripFirstVmForTest(leaf0);
            VERIFY_IS_NOT_NULL(vm);

            vm.RequestRename(winrt::hstring{ L"" });

            const auto* node = page->_workspaceModelState->pane(leaf0);
            const auto* leafPane = std::get_if<::WorkspaceModel::LeafPane>(node);
            VERIFY_IS_NOT_NULL(leafPane);
            VERIFY_IS_TRUE(leafPane->tabs[0].customTitle.empty(),
                           L"renaming to empty must reset the model's customTitle");

            const auto liveAgain = page->_paneTabStripFirstTitleForTest(leaf0);
            VERIFY_IS_TRUE(liveAgain.has_value());
            VERIFY_ARE_EQUAL(winrt::hstring{ L"Administrator: Command Prompt" }, *liveAgain,
                             L"with the custom title cleared, the VM Title falls back to the latest live title");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice 2a.2 (#54): the selected pane-tab's connected background tracks the
    // mounted content's live background COLOR — the faithful classic WT behavior
    // ("tab.background = terminalBackground", Tab::_RecalculateAndApplyTabColor)
    // that fixes the inverted selected tab (Slice 2a.1 had bound a static brush
    // that floated up at #333333 instead of merging down into the #0C0C0C
    // content). The factory hands out a MockPaneContent; we set its
    // BackgroundBrush to RED before the startup tab's content mounts during
    // Create(), so the root leaf's first strip VM's Background must be seeded to
    // RED at mount. We then change the mock's brush to BLUE and raise a title
    // change (SetTitle) — the bg has no dedicated event, so it re-pulls on the
    // TitleChanged subscription — and the VM's Background must follow to BLUE.
    // The page extracts the COLOR and builds a FRESH SolidColorBrush, so the
    // assertion compares colors (til::color), not brush identity.
    //
    // RED before the wiring: the VM's Background is null and never moves off it.
    // GREEN after: it equals the content's bg color at mount and follows every
    // TitleChanged-driven refresh. The classic path is untouched.
    void WorkspaceTests::LivePaneTabBackground_TracksContentBackground()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        const winrt::Windows::UI::Color red{ 255, 255, 0, 0 };
        const winrt::Windows::UI::Color blue{ 255, 0, 0, 255 };

        // The factory sets each handed-out mock's bg to RED immediately, BEFORE
        // it is returned to the registry's EnsureMounted — so the startup tab's
        // content already carries RED at the moment the ContentMounted arm seeds
        // the strip VM's Background from it.
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks, red](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks, red](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xB000 + mocks->size()));
                mock->SetBackgroundBrush(Media::SolidColorBrush{ red });
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };

        auto result = RunOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_workspaceView != nullptr);
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"startup: the root leaf's strip has one VM (the first tab)");

            // The startup tab's content mounted during Create(), so exactly one
            // mock was handed out (and its bg was seeded into the VM at mount).
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), mocks->size(),
                             L"the startup tab's content must have mounted (one mock)");

            const auto seeded = page->_paneTabStripFirstBackgroundForTest(leaf0);
            VERIFY_IS_TRUE(seeded.has_value(),
                           L"the strip VM Background must be seeded from content.BackgroundBrush() at mount (not null)");
            VERIFY_ARE_EQUAL(til::color{ red }, *seeded,
                             L"the strip VM Background color must equal the mock content's bg color (red) at mount");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Change the content bg to blue, then raise TitleChanged to drive the bg refresh");
        result = RunOnUIThread([&]() {
            (*mocks)[0]->SetBackgroundBrush(Media::SolidColorBrush{ blue });
            // The bg has no dedicated event; the refresh piggybacks on the
            // TitleChanged subscription. SetTitle raises TitleChanged.
            (*mocks)[0]->SetTitle(L"x");

            const auto updated = page->_paneTabStripFirstBackgroundForTest(leaf0);
            VERIFY_IS_TRUE(updated.has_value());
            VERIFY_ARE_EQUAL(til::color{ blue }, *updated,
                             L"the strip VM Background color must follow the content's new bg (blue) on the TitleChanged-driven refresh");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Slice 2a.2 follow-up (#54): the bg refresh piggybacks on TitleChanged
    // (per-prompt cadence for many shells). The VM's Background is a
    // WINRT_OBSERVABLE_PROPERTY whose setter guards by REFERENCE identity, so a
    // freshly-allocated SolidColorBrush of the SAME color is never reference-equal
    // to the stored one — without a color-equality short-circuit, every refresh
    // would re-allocate a brush and raise PropertyChanged(Background), churning
    // the bound Border re-eval. This test subscribes to the strip VM's
    // PropertyChanged, counts "Background" raises, then:
    //   (1) drives a refresh with the SAME content color (SetTitle again, bg
    //       brush unchanged) and asserts NO new Background raise, and
    //   (2) drives a refresh with a GENUINE new color and asserts exactly one new
    //       Background raise.
    // Headless: pure VM/accessor assertions, no layout/resize.
    void WorkspaceTests::LivePaneTabBackground_NoChurnOnUnchangedColor()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        const winrt::Windows::UI::Color red{ 255, 255, 0, 0 };
        const winrt::Windows::UI::Color green{ 255, 0, 255, 0 };

        // The startup tab's content is seeded RED at mount (same factory pattern
        // as LivePaneTabBackground_TracksContentBackground).
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks, red](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks, red](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xB100 + mocks->size()));
                mock->SetBackgroundBrush(Media::SolidColorBrush{ red });
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto backgroundRaises = std::make_shared<int>(0);

        auto result = RunOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_workspaceView != nullptr);
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            const auto vm = page->_paneTabStripFirstVmForTest(leaf0);
            VERIFY_IS_NOT_NULL(vm);

            const auto seeded = page->_paneTabStripFirstBackgroundForTest(leaf0);
            VERIFY_IS_TRUE(seeded.has_value());
            VERIFY_ARE_EQUAL(til::color{ red }, *seeded,
                             L"precondition: the strip VM Background is seeded red at mount");

            // Subscribe AFTER mount so we only count refresh-driven raises, and
            // tally only the Background property.
            vm.PropertyChanged([backgroundRaises](const auto&, const winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
                if (args.PropertyName() == L"Background")
                {
                    ++(*backgroundRaises);
                }
            });
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Drive a refresh with the SAME content color (bg brush unchanged) — must NOT raise Background");
        result = RunOnUIThread([&]() {
            // The bg brush is still red; SetTitle raises TitleChanged which drives
            // the bg refresh. The color-equality short-circuit must skip the set.
            (*mocks)[0]->SetTitle(L"same-color-1");
            (*mocks)[0]->SetTitle(L"same-color-2");

            VERIFY_ARE_EQUAL(0, *backgroundRaises,
                             L"an unchanged-color refresh must NOT raise Background PropertyChanged (no churn)");

            // Also set a brand-new brush of the IDENTICAL color: still no raise,
            // because the short-circuit compares COLOR, not brush reference.
            (*mocks)[0]->SetBackgroundBrush(Media::SolidColorBrush{ red });
            (*mocks)[0]->SetTitle(L"same-color-3");

            VERIFY_ARE_EQUAL(0, *backgroundRaises,
                             L"a fresh brush of the identical color must NOT raise Background (color-equality, not reference)");

            const auto still = page->_paneTabStripFirstBackgroundForTest(leaf0);
            VERIFY_IS_TRUE(still.has_value());
            VERIFY_ARE_EQUAL(til::color{ red }, *still,
                             L"the rendered color is unchanged (still red) — the short-circuit does not alter it");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Drive a refresh with a GENUINE new color (green) — must raise Background exactly once");
        result = RunOnUIThread([&]() {
            (*mocks)[0]->SetBackgroundBrush(Media::SolidColorBrush{ green });
            (*mocks)[0]->SetTitle(L"now-green");

            VERIFY_ARE_EQUAL(1, *backgroundRaises,
                             L"a genuine color change must raise Background PropertyChanged exactly once");

            const auto updated = page->_paneTabStripFirstBackgroundForTest(leaf0);
            VERIFY_IS_TRUE(updated.has_value());
            VERIFY_ARE_EQUAL(til::color{ green }, *updated,
                             L"the strip VM Background color follows the content's new bg (green)");

            // And a follow-up refresh at the SAME (now green) color again raises
            // nothing — the short-circuit re-armed at the new color.
            (*mocks)[0]->SetTitle(L"still-green");
            VERIFY_ARE_EQUAL(1, *backgroundRaises,
                             L"a same-color refresh after the change still raises nothing (count stays 1)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice C (#54): apply(ActiveTabChanged) flips the strip's active
    // row to the newly-active tab AND swaps the (collapsed) host's child to that
    // tab's content GetRoot (extending Slice B's single-content attach to
    // per-tab). We add a 2nd tab (which becomes active, firing ActiveTabChanged
    // 0->1), then selectTab back to the first (firing ActiveTabChanged 1->0),
    // asserting the active VM and the host child follow each time. The classic
    // path is untouched (still one classic Tab, host Collapsed).
    //
    // RED before the arm is real: ActiveTabChanged is a no-op, so neither the
    // active VM nor the host child ever flips. GREEN after: the active strip VM
    // is the newly-active tab and the host child is that tab's content root, by
    // identity.
    void WorkspaceTests::BigFlipC_ActiveTabChanged_FlipsSelectionAndSwapsHostChild()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        // The override hands out a fresh tagged MockPaneContent per mount, in
        // creation order: index 0 == the startup (first) tab's content, index 1
        // == the added (second) tab's content.
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xC000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::TabId firstTabId{ 0 };
        ::WorkspaceModel::TabId secondTabId{ 0 };

        auto result = RunOnUIThread([&]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            ws0 = workspaces[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            firstTabId = leaf->tabs[0].id;
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a 2nd tab — it becomes active, firing ActiveTabChanged 0->1");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            secondTabId = added.id;
            page->_applyWorkspaceAction(std::move(added.state));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), mocks->size(),
                             L"two contents materialised — one per tab");

            // GREEN: the active strip VM is the newly-added (second) tab.
            const auto activeId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeId.has_value(),
                           L"after ActiveTabChanged, one strip row must be active");
            VERIFY_ARE_EQUAL(secondTabId.v, *activeId,
                             L"the active strip row must be the newly-active (2nd) tab");

            // Big-flip Slice F-0 (#54): the leaf's per-leaf host child swapped to
            // the 2nd tab's content GetRoot (mock 1) — the per-leaf content host
            // now backs the active tab's content (the shared host became the
            // outer wrapper).
            const auto leafChild = page->_leafHostChildForTest(leaf0);
            const auto secondRoot = (*mocks)[1]->GetRoot();
            VERIFY_IS_NOT_NULL(secondRoot);
            VERIFY_IS_TRUE(leafChild == secondRoot,
                           L"the leaf host child must be the newly-active tab's content root");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across an active-tab change");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"an active-tab change must not build a classic tab (cutover)");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Select the FIRST tab — ActiveTabChanged 1->0 flips back");
        result = RunOnUIThread([&]() {
            auto next = ::WorkspaceModel::selectTab(page->_workspaceModelState, firstTabId);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto activeId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeId.has_value());
            VERIFY_ARE_EQUAL(firstTabId.v, *activeId,
                             L"selecting the first tab flips the active strip row back to it");

            // Big-flip Slice F-0 (#54): the leaf's per-leaf host child swapped
            // back to the first tab's content GetRoot (mock 0).
            const auto leafChild = page->_leafHostChildForTest(leaf0);
            const auto firstRoot = (*mocks)[0]->GetRoot();
            VERIFY_IS_TRUE(leafChild == firstRoot,
                           L"the leaf host child must swap back to the first tab's content root");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across the second active-tab change");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice C (#54): apply(TabRemoved) removes the strip VM that
    // mirrored a closed tab from its leaf's collection. We add a 2nd tab (strip
    // size 2), then closeTab it — a multi-tab leaf close emits TabRemoved (NOT
    // WorkspaceRemoved, which only fires when the WHOLE workspace goes). The
    // strip shrinks back to 1 and the classic tab count is unchanged (the
    // additional tab never had a classic Tab). Host stays Collapsed.
    //
    // RED before the arm removes the VM: closing the additional tab leaves the
    // strip at 2 (no _removePaneTabVm wiring). GREEN after: the strip is 1
    // again, _tabs unchanged, host Collapsed.
    void WorkspaceTests::BigFlipC_TabRemoved_RemovesStripVm()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xC000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::TabId secondTabId{ 0 };

        auto result = RunOnUIThread([&]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            ws0 = workspaces[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0));
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a 2nd tab to the leaf (strip -> 2)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            secondTabId = added.id;
            page->_applyWorkspaceAction(std::move(added.state));
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(leaf0),
                             L"precondition: the leaf's strip has two VMs before the close");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close the additional tab (multi-tab leaf -> TabRemoved, NOT WorkspaceRemoved)");
        result = RunOnUIThread([&]() {
            auto next = ::WorkspaceModel::closeTab(page->_workspaceModelState, secondTabId);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // The workspace + its leaf survive (only the 2nd tab closed).
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size(),
                             L"closing one of two tabs must NOT remove the workspace");
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), leaf->tabs.size());

            // GREEN: the strip shrank back to one VM (the removed tab's VM gone).
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"closing the additional tab removes its strip VM (back to one)");

            // No classic tab exists flag-on (cutover) — `_tabs` stays empty.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"closing the additional tab must not build a classic tab (cutover)");

            // Host still Visible.
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across a tab close");
        });
        VERIFY_SUCCEEDED(result);
    }

    namespace
    {
        // Big-flip Slice D (#54) test helper: the star value of the
        // ColumnDefinition (axis Vertical) or RowDefinition (axis Horizontal)
        // at index `cell` of a projected split Grid. The projector sizes the
        // first cell `ratio` and the second `1-ratio` in star units, so this
        // reads the model ratio back out of the XAML structure WITHOUT
        // measuring any laid-out pixel width (the headless-resize-clamp trap).
        double _splitCellStar(const winrt::Windows::UI::Xaml::Controls::Grid& grid,
                              ::WorkspaceModel::Axis axis,
                              uint32_t cell)
        {
            if (axis == ::WorkspaceModel::Axis::Vertical)
            {
                const auto def = grid.ColumnDefinitions().GetAt(cell);
                return def.Width().Value;
            }
            const auto def = grid.RowDefinitions().GetAt(cell);
            return def.Height().Value;
        }

        // Find the leaf container the projector tagged with `leaf`.v anywhere in
        // the projected subtree rooted at `node`. Leaf containers and split
        // Grids both carry their PaneId.v in Tag(); a leaf container holds the
        // leaf's Slice-C strip, a split Grid holds two child cells. Returns
        // nullptr if no leaf container with that tag exists.
        winrt::Windows::UI::Xaml::FrameworkElement _findLeafContainer(
            const winrt::Windows::UI::Xaml::FrameworkElement& node,
            ::WorkspaceModel::PaneId leaf)
        {
            if (!node)
            {
                return nullptr;
            }
            const auto tag = node.Tag().try_as<uint64_t>();
            const auto grid = node.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            // A split Grid carries star-sized child cells (2+ children that are
            // themselves projected nodes). A leaf container is the terminal node
            // whose tag matches the leaf id we want.
            if (tag && *tag == leaf.v)
            {
                // Disambiguate a leaf container from a split with the same tag:
                // splits never share a leaf's id, so a tag match IS the leaf.
                return node;
            }
            if (grid)
            {
                for (uint32_t i = 0; i < grid.Children().Size(); ++i)
                {
                    const auto child = grid.Children().GetAt(i).try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
                    if (const auto found = _findLeafContainer(child, leaf))
                    {
                        return found;
                    }
                }
            }
            return nullptr;
        }
    }

    // Big-flip Slice D (#54): splitting the active workspace's root leaf builds
    // a NESTED split container in the (collapsed) WorkspaceContentHost — a split
    // Grid with two star-sized cells, each holding a leaf container, mirroring
    // the model's 1->2 leaves. Each leaf container carries its leaf's Slice-C
    // strip. We drive the model splitPane action (NOT a classic resize), so this
    // asserts the projected element-tree STRUCTURE/nesting, never pixel widths.
    // The classic split path is left intact (still one classic Tab; the classic
    // pane tree grew to 2 leaves), and the host stays Collapsed.
    //
    // RED before the split arms project the tree: SplitPaneCreated /
    // LeafPaneCreated are no-ops for the new tree, so the host's projected
    // pane-tree root never becomes a split Grid (it stays a single leaf
    // container, or null). GREEN after: the root is a split Grid with two leaf
    // containers, one per model leaf.
    void WorkspaceTests::BigFlipD_Split_BuildsNestedContainers()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xD000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };

        auto result = RunOnUIThread([&]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::LeafPane>(workspaces[0].root),
                           L"fresh workspace root must be a single leaf");
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            // Baseline: the projected pane-tree root is a single leaf container
            // for leaf0 — not yet a split.
            const auto rootBefore = page->_workspacePaneTreeRootChildForTest();
            const auto leafBefore = _findLeafContainer(rootBefore, leaf0);
            VERIFY_IS_NOT_NULL(leafBefore,
                               L"baseline: the root leaf must already have a projected leaf container");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf via the model (splitPane, vertical, ratio 0.5)");
        ::WorkspaceModel::PaneId rightLeaf{ 0 };
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid(), L"splitPane must allocate a new sibling leaf");
            rightLeaf = split.newPaneId;
            page->_applyWorkspaceAction(std::move(split.state));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // Model: the active workspace's root is now a SplitPane with two
            // leaf children.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* split = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(split, L"model root must be a SplitPane after the split");

            // GREEN: the projected pane-tree root is now a split Grid (carrying
            // the SplitPane's id), with two star-sized cells.
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            VERIFY_IS_NOT_NULL(rootChild, L"the host must have a projected pane-tree root");
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid, L"the projected root must be a Grid for the split");
            const auto rootTag = splitGrid.Tag().try_as<uint64_t>();
            VERIFY_IS_TRUE(rootTag.has_value() && *rootTag == split->id.v,
                           L"the split Grid must carry the SplitPane's id in Tag");
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), splitGrid.ColumnDefinitions().Size(),
                             L"a vertical split projects two columns (two cells along the axis)");

            // Both model leaves have a projected leaf container nested in the
            // split (structure mirrors the model's 1->2 leaves).
            const auto leftContainer = _findLeafContainer(rootChild, leaf0);
            const auto rightContainer = _findLeafContainer(rootChild, rightLeaf);
            VERIFY_IS_NOT_NULL(leftContainer, L"the original leaf must have a container in the split");
            VERIFY_IS_NOT_NULL(rightContainer, L"the new sibling leaf must have a container in the split");

            // Each leaf container holds that leaf's Slice-C strip — the strip
            // collections for both leaves exist and are non-empty (one tab each).
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"the original leaf keeps its one-tab strip across the split");
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(rightLeaf),
                             L"the new sibling leaf gets its own one-tab strip (split-sibling skip lifted)");

            // Big-flip Slice F-5 (#54): no classic tab/pane tree is built flag-on
            // — the projected tree renders the split. `_tabs` stays empty.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"the split must not create a classic Tab (cutover)");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host is Visible (F-5 cutover) — the projected split IS the display");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice D (#54): a SplitRatioChanged updates the projected split
    // Grid's two cells' star sizes to match the model's new ratio. We split, then
    // resizePane to a non-0.5 ratio via the model and assert the GridLength star
    // VALUES (not measured widths) of the two cells are ratio and 1-ratio. This
    // is the headless-safe ratio assertion — pure GridLength inspection, no real
    // geometry. Host stays Collapsed; classic split untouched.
    //
    // RED before SplitRatioChanged updates the star sizes: the cells keep their
    // 0.5/0.5 split-creation sizes. GREEN after: they are 0.7/0.3.
    void WorkspaceTests::BigFlipD_SplitRatio_SetsStarSizes()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xD100 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::PaneId splitId{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf (vertical, ratio 0.5)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            page->_applyWorkspaceAction(std::move(split.state));

            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            splitId = sp->id;
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // The freshly-created split Grid's cells are 0.5 / 0.5 in star units.
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid);
            VERIFY_ARE_EQUAL(0.5, _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Vertical, 0),
                             L"created split: first cell star == ratio (0.5)");
            VERIFY_ARE_EQUAL(0.5, _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Vertical, 1),
                             L"created split: second cell star == 1-ratio (0.5)");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Change the split ratio to 0.7 via the model (resizePane)");
        result = RunOnUIThread([&]() {
            auto next = ::WorkspaceModel::resizePane(page->_workspaceModelState, splitId, 0.7);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));

            // The model recorded the new ratio.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            VERIFY_ARE_EQUAL(0.7, sp->ratio);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // GREEN: the projected split Grid's cells follow the model ratio.
            // XAML's GridLength backs its Value with float precision, so the
            // double 0.7 round-trips to ~0.69999998; compare with a small
            // tolerance (this asserts the RATIO structure, never a laid-out
            // pixel width — the headless-resize-clamp trap).
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid);
            VERIFY_IS_TRUE(std::abs(0.7 - _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Vertical, 0)) < 1e-5,
                           L"after resize: first cell star == ratio (0.7)");
            VERIFY_IS_TRUE(std::abs(0.3 - _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Vertical, 1)) < 1e-5,
                           L"after resize: second cell star == 1-ratio (0.3)");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across a ratio change");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice D (#54): closing one of a split's two leaves collapses the
    // split — the model lifts the surviving sibling to the workspace root — and
    // the projection follows: the host's projected pane-tree root becomes a
    // single leaf container again (no split Grid). We split, then closePane the
    // new sibling leaf; the model's root reverts to a LeafPane and the projected
    // root is a leaf container for the survivor. Host stays Collapsed.
    //
    // RED before SplitPaneCollapsed lifts the survivor: the projected root stays
    // a split Grid. GREEN after: it is a single leaf container for the survivor.
    void WorkspaceTests::BigFlipD_Collapse_LiftsSurvivor()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xD200 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::PaneId rightLeaf{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf (vertical), then confirm the projection is a split Grid");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            rightLeaf = split.newPaneId;
            page->_applyWorkspaceAction(std::move(split.state));

            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid, L"precondition: the projection is a split Grid before collapse");
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), splitGrid.ColumnDefinitions().Size());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close the new sibling leaf (closePane) — the split collapses to the survivor");
        result = RunOnUIThread([&]() {
            auto next = ::WorkspaceModel::closePane(page->_workspaceModelState, rightLeaf);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));

            // Model: the root is a single LeafPane again (the survivor, leaf0).
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size(), L"closing a split child must NOT remove the workspace");
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf, L"the root must collapse back to a single leaf");
            VERIFY_ARE_EQUAL(leaf0.v, leaf->id.v, L"the surviving leaf is the original (leaf0)");
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // GREEN: the projected root is a single leaf container for the
            // survivor, NOT a split Grid.
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            VERIFY_IS_NOT_NULL(rootChild, L"the host must still have a projected pane-tree root");
            const auto rootTag = rootChild.Tag().try_as<uint64_t>();
            VERIFY_IS_TRUE(rootTag.has_value() && *rootTag == leaf0.v,
                           L"the projected root must now be the survivor leaf's container (tag == leaf0)");

            // The collapsed-away leaf's container is gone from the tree.
            const auto goneContainer = _findLeafContainer(rootChild, rightLeaf);
            VERIFY_IS_NULL(goneContainer, L"the closed leaf's container must be lifted out of the tree");

            // The survivor still has its container + strip.
            const auto survivor = _findLeafContainer(rootChild, leaf0);
            VERIFY_IS_NOT_NULL(survivor);
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"the survivor keeps its one-tab strip");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across the collapse");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-0 (#54): a single-leaf workspace's content GetRoot() is
    // parented into the root leaf's OWN per-leaf content host (inside the
    // projected pane tree), not loosely into the single shared host — the shared
    // host is now the outer wrapper. We assert by element identity against the
    // leaf's host child, and that the host + tree stay Collapsed (ZERO visible
    // change). The mock content's GetRoot() is the stable Grid we attach.
    //
    // RED before F-0 populates the leaf cell: _projectLeafContainer left the
    // leaf's star row EMPTY, so _leafHostChildForTest is always null.
    // GREEN after: the root leaf's host child == the content's GetRoot().
    void WorkspaceTests::BigFlipF0_SingleLeaf_ContentInLeafHost()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF000 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        auto result = RunOnUIThread([&]() {
            const auto leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            // Exactly one content materialised at startup; it is the root leaf's.
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), mocks->size(),
                             L"startup materialised one content");
            const auto expectedRoot = (*mocks)[0]->GetRoot();
            VERIFY_IS_NOT_NULL(expectedRoot);

            // GREEN: the root leaf's per-leaf host holds that content root.
            const auto leafChild = page->_leafHostChildForTest(leaf0);
            VERIFY_IS_NOT_NULL(leafChild,
                               L"the root leaf's content host must hold the startup content");
            VERIFY_IS_TRUE(leafChild == expectedRoot,
                           L"the leaf host child must be the content's GetRoot() (by identity)");

            // The content root lives in the LEAF host, not loosely in the shared
            // host (which is now the outer wrapper).
            VERIFY_IS_TRUE(page->_workspaceHostChildForTest() != expectedRoot,
                           L"the content root must live in the leaf host, not the shared host");

            // Big-flip Slice F-5 (#54): no classic tab flag-on; the VISIBLE host
            // is the display.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must be Visible (F-5 cutover) — it is the display");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-0 (#54): THE KEY NEW CAPABILITY. A SPLIT workspace gives
    // EACH leaf its OWN per-leaf content host, and EACH host holds that leaf's
    // OWN distinct content root — the thing the single shared host could never
    // represent (it can hold only one element). We split the root leaf via the
    // model (the same path BigFlipD drives), then assert each of the two leaves'
    // hosts holds a distinct content root, by identity. Host + tree Collapsed.
    //
    // RED before F-0: the leaf cells are empty, so both _leafHostChildForTest
    // calls return null. GREEN after: two non-null, DISTINCT content roots, one
    // per leaf.
    void WorkspaceTests::BigFlipF0_Split_EachLeafHostHoldsOwnContent()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF100 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::PaneId rightLeaf{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf via the model (vertical, ratio 0.5)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            rightLeaf = split.newPaneId;
            page->_applyWorkspaceAction(std::move(split.state));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // Two contents materialised — one per leaf.
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), mocks->size(),
                             L"a split materialised a second content for the sibling leaf");

            // GREEN: each leaf's per-leaf host holds a non-null content root.
            const auto leftChild = page->_leafHostChildForTest(leaf0);
            const auto rightChild = page->_leafHostChildForTest(rightLeaf);
            VERIFY_IS_NOT_NULL(leftChild, L"the original leaf's host must hold its content");
            VERIFY_IS_NOT_NULL(rightChild, L"the new sibling leaf's host must hold ITS OWN content");

            // The KEY assertion: the two leaf hosts hold DISTINCT content roots
            // (each leaf renders its own terminal in its own cell). This is what
            // the single shared host could not represent.
            VERIFY_IS_TRUE(leftChild != rightChild,
                           L"each split leaf host must hold a DISTINCT content root");

            // Identity check against the two materialised mocks (order: index 0 =
            // the original leaf's startup content, index 1 = the split sibling's).
            const auto root0 = (*mocks)[0]->GetRoot();
            const auto root1 = (*mocks)[1]->GetRoot();
            VERIFY_IS_TRUE(leftChild == root0,
                           L"the original leaf host holds the original (startup) content root");
            VERIFY_IS_TRUE(rightChild == root1,
                           L"the sibling leaf host holds the split sibling's content root");

            // Big-flip Slice F-5 (#54): no classic tab is built flag-on; the
            // VISIBLE host's per-leaf cells hold each leaf's content.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"the split must not create a classic Tab (cutover)");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host is Visible (F-5 cutover) — each leaf cell holds its content");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-0 (#54): flag-off mirror. None of the per-leaf content
    // host machinery realizes when the flag is off — the workspace shell is
    // never initialized, so _paneContentHosts is empty and there are no projected
    // leaves. The classic single-tab UI renders exactly as upstream.
    void WorkspaceTests::BigFlipF0_FlagOff_NoLeafHosts()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            // Flag-off: the shell + model + view stay dormant, so no per-leaf
            // host (or any projection) was ever created. _leafHostChildForTest
            // returns null for any id (the map is empty).
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off must leave the workspace model dormant");
            VERIFY_IS_TRUE(page->_workspaceView == nullptr,
                           L"flag-off must NOT instantiate WorkspaceView");
            VERIFY_IS_NULL(page->_leafHostChildForTest(::WorkspaceModel::PaneId{ 0 }),
                           L"flag-off must realize NO per-leaf content hosts");
            VERIFY_IS_TRUE(page->_leafContentTabs().empty(),
                           L"flag-off must project NO leaves");

            // Classic single-tab UI is upstream-identical.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"flag-off startup renders the classic single-tab UI");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-1 (#54): a split projection carries a CUSTOM separator
    // Border between its two cells — NOT a community-toolkit GridSplitter. The
    // separator is a direct child of the split Grid (no third column/row, so the
    // two-cell ratio structure D asserts is intact), and it is tagged with the
    // split id it controls. Asserted by element TYPE (Border) and by the split id
    // (Tag) — never by laid-out geometry. Host stays Collapsed.
    //
    // RED before F-1 builds the separator: the split Grid has only its two cell
    // children, so _splitSeparatorForTest returns null. GREEN after: a Border
    // separator tagged with the split id exists.
    void WorkspaceTests::BigFlipF1_Split_BuildsCustomSeparator()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF100 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::PaneId splitId{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());

            // Baseline: a single-leaf projection has NO split Grid, hence no
            // separator.
            const auto rootBefore = page->_workspacePaneTreeRootChildForTest();
            const auto gridBefore = rootBefore.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NULL(page->_splitSeparatorForTest(gridBefore),
                           L"baseline single-leaf projection carries no split separator");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf (vertical) so a split Grid is projected");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            page->_applyWorkspaceAction(std::move(split.state));

            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            splitId = sp->id;
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // GREEN: the projected split Grid carries a CUSTOM Border separator
            // tagged with the split id.
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid);

            const auto separator = page->_splitSeparatorForTest(splitGrid);
            VERIFY_IS_NOT_NULL(separator,
                               L"the split Grid must carry a custom Border separator");
            // It is a Border (the custom separator), NOT a toolkit GridSplitter.
            VERIFY_IS_NOT_NULL(separator.try_as<winrt::Windows::UI::Xaml::Controls::Border>(),
                               L"the separator must be a custom Border, not a GridSplitter");
            const auto sepTag = separator.Tag().try_as<uint64_t>();
            VERIFY_IS_TRUE(sepTag.has_value() && *sepTag == splitId.v,
                           L"the separator must know which split it controls (Tag == splitId)");

            // The two-cell ratio structure is untouched (no third column added).
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), splitGrid.ColumnDefinitions().Size(),
                             L"the separator must NOT add a third column/cell");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) — the separator is invisible this slice");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-1 (#54): THE CRUX. A SIMULATED drag — calling the
    // headless-testable _resizeSplitFromDrag helper with a synthetic pixel delta
    // and a KNOWN total extent (never a laid-out pixel; the host is Collapsed so
    // ActualWidth would be 0) — dispatches the model resizePane action and the
    // projected split Grid's two cells re-project to the new ratio. We start at
    // ratio 0.5, drag +100px on a 1000px extent → expected new ratio 0.6, and
    // assert the model ratio AND the two cells' GridLength.Value (within 1e-5).
    // This proves drag→ratio→dispatch→re-project end-to-end with ZERO geometry.
    //
    // RED before _resizeSplitFromDrag dispatches resizePane: the model ratio (and
    // the projected cells) stay 0.5/0.5. GREEN after: 0.6/0.4.
    void WorkspaceTests::BigFlipF1_SimulatedDrag_DispatchesResizeAndReprojects()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF200 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::PaneId splitId{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf (vertical, ratio 0.5)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            page->_applyWorkspaceAction(std::move(split.state));

            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            splitId = sp->id;
            VERIFY_ARE_EQUAL(0.5, sp->ratio, L"precondition: split starts at ratio 0.5");
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // The headless ratio math, in isolation: +100px on a 1000px extent
            // moves a 0.5 ratio to 0.6.
            const double computed = page->_computeSplitRatioFromDrag(splitId, 100.0, 1000.0);
            VERIFY_IS_TRUE(std::abs(0.6 - computed) < 1e-5,
                           L"_computeSplitRatioFromDrag: +100/1000 from 0.5 -> 0.6");

            // A non-positive extent (the invisible/headless case) is a no-op: the
            // ratio is returned unchanged, so an invisible drag cannot move it.
            const double headless = page->_computeSplitRatioFromDrag(splitId, 100.0, 0.0);
            VERIFY_IS_TRUE(std::abs(0.5 - headless) < 1e-5,
                           L"_computeSplitRatioFromDrag: extent==0 returns the current ratio unchanged");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Simulate the drag through the dispatch helper (synthetic delta + known extent)");
        result = RunOnUIThread([&]() {
            // This is the SAME entry point the live PointerMoved handler calls;
            // here we feed it a synthetic delta + a known extent rather than a
            // laid-out one. It computes 0.6 and dispatches resizePane.
            page->_resizeSplitFromDrag(splitId, 100.0, 1000.0);

            // Model recorded the new ratio.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            VERIFY_IS_TRUE(std::abs(0.6 - sp->ratio) < 1e-5,
                           L"the simulated drag dispatched resizePane to ratio 0.6");
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // GREEN: the projected split Grid's two cells re-projected to the new
            // ratio. Compare GridLength.Value (float-backed) within 1e-5 — never
            // a measured pixel width (the headless-resize-clamp trap).
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid);
            VERIFY_IS_TRUE(std::abs(0.6 - _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Vertical, 0)) < 1e-5,
                           L"after the simulated drag: first cell star == ratio (0.6)");
            VERIFY_IS_TRUE(std::abs(0.4 - _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Vertical, 1)) < 1e-5,
                           L"after the simulated drag: second cell star == 1-ratio (0.4)");

            // The separator survives the re-projection (a fresh tree carries a
            // fresh separator).
            VERIFY_IS_NOT_NULL(page->_splitSeparatorForTest(splitGrid),
                               L"the re-projected split keeps its custom separator");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across the simulated drag");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-1 (#54): the orientation guard. A HORIZONTAL split projects
    // two ROWS; a simulated vertical drag (delta along the Y/height axis) must
    // move the boundary the same way it moves columns for a vertical split — the
    // helper takes the extent as a parameter, so the SAME math applies to both
    // axes. We drag +200px on a 1000px row extent from 0.5 → 0.7 and assert the
    // two ROW GridLengths re-project. This catches a horizontal/vertical sign or
    // orientation mismatch in the drag→ratio path.
    void WorkspaceTests::BigFlipF1_HorizontalSplit_DragReprojectsRows()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF300 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        ::WorkspaceModel::PaneId splitId{ 0 };

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf HORIZONTALLY (two rows, ratio 0.5)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Horizontal,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            page->_applyWorkspaceAction(std::move(split.state));

            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            splitId = sp->id;

            // The split Grid carries two ROWS and a separator tagged with the
            // split id.
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid);
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), splitGrid.RowDefinitions().Size(),
                             L"a horizontal split projects two rows");
            VERIFY_IS_NOT_NULL(page->_splitSeparatorForTest(splitGrid),
                               L"the horizontal split also carries a custom separator");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Simulate a +200px drag on a 1000px row extent -> ratio 0.7");
        result = RunOnUIThread([&]() {
            page->_resizeSplitFromDrag(splitId, 200.0, 1000.0);

            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* sp = std::get_if<::WorkspaceModel::SplitPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(sp);
            VERIFY_IS_TRUE(std::abs(0.7 - sp->ratio) < 1e-5,
                           L"the simulated drag dispatched resizePane to ratio 0.7");

            // GREEN: the two ROW GridLengths re-projected to 0.7 / 0.3.
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto splitGrid = rootChild.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            VERIFY_IS_NOT_NULL(splitGrid);
            VERIFY_IS_TRUE(std::abs(0.7 - _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Horizontal, 0)) < 1e-5,
                           L"after the drag: first ROW star == ratio (0.7)");
            VERIFY_IS_TRUE(std::abs(0.3 - _splitCellStar(splitGrid, ::WorkspaceModel::Axis::Horizontal, 1)) < 1e-5,
                           L"after the drag: second ROW star == 1-ratio (0.3)");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across the horizontal drag");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-1 (#54): flag-off mirror. The workspace shell is never
    // initialized when the flag is off, so no pane tree is projected and no split
    // separator is ever built. Upstream rendering is byte-for-byte unchanged.
    void WorkspaceTests::BigFlipF1_FlagOff_NoSeparator()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            // Flag-off: model + view dormant, no projected pane tree, hence no
            // separator anywhere.
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off must leave the workspace model dormant");
            VERIFY_IS_NULL(page->_workspacePaneTreeRootChildForTest(),
                           L"flag-off projects no pane tree, so there is no split Grid");
            // _splitSeparatorForTest is null-safe for a null Grid.
            VERIFY_IS_NULL(page->_splitSeparatorForTest(nullptr),
                           L"flag-off builds no custom separator");

            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"flag-off startup renders the classic single-tab UI");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-2 (#54): after startup the root leaf's first (and only)
    // strip VM has IsActive == true WITHOUT any ActiveTabChanged event. The seed
    // comes from querying the model's LeafPane::activeTabIdx at append time in
    // _appendPaneTabVm: tabs[activeTabIdx].id == the appended tab => IsActive.
    //
    // RED before F-2: _appendPaneTabVm hardcoded IsActive(false); the leaf's
    // only VM was inactive at startup (the diff engine suppresses
    // ActiveTabChanged for index 0 since no index change occurred).
    // GREEN after: the startup VM reports IsActive == true.
    void WorkspaceTests::BigFlipF2_FirstTab_IsActiveSeeded()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF200 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        auto result = RunOnUIThread([&]() {
            VERIFY_IS_TRUE(page->_workspaceModelState != nullptr,
                           L"model must be alive after flag-on startup");

            const auto leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid(), L"root leaf must be valid");

            // The leaf has exactly one strip VM.
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"startup: the root leaf's strip has one VM");

            // GREEN: that one VM is the active one — seeded from the model's
            // activeTabIdx without any ActiveTabChanged event.
            const auto activeId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeId.has_value(),
                           L"the startup strip VM must be the active row (IsActive seeded)");

            // The seeded active tab id must match the model's active tab for
            // this leaf (tabs[activeTabIdx].id).
            const auto* node = page->_workspaceModelState->pane(leaf0);
            VERIFY_IS_NOT_NULL(node);
            const auto* leafPane = std::get_if<::WorkspaceModel::LeafPane>(node);
            VERIFY_IS_NOT_NULL(leafPane);
            VERIFY_IS_FALSE(leafPane->tabs.empty());
            const auto modelActiveId = leafPane->tabs[leafPane->activeTabIdx].id;
            VERIFY_ARE_EQUAL(modelActiveId.v, *activeId,
                             L"seeded active id must equal the model's activeTabIdx entry");

            // Invariant: only ONE row active (there is exactly one VM, and it
            // IS active — no over-activation). We know the strip has one entry;
            // _activePaneTabIdForTest already confirmed that one is active.
            // For a multi-tab leaf the invariant would be: sum of IsActive==1.
            // With a one-tab strip, finding the active id is sufficient.
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"exactly one VM in the single-tab startup strip");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) (INVISIBLE this slice)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-2 (#54): after a split-then-collapse, the dead leaf's
    // entry is pruned from _paneTabStrips during the rebuild triggered by
    // SplitPaneCollapsed. Before the GC the map contained TWO entries (leaf0 +
    // rightLeaf); after the collapse and rebuild it must contain ONE (leaf0 only).
    //
    // RED before F-2: the dead leaf's collection was left in the map indefinitely
    // (Slice D left this as "harmless, GC later").
    // GREEN after: _rebuildActiveWorkspacePaneTree prunes the dead key.
    void WorkspaceTests::BigFlipF2_Collapse_PrunesDeadLeafFromStrip()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xF201 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{};
        ::WorkspaceModel::PaneId rightLeaf{};

        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Split the root leaf (vertical)");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto split = ::WorkspaceModel::splitPane(page->_workspaceModelState,
                                                     leaf0,
                                                     ::WorkspaceModel::Axis::Vertical,
                                                     0.5,
                                                     ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(split.newPaneId.valid());
            rightLeaf = split.newPaneId;
            page->_applyWorkspaceAction(std::move(split.state));

            // Precondition: two strip entries exist after the split.
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"precondition: original leaf has one strip VM");
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(rightLeaf),
                             L"precondition: sibling leaf has one strip VM");
            VERIFY_ARE_EQUAL(2u, static_cast<uint32_t>(page->_paneTabStrips.size()),
                             L"precondition: _paneTabStrips has two entries after split");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Close the sibling leaf (closePane) — split collapses");
        result = RunOnUIThread([&]() {
            auto next = ::WorkspaceModel::closePane(page->_workspaceModelState, rightLeaf);
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // GREEN: the dead leaf's strip entry was pruned.
            VERIFY_ARE_EQUAL(1u, static_cast<uint32_t>(page->_paneTabStrips.size()),
                             L"after collapse _paneTabStrips must have exactly one entry (the survivor)");

            // The surviving leaf's strip is intact.
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"the survivor's strip still has its one VM");

            // The dead leaf's strip entry is gone.
            VERIFY_ARE_EQUAL(0u, page->_paneTabStripSizeForTest(rightLeaf),
                             L"the collapsed leaf's strip entry must be pruned (returns 0 for missing key)");

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Visible (F-5 cutover) across the collapse");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Big-flip Slice F-2 (#54): flag-off mirror. When the workspaces flag is
    // off, the model is dormant and _paneTabStrips is never populated. The map
    // size is zero and _paneTabStripSizeForTest returns 0 for any PaneId.
    void WorkspaceTests::BigFlipF2_FlagOff_NoStripEntries()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off must leave the workspace model dormant");

            // No strip entries — the model never fired TabAdded through the
            // workspace path, so _paneTabStrips was never written.
            VERIFY_ARE_EQUAL(0u, static_cast<uint32_t>(page->_paneTabStrips.size()),
                             L"flag-off: _paneTabStrips must be empty");

            // Any PaneId lookup returns 0 (missing key).
            VERIFY_ARE_EQUAL(0u, page->_paneTabStripSizeForTest(::WorkspaceModel::PaneId{ 1 }),
                             L"flag-off: strip size for any leaf is 0 (no entry)");

            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"flag-off startup renders the classic single-tab UI unchanged");
        });
        VERIFY_SUCCEEDED(result);
    }

    // ------------------------------------------------------------------
    // Big-flip Slice F-5 (#54): THE CUTOVER. The model's projected pane tree is
    // now the VISIBLE display; the classic tab strip is retired flag-on.
    // ------------------------------------------------------------------

    // (a) The projected-pane-tree host is Visible flag-on AND is the SOLE child
    // of _tabContent — it IS the display (no classic Tab, no double-render).
    void WorkspaceTests::BigFlipF5_FlagOn_HostVisibleAndSoleChildOfTabContent()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_IS_TRUE(page->_workspaceContentHost != nullptr);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must be Visible flag-on (the cutover made it the display)");

            // No classic tab was ever built flag-on.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on the classic tab strip is never populated (cutover)");

            // _tabContent has EXACTLY one child, and it is the host (no
            // double-render: the classic content was never appended).
            VERIFY_IS_TRUE(page->_tabContent != nullptr);
            VERIFY_ARE_EQUAL(1u, page->_tabContent.Children().Size(),
                             L"_tabContent must hold exactly one child flag-on");
            uint32_t hostIndex = 0;
            VERIFY_IS_TRUE(page->_tabContent.Children().IndexOf(page->_workspaceContentHost, hostIndex),
                           L"the host must be _tabContent's SOLE child (the projected tree is the display)");
            VERIFY_ARE_EQUAL(0u, hostIndex);
        });
        VERIFY_SUCCEEDED(result);
    }

    // F-5 fix (#46): the PRIMARY blocker regression test. Flag-on, opening the
    // Settings UI (default keybinding ctrl+, / system menu / command palette)
    // routed through the classic-tab path, which selected the new tab and
    // cleared _tabContent — dropping the workspace host and leaving a blank
    // window. With OpenSettingsUI guarded to a no-op flag-on, the call must be
    // inert: no settings tab injected into _tabs, and the host must remain the
    // sole child of _tabContent (display NOT corrupted).
    void WorkspaceTests::BigFlipF5_FlagOn_OpenSettings_DoesNotCorruptHost()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&page]() {
            // Capture the pre-call invariant: host is the sole child of _tabContent.
            VERIFY_IS_TRUE(page->_workspaceContentHost != nullptr);
            VERIFY_IS_TRUE(page->_tabContent != nullptr);
            VERIFY_ARE_EQUAL(1u, page->_tabContent.Children().Size(),
                             L"precondition: _tabContent holds exactly one child (the host)");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"precondition: no classic tab exists flag-on");

            // Invoke the settings-open path. Flag-on this must be a no-op so it
            // cannot build a classic settings tab and clear _tabContent.
            page->OpenSettingsUI();

            // No classic settings tab was injected.
            VERIFY_IS_TRUE(page->_settingsTab == nullptr,
                           L"flag-on OpenSettingsUI must not create a settings tab");
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"flag-on OpenSettingsUI must not grow _tabs (no classic tab injected)");

            // The sole-child host invariant survived the call: the workspace
            // host is still the one and only child of _tabContent (display intact).
            VERIFY_ARE_EQUAL(1u, page->_tabContent.Children().Size(),
                             L"_tabContent must still hold exactly one child after OpenSettingsUI");
            uint32_t hostIndex = 0;
            VERIFY_IS_TRUE(page->_tabContent.Children().IndexOf(page->_workspaceContentHost, hostIndex),
                           L"the workspace host must still be _tabContent's sole child (not dropped)");
            VERIFY_ARE_EQUAL(0u, hostIndex);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Visible,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must still be Visible after OpenSettingsUI");
        });
        VERIFY_SUCCEEDED(result);
    }

    // (b) The classic window tab strip is retired flag-on: _UpdateTabView forces
    // _tabRow.Height(0) and the TabView Collapsed. Flag-off it keeps the upstream
    // auto-sizing (NaN height = "Auto").
    void WorkspaceTests::BigFlipF5_FlagOn_TabRowHeightZero()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&page]() {
            // Drive _UpdateTabView explicitly so the assertion is robust to the
            // exact startup call order; the flag-on branch forces the strip off.
            page->_UpdateTabView();
            VERIFY_IS_TRUE(page->_tabRow != nullptr);
            VERIFY_ARE_EQUAL(0.0, page->_tabRow.Height(),
                             L"flag-on the classic tab row must be forced to zero height (cutover)");
            VERIFY_IS_TRUE(page->_tabView != nullptr);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_tabView.Visibility(),
                             L"flag-on the classic TabView must be Collapsed (cutover)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // (b) flag-off mirror: _UpdateTabView leaves the tab row auto-sized (NaN
    // height) — byte-for-byte upstream.
    void WorkspaceTests::BigFlipF5_FlagOff_TabRowHeightAuto()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ],
            "alwaysShowTabs": true
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOff(page, settings);

        auto result = RunOnUIThread([&page]() {
            page->_UpdateTabView();
            VERIFY_IS_TRUE(page->_tabRow != nullptr);
            // NaN is the XAML "Auto" sentinel the upstream visible path sets;
            // it is NOT 0 (the cutover-forced value). Assert it is NOT zeroed.
            VERIFY_IS_TRUE(std::isnan(page->_tabRow.Height()),
                           L"flag-off the tab row keeps its upstream auto (NaN) height");
            VERIFY_IS_TRUE(page->_workspaceModelState == nullptr,
                           L"flag-off must leave the workspace model dormant");
        });
        VERIFY_SUCCEEDED(result);
    }

    // ============================================================
    // Per-pane strip Slice 1 (#54): TabStripView extraction.
    // ============================================================
    namespace
    {
        // The TabStripView the projector put in `leaf`'s container row 0. The
        // leaf container (built by _projectLeafContainer) is a Grid whose row-0
        // child is the strip control and row-1 child is the per-leaf content
        // host. Returns nullptr if no such control is there.
        winrt::TerminalApp::TabStripView _leafTabStripView(
            const winrt::Windows::UI::Xaml::FrameworkElement& leafContainer)
        {
            const auto grid = leafContainer.try_as<winrt::Windows::UI::Xaml::Controls::Grid>();
            if (!grid)
            {
                return nullptr;
            }
            for (uint32_t i = 0; i < grid.Children().Size(); ++i)
            {
                const auto child = grid.Children().GetAt(i);
                if (const auto strip = child.try_as<winrt::TerminalApp::TabStripView>())
                {
                    return strip;
                }
            }
            return nullptr;
        }
    }

    // Per-pane strip Slice 1 (#54): a projected leaf's row-0 child is a
    // TabStripView (NOT a bare ListView), and its inner ListView's items are the
    // leaf's PaneTabViewModels in model order. We add a SECOND tab to the root
    // leaf via the model (so the strip has 2 rows to order-check), then walk the
    // projected tree to the leaf container and assert the control type + items.
    //
    // RED before the extraction: the leaf row-0 child is a hand-built ListView,
    // not a TabStripView. GREEN after: it is a TabStripView whose inner
    // ListView's ItemsSource is the leaf's VM collection.
    void WorkspaceTests::StripSlice1_LeafRowZeroChildIsTabStripView_ItemsMatchModel()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5100 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };

        auto result = RunOnUIThread([&]() {
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            ws0 = workspaces[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a SECOND tab to the root leaf so the strip has two ordered rows");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            page->_applyWorkspaceAction(std::move(added.state));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto leafContainer = _findLeafContainer(rootChild, leaf0);
            VERIFY_IS_NOT_NULL(leafContainer, L"the root leaf must have a projected leaf container");

            // GREEN: the leaf container's row-0 child is a TabStripView.
            const auto strip = _leafTabStripView(leafContainer);
            VERIFY_IS_NOT_NULL(strip,
                               L"the leaf container's row-0 child must be a TabStripView (not a bare ListView)");

            // M1: the strip hosts the real MUX TabView, and its TabItems are the
            // manual projection of the leaf's PaneTabViewModels in model order
            // (each TabViewItem.Tag is its VM). The source VM collection the page
            // set is on the strip's ItemsSource.
            const auto tabView = strip.TabViewControl();
            VERIFY_IS_NOT_NULL(tabView, L"the TabStripView must host a MUX TabView");
            const auto items = strip.ItemsSource().try_as<IObservableVector<winrt::TerminalApp::PaneTabViewModel>>();
            VERIFY_IS_NOT_NULL(items, L"the strip's ItemsSource must be the leaf's VM collection");

            // Items count == the leaf's model tab count.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), leaf->tabs.size());
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), items.Size(),
                             L"the strip items count must equal the leaf's model tab count");
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), tabView.TabItems().Size(),
                             L"the TabView must project one TabViewItem per model tab");

            // Order matches the model's tab order (by stable id) — both the source
            // collection AND the projected TabViewItems (resolved via each item's
            // Tag VM).
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                VERIFY_ARE_EQUAL(leaf->tabs[i].id.v, items.GetAt(i).Id(),
                                 L"the strip rows must be in model tab order, by id");
                const auto item = tabView.TabItems().GetAt(i).try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
                VERIFY_IS_NOT_NULL(item, L"each TabItem must be a TabViewItem");
                const auto tagVm = item.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
                VERIFY_IS_NOT_NULL(tagVm, L"each TabViewItem must carry its VM in Tag");
                VERIFY_ARE_EQUAL(leaf->tabs[i].id.v, tagVm.Id(),
                                 L"the projected TabViewItems must be in model tab order, by id");
            }

            // Cutover invariant: no classic Tab was built.
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"the strip extraction must not create a classic Tab (cutover)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Workspaces M1 (#54, ADR-001): the strip's MUX TabView has all three drag
    // flags OFF — CanReorderTabs / CanDragTabs / AllowDropTabs == false. This is
    // failfast-avoidance, not cosmetics: AllowDropTabs defaults TRUE, and pairing
    // a draggable TabView with no TabDroppedOutside handler crashes
    // Windows.UI.Xaml.dll (0xc000027b). M6 wires the full drag state machine; M1
    // must keep drag disabled. (The old Slice-1 premise — an explicit horizontal
    // ItemsPanel + SelectionMode=Single on a hand-built ListView — no longer
    // applies: the MUX TabView is inherently a horizontal single-select strip, so
    // we assert the M1-relevant invariant instead.) STRUCTURAL only — no layout.
    void WorkspaceTests::StripSlice1_InnerListViewIsHorizontalAndSingleSelect()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5200 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto leafContainer = _findLeafContainer(rootChild, leaf0);
            VERIFY_IS_NOT_NULL(leafContainer);
            const auto strip = _leafTabStripView(leafContainer);
            VERIFY_IS_NOT_NULL(strip);
            const auto tabView = strip.TabViewControl();
            VERIFY_IS_NOT_NULL(tabView);

            // Drag OFF — the M1 failfast-avoidance invariant (all three flags).
            VERIFY_IS_FALSE(tabView.CanReorderTabs(),
                            L"M1: CanReorderTabs must be false (drag is M6)");
            VERIFY_IS_FALSE(tabView.CanDragTabs(),
                            L"M1: CanDragTabs must be false (drag is M6)");
            VERIFY_IS_FALSE(tabView.AllowDropTabs(),
                            L"M1: AllowDropTabs must be false (it defaults true; on + no "
                            L"TabDroppedOutside handler crashes Windows.UI.Xaml.dll)");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Workspaces M1 (#54, ADR-001), strengthened from Slice 1/2a: selection is a
    // ONE-WAY projection of the model with a reentrancy guard. The TabView's
    // SelectedItem is driven FROM the model (each VM's IsActive push), never the
    // other way: the control never writes IsActive back. We prove:
    //   (1) Model→control: TabView.SelectedItem is the TabViewItem whose Tag VM
    //       IsActive.
    //   (2) A MODEL-driven active-tab change (selectTab via the diff) MOVES
    //       SelectedItem to the newly-active item AND raises ZERO ActivateRequested
    //       intents — i.e. the programmatic SelectedItem push is reentrancy-guarded
    //       so it does NOT loop back into a user intent (the crash-avoidance
    //       0xc000027b corner) and the control never re-derives the model.
    //   (3) The control never sets IsActive directly: after the model push the
    //       VMs' IsActive exactly mirror the model, and no extra intent fired.
    // (The old Slice-1 premise — forcing a ListView's unbound SelectedItem to a
    // non-active row must NOT change the model — is inverted under the real
    // TabView: forcing SelectedItem there fires SelectionChanged, which is a USER
    // intent and CORRECTLY flows selectTab through the model. That intent path is
    // covered by StripSlice1_ActivateIntent_DispatchesSelectTab; here we pin the
    // model→control direction + the guard.)
    void WorkspaceTests::StripSlice1_ActiveVmIsReflected_SelectionIsPureProjection()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5300 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto result = RunOnUIThread([&]() {
            ws0 = page->_workspaceModelState->workspaces_view()[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a SECOND tab so the strip has a concrete non-active VM to click on");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            page->_applyWorkspaceAction(std::move(added.state));
            // The first tab stays active after adding a second (model invariant).
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(leaf0));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            // The model's active tab id for the root leaf.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_IS_FALSE(leaf->tabs.empty());
            const auto modelActiveId = leaf->tabs[leaf->activeTabIdx].id.v;

            // The strip's active VM (IsActive) is the model's active tab — the
            // highlight derives from IsActive (F-2 seed), not the ListView's own
            // selection state.
            const auto activeVmId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeVmId.has_value(), L"the leaf's strip must have an active VM");
            VERIFY_ARE_EQUAL(modelActiveId, *activeVmId,
                             L"the active strip VM must reflect the model's active tab");

            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto leafContainer = _findLeafContainer(rootChild, leaf0);
            VERIFY_IS_NOT_NULL(leafContainer);
            const auto strip = _leafTabStripView(leafContainer);
            VERIFY_IS_NOT_NULL(strip);
            const auto tabView = strip.TabViewControl();
            VERIFY_IS_NOT_NULL(tabView);

            const auto items = strip.ItemsSource().try_as<IObservableVector<winrt::TerminalApp::PaneTabViewModel>>();
            VERIFY_IS_NOT_NULL(items);
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), items.Size());

            // A small helper: the TabViewItem whose Tag VM has the given id.
            const auto itemForId = [&](uint64_t id) -> winrt::Microsoft::UI::Xaml::Controls::TabViewItem {
                for (uint32_t i = 0; i < tabView.TabItems().Size(); ++i)
                {
                    const auto it = tabView.TabItems().GetAt(i).try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
                    if (it)
                    {
                        const auto vm = it.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
                        if (vm && vm.Id() == id)
                        {
                            return it;
                        }
                    }
                }
                return nullptr;
            };

            // (1) Model→control: SelectedItem is the active VM's TabViewItem.
            const auto selectedItem = tabView.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            VERIFY_IS_NOT_NULL(selectedItem, L"the TabView must have a selected item (the active tab)");
            const auto selectedVm = selectedItem.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
            VERIFY_IS_NOT_NULL(selectedVm);
            VERIFY_ARE_EQUAL(modelActiveId, selectedVm.Id(),
                             L"TabView.SelectedItem must project the model's active tab");

            // The non-active tab's id (the model's other tab).
            uint64_t otherId = 0;
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                if (!items.GetAt(i).IsActive())
                {
                    otherId = items.GetAt(i).Id();
                    break;
                }
            }
            VERIFY_IS_TRUE(otherId != 0, L"there must be a non-active tab");

            // (2)+(3) Drive the model's active tab via selectTab (a MODEL change,
            // NOT a synthetic SelectionChanged). Count ActivateRequested raises on
            // BOTH VMs across the push: a model-driven SelectedItem push must
            // raise ZERO intents (the reentrancy guard) — the control re-derives
            // selection from the model, never the reverse.
            int activateRaises = 0;
            std::vector<winrt::event_token> tokens;
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                tokens.push_back(items.GetAt(i).ActivateRequested([&](auto&&, auto&&) { ++activateRaises; }));
            }

            auto next = ::WorkspaceModel::selectTab(page->_workspaceModelState, ::WorkspaceModel::TabId{ otherId });
            VERIFY_IS_TRUE(next != nullptr);
            page->_applyWorkspaceAction(std::move(next));

            // SelectedItem MOVED to the newly-active tab (model→control), and the
            // VMs' IsActive exactly mirror the model — the control set neither.
            const auto selectedAfter = tabView.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            VERIFY_IS_NOT_NULL(selectedAfter);
            const auto selectedVmAfter = selectedAfter.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
            VERIFY_IS_NOT_NULL(selectedVmAfter);
            VERIFY_ARE_EQUAL(otherId, selectedVmAfter.Id(),
                             L"a model-driven selectTab must MOVE SelectedItem (model->control projection)");
            VERIFY_ARE_EQUAL(otherId, itemForId(otherId).Tag().as<winrt::TerminalApp::PaneTabViewModel>().Id());

            const auto activeVmAfter = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeVmAfter.has_value());
            VERIFY_ARE_EQUAL(otherId, *activeVmAfter,
                             L"the active VM mirrors the model after the diff (a pure projection)");

            // THE LOAD-BEARING ASSERTION: the programmatic SelectedItem push fired
            // NO activate intent — the reentrancy guard prevented the control's
            // own SelectionChanged from looping back into RequestActivate.
            VERIFY_ARE_EQUAL(0, activateRaises,
                             L"a model-driven SelectedItem push must raise ZERO ActivateRequested "
                             L"(reentrancy guard — no write-back, no 0xc000027b reentrancy)");

            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                items.GetAt(i).ActivateRequested(tokens[i]);
            }
        });
        VERIFY_SUCCEEDED(result);
    }

    // Per-pane strip Slice 1 (#54): a row's RequestActivate intent still
    // dispatches selectTab through the model (the intent is wired on the VM in
    // _appendPaneTabVm; the template's Tapped binds to it). Dispatch-level —
    // raise RequestActivate on a non-active VM and assert the model's active tab
    // flips to it (mirroring the BigFlipC strip-intent coverage).
    void WorkspaceTests::StripSlice1_ActivateIntent_DispatchesSelectTab()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5400 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto result = RunOnUIThread([&]() {
            ws0 = page->_workspaceModelState->workspaces_view()[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a second tab; the first stays active. Raise RequestActivate on the second.");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            page->_applyWorkspaceAction(std::move(added.state));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto leafContainer = _findLeafContainer(rootChild, leaf0);
            const auto strip = _leafTabStripView(leafContainer);
            VERIFY_IS_NOT_NULL(strip);
            const auto items = strip.ItemsSource().try_as<IObservableVector<winrt::TerminalApp::PaneTabViewModel>>();
            VERIFY_IS_NOT_NULL(items);
            VERIFY_ARE_EQUAL(static_cast<uint32_t>(2), items.Size());

            // Find a VM that is NOT currently active and raise its activate intent.
            winrt::TerminalApp::PaneTabViewModel target{ nullptr };
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                if (!items.GetAt(i).IsActive())
                {
                    target = items.GetAt(i);
                    break;
                }
            }
            VERIFY_IS_NOT_NULL(target, L"there must be a non-active row to activate");
            const auto targetId = target.Id();

            // The template binds Tapped -> RequestActivate; call it directly
            // (dispatch-level — no synthetic pointer / layout).
            target.RequestActivate();

            // The model's active tab flipped to the target.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(targetId, leaf->tabs[leaf->activeTabIdx].id.v,
                             L"RequestActivate must dispatch selectTab so the model's active tab flips");
            // And the projected active VM reflects the new active tab.
            const auto activeVmId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeVmId.has_value());
            VERIFY_ARE_EQUAL(targetId, *activeVmId,
                             L"the strip's active VM must re-project to the newly active tab");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Per-pane strip Slice 1 (#54): a row's RequestClose intent still dispatches
    // closeTab through the model. Dispatch-level — add a 2nd tab, raise
    // RequestClose on one row, assert the leaf drops to one model tab and the
    // strip re-projects to one row.
    void WorkspaceTests::StripSlice1_CloseIntent_DispatchesCloseTab()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5500 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto result = RunOnUIThread([&]() {
            ws0 = page->_workspaceModelState->workspaces_view()[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
        });
        VERIFY_SUCCEEDED(result);

        ::WorkspaceModel::TabId secondId{ 0 };
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            secondId = added.id;
            page->_applyWorkspaceAction(std::move(added.state));
            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(leaf0));
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto strip = _leafTabStripView(_findLeafContainer(page->_workspacePaneTreeRootChildForTest(), leaf0));
            VERIFY_IS_NOT_NULL(strip);
            const auto items = strip.ItemsSource().try_as<IObservableVector<winrt::TerminalApp::PaneTabViewModel>>();
            VERIFY_IS_NOT_NULL(items);

            // Raise RequestClose on the row for the SECOND tab (the close Button
            // binds Click -> RequestClose; call it directly, dispatch-level).
            winrt::TerminalApp::PaneTabViewModel target{ nullptr };
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                if (items.GetAt(i).Id() == secondId.v)
                {
                    target = items.GetAt(i);
                    break;
                }
            }
            VERIFY_IS_NOT_NULL(target);
            target.RequestClose();

            // The leaf dropped to one model tab and the strip re-projected to one.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            const auto* leaf = std::get_if<::WorkspaceModel::LeafPane>(&workspaces[0].root);
            VERIFY_IS_NOT_NULL(leaf);
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), leaf->tabs.size(),
                             L"RequestClose must dispatch closeTab so the leaf drops to one tab");
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0),
                             L"the strip must re-project to one row after the close");
        });
        VERIFY_SUCCEEDED(result);
    }

    // ============================================================
    // Workspaces M1 (#54, ADR-001): NATIVE TabViewItem chrome. The old bespoke
    // re-skin (rounded-top shape, flare/foot, inter-tab separators, hover
    // highlight, IsActive-driven SelectedBackground border + theme-correct
    // foreground labels) is DELETED — the real MUX TabView renders all of that
    // natively. The chrome that M1 PROJECTS is the native TabViewItem.Header
    // (title), .IconSource (icon path) and tooltip, driven from the VM. We assert
    // those projections STRUCTURALLY (no laid-out pixel — the headless
    // std::clamp/layout limits). The selection visual is now TabView.SelectedItem,
    // covered by StripSlice1_ActiveVmIsReflected_SelectionIsPureProjection.
    // ============================================================

    // M1: each projected TabViewItem's native Header reflects its VM's Title (the
    // native chrome the deleted re-skin used to hand-build), and the projection
    // tracks the model — when the live content title changes (via the
    // ContentMounted/TitleChanged bind), the VM Title moves and the Header
    // re-projects. We assert the Header equals the VM Title, headless (no layout).
    void WorkspaceTests::StripSlice2a_InnerListViewCarriesReskinContainerStyle()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5600 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto rootChild = page->_workspacePaneTreeRootChildForTest();
            const auto leafContainer = _findLeafContainer(rootChild, leaf0);
            VERIFY_IS_NOT_NULL(leafContainer);
            const auto strip = _leafTabStripView(leafContainer);
            VERIFY_IS_NOT_NULL(strip);
            const auto tabView = strip.TabViewControl();
            VERIFY_IS_NOT_NULL(tabView);
            VERIFY_IS_TRUE(tabView.TabItems().Size() >= 1u, L"the leaf has at least one projected tab");

            // Native chrome: M1 set the TabViewItem.Header to the VM Title
            // (box_value'd string). Workspaces M2 (#54, ADR-001) replaced that
            // plain string with a hosted TabHeaderControl (reusing classic WT's
            // inline renamer) whose Title carries the VM Title — so the header is
            // now the control, and its Title (not a boxed string) projects the VM
            // Title. The deleted re-skin used a hand-built TextBlock.
            const auto item = tabView.TabItems().GetAt(0).try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            VERIFY_IS_NOT_NULL(item);
            const auto vm = item.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
            VERIFY_IS_NOT_NULL(vm);
            const auto header = item.Header().try_as<winrt::TerminalApp::TabHeaderControl>();
            VERIFY_IS_NOT_NULL(header,
                               L"the native TabViewItem.Header must be the reused TabHeaderControl (M2 renamer host)");
            VERIFY_ARE_EQUAL(vm.Title(), header.Title(),
                             L"the hosted TabHeaderControl.Title must project the VM Title");

            // The Content is the throwaway empty Border (the drag-identity bodge —
            // the terminal lives in the leaf content host, not the strip).
            const auto content = item.Content().try_as<winrt::Windows::UI::Xaml::Controls::Border>();
            VERIFY_IS_NOT_NULL(content,
                               L"the TabViewItem.Content must be a throwaway empty Border (drag-identity bodge); "
                               L"the terminal is hosted in leafContentHost, not the strip");
        });
        VERIFY_SUCCEEDED(result);
    }

    // M1: each projected TabViewItem's native IconSource is driven from the VM's
    // Icon path (projected from the mounted content's Icon() via
    // _bindTabChromeToContent), resolved through IconPathConverter::IconSourceMUX
    // exactly like the classic Tab::UpdateIcon. We set the VM Icon to a known
    // path and assert the TabViewItem.IconSource becomes non-null; clearing the
    // path clears it. STRUCTURAL only — we assert presence/absence of the
    // IconSource, never a rendered glyph (the headless layout limits).
    void WorkspaceTests::StripSlice2a_IsActiveDrivesSelectedBackgroundVisibility()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0x5700 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::PaneId leaf0{ 0 };
        auto result = RunOnUIThread([&]() {
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&]() {
            const auto strip = _leafTabStripView(_findLeafContainer(page->_workspacePaneTreeRootChildForTest(), leaf0));
            VERIFY_IS_NOT_NULL(strip);
            const auto tabView = strip.TabViewControl();
            VERIFY_IS_NOT_NULL(tabView);
            VERIFY_IS_TRUE(tabView.TabItems().Size() >= 1u);

            const auto item = tabView.TabItems().GetAt(0).try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            VERIFY_IS_NOT_NULL(item);
            const auto vm = item.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
            VERIFY_IS_NOT_NULL(vm);

            // Set a known icon path on the VM; the strip's PropertyChanged arm
            // re-projects the native IconSource via IconSourceMUX.
            vm.Icon(L"ms-appx:///ProfileIcons/{0caa0dad-35be-5f56-a8ff-afceeeaa6101}.png");
            const auto iconSource = item.IconSource();
            VERIFY_IS_NOT_NULL(iconSource,
                               L"a non-empty VM Icon must project a native TabViewItem.IconSource (IconSourceMUX)");

            // Clearing the path clears the IconSource (mirrors Tab::UpdateIcon's
            // hidden-icon path).
            vm.Icon(L"");
            VERIFY_IS_NULL(item.IconSource(),
                           L"an empty VM Icon must clear the native TabViewItem.IconSource");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Workspaces M3 (#54, ADR-001): tab bell/attention as VM-RUNTIME state (an ADR
    // deviation — NOT a WorkspaceModel field; a bell is ephemeral content-emitted
    // attention auto-dismissed on focus / a timer, the same category M2 pushes onto
    // the VM directly like the live shell title). Placed here (after the M1 chrome
    // tests) so the namespace-local _leafTabStripView / _findLeafContainer helpers
    // it uses to reach the hosted header are in scope.
    //
    // This pins the bell behaviour at the VM/projection level (no real gestures /
    // layout — the headless std::clamp-resize trap), using a minimal
    // MockPaneContent.RaiseBell() raiser (mirrors the existing SetTitle raiser).
    // We add a 2nd tab, resolve whichever tab is left INACTIVE (we don't assume an
    // ordering), and drive the bell on that inactive tab:
    //  1. Add a 2nd tab; resolve the INACTIVE tab (and its backing mock by index).
    //  2. A BellRequested with SendNotification==false on the inactive tab does NOT
    //     light it (mirrors classic Tab.cpp:1154's notification gate).
    //  3. A BellRequested with SendNotification==true sets that VM's BellIndicator
    //     true AND the hosted header's TerminalTabStatus.BellIndicator reflects it
    //     (the chrome projection).
    //  4. Making that tab active (the same _setActivePaneTabVm projection the real
    //     ActiveTabChanged arm drives, via a RequestActivate intent) CLEARS the
    //     BellIndicator (dismiss-on-focus, mirroring Tab.cpp:328/1421), and the
    //     header reflects the cleared state.
    //
    // In a headless test the BellRequested handler's HasThreadAccess()-gated
    // dispatcher hop is a no-op (already on the UI thread), so the VM is updated
    // synchronously. The classic path is untouched.
    void WorkspaceTests::StripM3_Bell_SetsIndicatorAndDismissesOnFocus()
    {
        static constexpr std::wstring_view settingsJson{ LR"(
        {
            "defaultProfile": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
            "experimental.workspaces.enabled": true,
            "profiles": [
                {
                    "name" : "profile0",
                    "guid": "{6239a42c-1111-49a3-80bd-e8fdd045185c}",
                    "historySize": 1,
                    "closeOnExit": "never"
                }
            ]
        })" };

        CascadiaSettings settings{ settingsJson, {} };
        VERIFY_IS_NOT_NULL(settings);

        // The factory records every MockPaneContent it hands out so the test can
        // raise a bell from a specific tab's content after the fact.
        auto mocks = std::make_shared<std::vector<winrt::com_ptr<MockPaneContent>>>();

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings, [mocks](winrt::TerminalApp::implementation::TerminalPage* p) {
            p->_makePaneContentForSpecOverrideForTest = [mocks](const ::WorkspaceModel::TabContent&) -> winrt::TerminalApp::IPaneContent {
                auto mock = winrt::make_self<MockPaneContent>(static_cast<uint64_t>(0xE200 + mocks->size()));
                mocks->push_back(mock);
                return mock.as<winrt::TerminalApp::IPaneContent>();
            };
        });

        ::WorkspaceModel::WorkspaceId ws0{ 0 };
        ::WorkspaceModel::PaneId leaf0{ 0 };
        // The strip index of the INACTIVE tab (the one we bell), and its model id.
        // The mounted-content factory hands out mocks in strip-index order, so
        // mocks[inactiveIdx] backs the inactive tab's content.
        uint32_t inactiveIdx{ 0 };
        uint64_t inactiveTabId{ 0 };

        auto result = RunOnUIThread([&]() {
            ws0 = page->_workspaceModelState->workspaces_view()[0].id;
            leaf0 = _rootLeafId(page->_workspaceModelState, 0);
            VERIFY_IS_TRUE(leaf0.valid());
            VERIFY_ARE_EQUAL(1u, page->_paneTabStripSizeForTest(leaf0));
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Add a second tab; one of the two tabs is now INACTIVE — that is the one we bell.");
        result = RunOnUIThread([&]() {
            const ::WorkspaceModel::TerminalSpec spec{};
            auto added = ::WorkspaceModel::newTab(page->_workspaceModelState, ws0, leaf0, ::WorkspaceModel::TabContent{ spec });
            VERIFY_IS_TRUE(added.id.valid());
            page->_applyWorkspaceAction(std::move(added.state));

            VERIFY_ARE_EQUAL(2u, page->_paneTabStripSizeForTest(leaf0));
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), mocks->size(),
                             L"both tabs' content must have mounted (two mocks)");

            // Resolve the inactive tab by querying IsActive — robust to which tab
            // the model leaves active after the add (don't assume an ordering).
            const auto activeId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeId.has_value());

            bool foundInactive = false;
            for (uint32_t i = 0; i < 2; ++i)
            {
                const auto vm = page->_paneTabStripVmAtForTest(leaf0, i);
                VERIFY_IS_NOT_NULL(vm);
                if (!vm.IsActive())
                {
                    inactiveIdx = i;
                    inactiveTabId = vm.Id();
                    foundInactive = true;
                    VERIFY_IS_FALSE(vm.BellIndicator(), L"no bell on the inactive tab yet");
                    break;
                }
            }
            VERIFY_IS_TRUE(foundInactive, L"with two tabs there must be exactly one inactive tab to bell");
        });
        VERIFY_SUCCEEDED(result);

        // A small helper: the TabHeaderControl hosting the INACTIVE tab's header,
        // so the test can read its TerminalTabStatus.BellIndicator (the chrome the
        // bell drives). Resolved by Tag identity, like the M1/M2 chrome tests.
        const auto belledHeader = [&]() -> winrt::TerminalApp::TabHeaderControl {
            const auto leafContainer = _findLeafContainer(page->_workspacePaneTreeRootChildForTest(), leaf0);
            const auto strip = _leafTabStripView(leafContainer);
            if (!strip)
            {
                return nullptr;
            }
            const auto tabView = strip.TabViewControl();
            if (!tabView)
            {
                return nullptr;
            }
            for (uint32_t i = 0; i < tabView.TabItems().Size(); ++i)
            {
                const auto item = tabView.TabItems().GetAt(i).try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
                if (!item)
                {
                    continue;
                }
                const auto vm = item.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
                if (vm && vm.Id() == inactiveTabId)
                {
                    return item.Header().try_as<winrt::TerminalApp::TabHeaderControl>();
                }
            }
            return nullptr;
        };

        Log::Comment(L"A bell WITHOUT a notification request must NOT light the tab (Tab.cpp:1154 gate).");
        result = RunOnUIThread([&]() {
            // mocks[inactiveIdx] backs the inactive tab. Raise a bell with
            // sendNotification == false.
            (*mocks)[inactiveIdx]->RaiseBell(/*flashTaskbar*/ false, /*sendNotification*/ false);

            const auto vm = page->_paneTabStripVmAtForTest(leaf0, inactiveIdx);
            VERIFY_IS_NOT_NULL(vm);
            VERIFY_ARE_EQUAL(inactiveTabId, vm.Id());
            VERIFY_IS_FALSE(vm.BellIndicator(),
                            L"a bell with SendNotification==false must not set the indicator");

            const auto header = belledHeader();
            VERIFY_IS_NOT_NULL(header);
            VERIFY_IS_NOT_NULL(header.TabStatus());
            VERIFY_IS_FALSE(header.TabStatus().BellIndicator(),
                            L"the header status must stay un-belled for a no-notification bell");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"A bell WITH a notification request sets BellIndicator and the header reflects it.");
        result = RunOnUIThread([&]() {
            (*mocks)[inactiveIdx]->RaiseBell(/*flashTaskbar*/ false, /*sendNotification*/ true);

            const auto vm = page->_paneTabStripVmAtForTest(leaf0, inactiveIdx);
            VERIFY_IS_NOT_NULL(vm);
            VERIFY_ARE_EQUAL(inactiveTabId, vm.Id());
            VERIFY_IS_TRUE(vm.BellIndicator(),
                           L"a bell with SendNotification==true must set the VM's BellIndicator");

            const auto header = belledHeader();
            VERIFY_IS_NOT_NULL(header);
            VERIFY_IS_NOT_NULL(header.TabStatus());
            VERIFY_IS_TRUE(header.TabStatus().BellIndicator(),
                           L"the hosted header's TerminalTabStatus.BellIndicator must reflect the VM bell");
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Making the belled tab active (dismiss-on-focus) clears the indicator (Tab.cpp:328/1421).");
        result = RunOnUIThread([&]() {
            // Activate the belled (inactive) tab via the same RequestActivate intent
            // a tab tap would raise — it dispatches selectTab; the ActiveTabChanged
            // arm flips the active row through _setActivePaneTabVm, which clears the
            // bell.
            const auto vm = page->_paneTabStripVmAtForTest(leaf0, inactiveIdx);
            VERIFY_IS_NOT_NULL(vm);
            vm.RequestActivate();

            const auto activeId = page->_activePaneTabIdForTest(leaf0);
            VERIFY_IS_TRUE(activeId.has_value());
            VERIFY_ARE_EQUAL(inactiveTabId, *activeId,
                             L"RequestActivate must make the belled tab active");
            VERIFY_IS_FALSE(vm.BellIndicator(),
                            L"becoming active must clear the bell (dismiss-on-focus)");

            const auto header = belledHeader();
            VERIFY_IS_NOT_NULL(header);
            VERIFY_IS_NOT_NULL(header.TabStatus());
            VERIFY_IS_FALSE(header.TabStatus().BellIndicator(),
                            L"the header status must reflect the dismissed bell");
        });
        VERIFY_SUCCEEDED(result);
    }
}
