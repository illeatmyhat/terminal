// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include <cmath>

#include "../TerminalApp/TerminalPage.h"
#include "../TerminalApp/TerminalWindow.h"
#include "../TerminalApp/ContentManager.h"
#include "../TerminalApp/ContentRegistry.h"
#include "../TerminalApp/BasicPaneEvents.h"
#include "../TerminalApp/WorkspaceView.h"
#include "../TerminalApp/WorkspaceViewModel.h"
#include "../TerminalApp/PaneTabViewModel.h"
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

        // #43: the keybinding-less _HandleNewTab(sender, nullptr) branch.
        TEST_METHOD(NewTab_FlagOn_NullArgs_AppendsTabThroughModel);
        TEST_METHOD(NewTab_FlagOff_NullArgs_AppendsTabWithoutModel);

        TEST_METHOD(SwitchToTab_FlagOn_ChangesActiveWorkspace);
        TEST_METHOD(SwitchToTab_FlagOff_ChangesSelectedTabWithoutModel);

        // #45/#44: id-based routing. The ActiveWorkspaceChanged /
        // TabDecorationUpdated arms carry a stable WorkspaceId and the view
        // resolves it to the CURRENT classic tab via its own resolver, so
        // routing is correct even when display order != workspace order, and
        // an unknown id is an explicit no-op.
        TEST_METHOD(IdResolver_RoutesToCorrectTab_AfterReorder);
        TEST_METHOD(IdResolver_UnknownId_IsNoOp);

        // Slice 6: decoration + explicit-profile dispatch.
        TEST_METHOD(RenameTab_FlagOn_UpdatesClassicTabAndModel);
        TEST_METHOD(RenameTab_FlagOff_UpdatesClassicTabOnly);
        TEST_METHOD(SetTabColor_FlagOn_UpdatesClassicTabAndModel);
        TEST_METHOD(SetTabColor_FlagOff_UpdatesClassicTabOnly);
        TEST_METHOD(NewTab_FlagOn_ExplicitProfileByName_AppendsTab);
        TEST_METHOD(NewTab_FlagOff_ExplicitProfileByName_AppendsTab);

        // #41: any non-default NewTerminalArgs field (beyond the profile
        // selector) must keep a flag-on new-tab on the classic path.
        // ReloadEnvironmentVariables is the exception: it is set on nearly
        // every launch and equals the default reload behavior, so it routes
        // through the model instead (see method comment / #48).
        TEST_METHOD(NewTab_FlagOn_TabColorField_RoutesClassic);
        TEST_METHOD(NewTab_FlagOn_SessionIdField_RoutesClassic);
        TEST_METHOD(NewTab_FlagOn_AppendCommandLineField_RoutesClassic);
        TEST_METHOD(NewTab_FlagOn_SuppressApplicationTitleField_RoutesClassic);
        TEST_METHOD(NewTab_FlagOn_ColorSchemeField_RoutesClassic);
        TEST_METHOD(NewTab_FlagOn_ElevateField_RoutesClassic);
        TEST_METHOD(NewTab_FlagOn_ReloadEnvironmentVariablesField_RoutesThroughModel);

        // Slice 6 review fixes:
        //  - sender-bypass: flag-on rename/color must honour the
        //    `sender` argument (right-clicked tab) instead of always
        //    routing to the focused tab.
        //  - DuplicateTab observable parity (skipped in the original
        //    slice).
        TEST_METHOD(SetTabColor_FlagOn_RoutesByRightClickedSender_NotFocusedTab);
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

        TEST_CLASS_SETUP(ClassSetup)
        {
            return true;
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
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
        // NewTerminalArgs carries a single non-default field, and assert
        // the model was NOT grown (the new tab took the classic path).
        void _verifyFlagOnNonDefaultFieldRoutesClassic(
            std::function<void(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs&)> setField,
            const wchar_t* fieldLabel);

        winrt::com_ptr<winrt::TerminalApp::implementation::WindowProperties> _windowProperties;
        winrt::com_ptr<winrt::TerminalApp::implementation::ContentManager> _contentManager;
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
            // Classic XAML tab is present
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"startup-replay should produce exactly one classic tab");

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

        // Pre-condition: startup-replay landed one tab.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
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
            // Classic tab strip now has two tabs. This matches the
            // flag-off observable behavior of a default new-tab.
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-on default-profile new-tab should append a classic tab");

            // Phase 1 maps one workspace == one classic tab, so the
            // model now has two workspaces (one per classic tab).
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on new-tab should create a second workspace");

            // Each workspace has exactly one leaf with exactly one tab
            // (the Phase 1 implicit constraint).
            for (const auto& ws : page->_workspaceModelState->workspaces_view())
            {
                const auto leaves = page->_workspaceModelState->leaves(ws.id);
                VERIFY_ARE_EQUAL(1u, leaves.size(),
                                 L"Phase 1 holds exactly one leaf per workspace");
                VERIFY_ARE_EQUAL(1u, leaves[0]->tabs.size(),
                                 L"Phase 1 holds exactly one tab per leaf");
            }
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

    // #43: a keybinding-less / menu invocation reaches _HandleNewTab
    // with a null ActionEventArgs (args == nullptr). Flag-on it must
    // still route through the model — newWorkspace + apply — and land
    // a second classic tab, exactly like the args-carrying default
    // new-tab.
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

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire _HandleNewTab with a null ActionEventArgs (keybinding-less path)");
        result = RunOnUIThread([&page]() {
            page->_HandleNewTab(nullptr, ActionEventArgs{ nullptr });
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-on null-args new-tab should append a classic tab");
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on null-args new-tab should create a second workspace");
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
    void WorkspaceTests::_verifyFlagOnNonDefaultFieldRoutesClassic(
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

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay should produce exactly one workspace");
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

        result = RunOnUIThread([&page, fieldLabel]() {
            // The field is not modelled in Phase 1, so the new tab must
            // take the classic path: the workspace model stays at its
            // single startup workspace rather than growing to two.
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             fieldLabel);
        });
        VERIFY_SUCCEEDED(result);
    }

    void WorkspaceTests::NewTab_FlagOn_TabColorField_RoutesClassic()
    {
        _verifyFlagOnNonDefaultFieldRoutesClassic(
            [](NewTerminalArgs& a) { a.TabColor(winrt::Windows::UI::Color{ 255, 10, 20, 30 }); },
            L"a TabColor override must keep the new tab on the classic path");
    }

    void WorkspaceTests::NewTab_FlagOn_SessionIdField_RoutesClassic()
    {
        _verifyFlagOnNonDefaultFieldRoutesClassic(
            [](NewTerminalArgs& a) { a.SessionId(winrt::guid{ 0x12345678, 0x1234, 0x1234, { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 } }); },
            L"a SessionId override must keep the new tab on the classic path");
    }

    void WorkspaceTests::NewTab_FlagOn_AppendCommandLineField_RoutesClassic()
    {
        _verifyFlagOnNonDefaultFieldRoutesClassic(
            [](NewTerminalArgs& a) { a.AppendCommandLine(true); },
            L"an AppendCommandLine override must keep the new tab on the classic path");
    }

    void WorkspaceTests::NewTab_FlagOn_SuppressApplicationTitleField_RoutesClassic()
    {
        _verifyFlagOnNonDefaultFieldRoutesClassic(
            [](NewTerminalArgs& a) { a.SuppressApplicationTitle(true); },
            L"a SuppressApplicationTitle override must keep the new tab on the classic path");
    }

    void WorkspaceTests::NewTab_FlagOn_ColorSchemeField_RoutesClassic()
    {
        _verifyFlagOnNonDefaultFieldRoutesClassic(
            [](NewTerminalArgs& a) { a.ColorScheme(L"Campbell"); },
            L"a ColorScheme override must keep the new tab on the classic path");
    }

    void WorkspaceTests::NewTab_FlagOn_ElevateField_RoutesClassic()
    {
        _verifyFlagOnNonDefaultFieldRoutesClassic(
            [](NewTerminalArgs& a) { a.Elevate(true); },
            L"an Elevate override must keep the new tab on the classic path");
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

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay should produce exactly one workspace");
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

        result = RunOnUIThread([&page]() {
            // ReloadEnvironmentVariables is not a meaningful per-tab override,
            // so the new tab routes through the model: the workspace count
            // grows from one to two rather than staying on the classic path.
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"a ReloadEnvironmentVariables-only new tab must route through the model");
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
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
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

            // Classic tab strip: the newly created tab is selected.
            VERIFY_ARE_EQUAL(1u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu));
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

            // ...and the classic XAML view agrees.
            VERIFY_ARE_EQUAL(0u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu),
                             L"WorkspaceView should have driven _SelectTab(0) via ActiveWorkspaceChanged");
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"switch is non-structural — tab count must not change");
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

    // #45/#44: the WorkspaceChange arms carry a stable WorkspaceId and the
    // WorkspaceView resolves it to the CURRENT classic tab through its own
    // resolver (_classicTabIndexForWorkspace), NOT a positional cast. This
    // test makes display order disagree with workspace order by reordering
    // the classic tab strip, then drives the two id-bearing arms directly
    // against the view and asserts they hit the right tab object regardless
    // of its current slot. With the old display-index contract these would
    // have routed to the wrong tab.
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

    // AC: "Rename observable parity with flag-off." Firing a RenameTab
    // action with the workspaces flag on must update both the classic
    // Tab's text (matching flag-off) AND the model's customTitle field.
    void WorkspaceTests::RenameTab_FlagOn_UpdatesClassicTabAndModel()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());

            RenameTabArgs renameArgs{ L"renamed" };
            ActionEventArgs eventArgs{ renameArgs };
            page->_HandleRenameTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // Classic Tab observable: GetTabText returns the new title.
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(winrt::hstring{ L"renamed" }, tab->GetTabText(),
                             L"flag-on rename must update the classic Tab text");

            // Model carries the same customTitle on the workspace's
            // only tab.
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto leaves = page->_workspaceModelState->leaves(workspaces[0].id);
            VERIFY_ARE_EQUAL(1u, leaves.size());
            VERIFY_ARE_EQUAL(1u, leaves[0]->tabs.size());
            VERIFY_ARE_EQUAL(std::string{ "renamed" }, leaves[0]->tabs[0].customTitle,
                             L"model state must record the rename");
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

    // AC: "Color observable parity with flag-off." Firing a SetTabColor
    // action with the workspaces flag on must update both the classic
    // Tab's runtime color AND the model's runtimeColor field.
    void WorkspaceTests::SetTabColor_FlagOn_UpdatesClassicTabAndModel()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

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
            VERIFY_IS_TRUE(classic.has_value(), L"flag-on color must populate the classic Tab runtime color");
            VERIFY_ARE_EQUAL(expected.R, classic.value().R);
            VERIFY_ARE_EQUAL(expected.G, classic.value().G);
            VERIFY_ARE_EQUAL(expected.B, classic.value().B);
            VERIFY_ARE_EQUAL(expected.A, classic.value().A);

            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(1u, workspaces.size());
            const auto leaves = page->_workspaceModelState->leaves(workspaces[0].id);
            VERIFY_ARE_EQUAL(1u, leaves.size());
            VERIFY_ARE_EQUAL(1u, leaves[0]->tabs.size());
            const auto& modelColor = leaves[0]->tabs[0].runtimeColor;
            VERIFY_IS_TRUE(modelColor.has_value(), L"model must carry the runtime color");
            VERIFY_ARE_EQUAL(expected.R, modelColor.value().r);
            VERIFY_ARE_EQUAL(expected.G, modelColor.value().g);
            VERIFY_ARE_EQUAL(expected.B, modelColor.value().b);
            VERIFY_ARE_EQUAL(expected.A, modelColor.value().a);
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
    // default profile) with the flag on must:
    //  - Append a second classic tab (matches flag-off).
    //  - Append a second model workspace whose only tab's TerminalSpec
    //    carries profile1's GUID, NOT the zero-GUID sentinel.
    void WorkspaceTests::NewTab_FlagOn_ExplicitProfileByName_AppendsTab()
    {
        CascadiaSettings settings{ settingsJsonFlagOn, {} };
        VERIFY_IS_NOT_NULL(settings);

        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page{ nullptr };
        _initializeTerminalPageWithFlagOn(page, settings);

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
                             L"flag-on explicit-profile new-tab should append a classic tab");

            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on explicit-profile new-tab should create a second workspace");

            // The most-recently-added workspace is the active one.
            const auto activeId = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeId.has_value());
            const auto leaves = page->_workspaceModelState->leaves(activeId.value());
            VERIFY_ARE_EQUAL(1u, leaves.size());
            VERIFY_ARE_EQUAL(1u, leaves[0]->tabs.size());
            const auto& description = leaves[0]->tabs[0].description;
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::TerminalSpec>(description));
            const auto& spec = std::get<::WorkspaceModel::TerminalSpec>(description);

            // The profile bytes must equal profile1's GUID, not the
            // zero-GUID sentinel that default-profile dispatch uses.
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

    // Regression guard for the sender-bypass bug: when a user right-
    // clicks tab 1 in the tab strip context menu while tab 0 is focused,
    // the flag-on path used to call _focusedTabModelId() and mutate tab
    // 0 (the wrong tab) while the classic flag-off path correctly used
    // _senderOrFocusedTab(sender) and mutated tab 1.
    //
    // After the fix, the flag-on path resolves through _modelIdForTab
    // and matches the classic semantics: rename/color on a non-focused
    // tab targets the right-clicked tab.
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
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());

            // Capture the source workspace's tab profile so we can
            // assert the duplicate inherits it.
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
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"flag-on duplicate-tab should append a classic tab");

            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on duplicate-tab should create a second workspace");

            // The most-recently-added workspace is the active one.
            const auto activeId = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeId.has_value());
            const auto leaves = page->_workspaceModelState->leaves(activeId.value());
            VERIFY_ARE_EQUAL(1u, leaves.size());
            VERIFY_ARE_EQUAL(1u, leaves[0]->tabs.size());

            const auto& description = leaves[0]->tabs[0].description;
            VERIFY_IS_TRUE(std::holds_alternative<::WorkspaceModel::TerminalSpec>(description),
                           L"duplicate-tab must produce a TerminalSpec");
            const auto& spec = std::get<::WorkspaceModel::TerminalSpec>(description);

            // Profile inheritance: bytes must match the source.
            VERIFY_IS_TRUE(spec.profile == sourceProfileBytes,
                           L"duplicate-tab must inherit the source tab's profile GUID");
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

    // Slice 3 AC: "Closing one tab in a multi-tab classic state behaves
    // identically to upstream." (Phase 1 maps multi-tab to multi-
    // workspace; closing the first tab leaves the second alive.)
    //
    // Start with two workspaces (one initial + one via NewTab), fire
    // CloseTab on index 0, then assert: _tabs has 1 entry AND the model
    // has 1 workspace AND the model state still validates.
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

        // Stand up a second classic tab (== second workspace) so we can
        // close the first one without triggering the last-tab window-
        // close path.
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire CloseTab(index=0) through the action handler");
        result = RunOnUIThread([&page]() {
            CloseTabArgs closeArgs{ 0u };
            ActionEventArgs eventArgs{ closeArgs };
            page->_HandleCloseTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            // The classic tab strip lost one entry…
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"close-tab should remove the classic tab");
            // …and so did the model.
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"close-tab on flag-on should remove the matching workspace");

            // No validator violations — the cascade and MRU fallback all
            // produced a well-formed model state.
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
    // Drive: start with the single startup-replay tab, fire CloseTab on
    // index 0, capture CloseWindowRequested via subscribing before the
    // action dispatch. The model should be empty AND _tabs should be
    // empty AND the close-window request should have fired exactly
    // once.
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
        auto result = RunOnUIThread([&page, closeWindowRequestCount]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size());

            page->CloseWindowRequested(
                [closeWindowRequestCount](auto&&, auto&&) {
                    closeWindowRequestCount->fetch_add(1);
                });
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire CloseTab(index=0) — the only remaining tab");
        result = RunOnUIThread([&page]() {
            CloseTabArgs closeArgs{ 0u };
            ActionEventArgs eventArgs{ closeArgs };
            page->_HandleCloseTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page, closeWindowRequestCount]() {
            VERIFY_ARE_EQUAL(0u, page->_tabs.Size(),
                             L"closing the last tab leaves the classic tab strip empty");
            VERIFY_ARE_EQUAL(0u, page->_workspaceModelState->workspaces_view().size(),
                             L"closing the last workspace leaves the model empty");
            VERIFY_IS_FALSE(page->_workspaceModelState->activeWorkspaceId_view().has_value(),
                            L"empty model has no active workspace");
            VERIFY_ARE_EQUAL(1, closeWindowRequestCount->load(),
                             L"the last-tab teardown must raise CloseWindowRequested exactly once");

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

        // After a close-tab cascade.
        result = RunOnUIThread([&page]() {
            CloseTabArgs closeArgs{ 0u };
            ActionEventArgs eventArgs{ closeArgs };
            page->_HandleCloseTab(nullptr, eventArgs);

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

        // Pre-condition: startup-replay landed one tab and the model
        // has one workspace bound to it.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"startup-replay should produce exactly one classic tab");
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"startup-replay should produce exactly one workspace");
            VERIFY_ARE_EQUAL(1u, page->_workspaceClassicTabs.size(),
                             L"startup-replay must register the initial classic Tab");
        });
        VERIFY_SUCCEEDED(result);

        // Snapshot the pre-existing Tab and workspace id so we can
        // assert later that nothing rebound the workspace to the wrong
        // tab.
        winrt::TerminalApp::Tab preexistingTab{ nullptr };
        ::WorkspaceModel::WorkspaceId preexistingWs{};
        result = RunOnUIThread([&page, &preexistingTab, &preexistingWs]() {
            preexistingTab = page->_tabs.GetAt(0);
            VERIFY_IS_NOT_NULL(preexistingTab);
            for (const auto& [ws, weakTab] : page->_workspaceClassicTabs)
            {
                preexistingWs = ws;
                break;
            }
            VERIFY_IS_TRUE(preexistingWs.valid());
        });
        VERIFY_SUCCEEDED(result);

        Log::Comment(L"Fire NewTab with an out-of-range ProfileIndex; this should bail without growing _tabs or the model");
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

        result = RunOnUIThread([&page, &preexistingTab, &preexistingWs]() {
            // 1. No zombie tab.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"failed-spawn NewTab must NOT append a classic tab");

            // 2. Model unchanged — the bail happened before model
            //    dispatch, so workspace count stays at 1.
            VERIFY_ARE_EQUAL(1u, page->_workspaceModelState->workspaces_view().size(),
                             L"failed-spawn NewTab must not grow the workspace model");

            // 3. The registry still holds exactly the original
            //    binding. Most importantly, the pre-existing workspace
            //    is still bound to the pre-existing tab — NOT to some
            //    new failed-spawn workspace.
            VERIFY_ARE_EQUAL(1u, page->_workspaceClassicTabs.size(),
                             L"registry must hold exactly the original workspace -> tab binding");
            const auto it = page->_workspaceClassicTabs.find(preexistingWs);
            VERIFY_IS_TRUE(it != page->_workspaceClassicTabs.end(),
                           L"pre-existing workspace must still be in the registry");
            const auto boundTab = it->second.get();
            VERIFY_IS_TRUE(boundTab == preexistingTab,
                           L"pre-existing workspace must still be bound to the pre-existing tab (no mis-bind)");

            // 4. Validator clean.
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
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
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
            // The classic Pane tree on the focused tab grew to 2 leaves.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"split must not create a new classic tab");
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(2, tab->GetLeafPaneCount(),
                             L"classic pane tree must have two leaves after a split");

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

        // Pre-condition: startup-replay landed one tab containing one
        // pane; the model has one workspace whose root is a LeafPane.
        auto result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
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
            // The classic Pane tree on the focused tab grew to 2 leaves.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"split must not create a new classic tab");
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(2, tab->GetLeafPaneCount(),
                             L"classic pane tree must have two leaves after a split");

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

        // Create a second workspace via a default new-tab.
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
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

            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
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

        // Startup gives workspace 0; a NewTab adds workspace 1 and makes it the
        // active one (newWorkspace's contract), selecting classic tab 1.
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(2u, page->_workspaceViewModels.Size());
            const auto& workspaces = page->_workspaceModelState->workspaces_view();
            VERIFY_ARE_EQUAL(static_cast<size_t>(2), workspaces.size());

            // Pre-condition: workspace 1 is active and classic tab 1 is focused.
            const auto activeBefore = page->_workspaceModelState->activeWorkspaceId_view();
            VERIFY_IS_TRUE(activeBefore.has_value());
            VERIFY_ARE_EQUAL(activeBefore.value(), workspaces[1].id,
                             L"workspaces[1] should be active after a NewTab");
            VERIFY_ARE_EQUAL(1u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu));

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

            // ...and the ActiveWorkspaceChanged arm drove _SelectTab to
            // workspace 0's classic tab (tab 0) without changing the tab count.
            VERIFY_ARE_EQUAL(0u, page->_GetFocusedTabIndex().value_or(0xFFFFFFFFu),
                             L"WorkspaceView should have driven _SelectTab(0) via ActiveWorkspaceChanged");
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"a switch is non-structural — the tab count must not change");
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

        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
            VERIFY_ARE_EQUAL(2u, page->_workspaceViewModels.Size());
            const auto vm0 = page->_workspaceViewModels.GetAt(0);
            const auto vm1 = page->_workspaceViewModels.GetAt(1);

            // After NewTab, workspace 1 is active: its row is highlighted, 0 isn't.
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

        // Startup gives workspace 0; two NewTabs add workspaces 1 and 2 so the
        // sidebar has three rows [w0, w1, w2] in declared order.
        auto result = RunOnUIThread([&page]() {
            for (int i = 0; i < 2; ++i)
            {
                NewTerminalArgs newTerminalArgs{};
                NewTabArgs newTabArgs{ newTerminalArgs };
                ActionEventArgs eventArgs{ newTabArgs };
                page->_HandleNewTab(nullptr, eventArgs);
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

    namespace
    {
        // A minimal test-only IPaneContent that stands in for a live
        // TermControl/ConPTY-backed content. It records how many times Close()
        // was called (the registry calls Close() exactly once, on removal —
        // the ConPTY-teardown point) and carries a unique tag so the test can
        // assert that a re-mount resolves the SAME instance the registry kept
        // alive across an unmount.
        struct MockPaneContent : public winrt::implements<MockPaneContent, winrt::TerminalApp::IPaneContent>,
                                 public winrt::TerminalApp::implementation::BasicPaneEvents
        {
            explicit MockPaneContent(uint64_t tag) :
                _tag{ tag } {}

            uint64_t Tag() const noexcept { return _tag; }
            int CloseCount() const noexcept { return _closeCount; }

            // Big-flip Slice B (#54): the host-attach plumbing parents this
            // content's GetRoot() into the (collapsed) WorkspaceContentHost. A
            // headless test asserts that parented element by identity, so the
            // mock must hand back a real, stable FrameworkElement — the SAME
            // instance every call (the registry keeps one content alive across
            // (un)mounts, so its root must be stable too). We lazily build a
            // bare Grid and cache it. Using a real TermControl's root in a
            // headless attach test is unnecessary and couples the test to
            // control geometry; this mock root keeps the attach test about the
            // plumbing only.
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
            winrt::hstring Title() { return L"mock"; }
            uint64_t TaskbarState() { return 0; }
            uint64_t TaskbarProgress() { return 0; }
            bool ReadOnly() { return false; }
            winrt::hstring Icon() { return {}; }
            winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept { return nullptr; }
            winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush() { return nullptr; }
            winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(winrt::TerminalApp::BuildStartupKind) { return nullptr; }
            void Focus(winrt::Windows::UI::Xaml::FocusState) {}
            void Close() { ++_closeCount; }

        private:
            uint64_t _tag{ 0 };
            int _closeCount{ 0 };
            winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr };
        };
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

            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"the classic Tab still displays — exactly one after startup");
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

        Log::Comment(L"Fire a default-profile NewTab (a 2nd, activated workspace)");
        result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
        });
        VERIFY_SUCCEEDED(result);

        // The new workspace is now active and materialised; the first
        // workspace's content stays alive in the registry (mount policy leaves
        // already-materialised tabs untouched), so the registry now owns TWO
        // live contents — one per workspace.
        result = RunOnUIThread([&page, &verifyAllMountsResolve]() {
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"the classic Tab still displays — two after a 2nd workspace");
            VERIFY_ARE_EQUAL(2u, page->_workspaceModelState->workspaces_view().size(),
                             L"flag-on new-tab created a second workspace");

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

        Log::Comment(L"Create a 2nd workspace via a default new-tab");
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
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
                             L"flag-on new-tab created a second workspace");

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

            // The classic display is intact: exactly one classic Tab whose
            // content is the VISIBLE child of _tabContent.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"the classic Tab still displays — exactly one after startup");

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

            // INVISIBLE this slice: the host stays Collapsed, so the user sees
            // only the classic terminal.
            VERIFY_IS_TRUE(page->_workspaceContentHost != nullptr);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must remain Collapsed — no visible change this slice");

            // The classic visible path is intact: _tabContent holds the classic
            // Tab's content (NOT the mock root), and that classic content is a
            // child of _tabContent.
            VERIFY_IS_TRUE(page->_tabContent != nullptr);
            const auto classicContent = page->_tabs.GetAt(0).Content();
            VERIFY_IS_NOT_NULL(classicContent);
            uint32_t classicIndex = 0;
            VERIFY_IS_TRUE(page->_tabContent.Children().IndexOf(classicContent, classicIndex),
                           L"the classic Tab content must still be a child of _tabContent (visible path intact)");
            VERIFY_IS_TRUE(classicContent != expectedRoot,
                           L"the classic visible content is distinct from the host's factory content this slice");
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

        Log::Comment(L"Create a 2nd workspace via a default new-tab (now active)");
        auto result = RunOnUIThread([&page]() {
            NewTerminalArgs newTerminalArgs{};
            NewTabArgs newTabArgs{ newTerminalArgs };
            ActionEventArgs eventArgs{ newTabArgs };
            page->_HandleNewTab(nullptr, eventArgs);
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
                             L"flag-on new-tab created a second workspace");
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"the classic Tab still displays — two after a 2nd workspace");
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

            // The host stays Collapsed; no visible change.
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must remain Collapsed across a switch");

            // Classic path intact: still two classic tabs, and the classic
            // content of the selected tab is a child of _tabContent.
            VERIFY_ARE_EQUAL(2u, page->_tabs.Size(),
                             L"the switch must not change the classic tab count");
            const auto focusedIdx = page->_GetFocusedTabIndex();
            VERIFY_IS_TRUE(focusedIdx.has_value());
            const auto classicContent = page->_tabs.GetAt(*focusedIdx).Content();
            uint32_t classicIndex = 0;
            VERIFY_IS_TRUE(page->_tabContent.Children().IndexOf(classicContent, classicIndex),
                           L"the selected classic Tab content must be a child of _tabContent (visible path intact)");
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

            // Startup baseline: one classic Tab, one workspace, the root leaf's
            // strip has exactly one VM (the startup tab projected by TabAdded).
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"startup: exactly one classic Tab");
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

            // CRITICAL: NO second classic Tab was created — the additional tab
            // is represented ONLY as a strip VM (invisible). _tabs stays at one.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"an additional leaf tab must NOT create a 2nd classic Tab");

            // INVISIBLE: the host (and thus the strip inside it) stays Collapsed.
            VERIFY_IS_TRUE(page->_workspaceContentHost != nullptr);
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must remain Collapsed — no visible change this slice");
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

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across an active-tab change");
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"an active-tab change must not touch the classic tab count");
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

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across the second active-tab change");
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

            // The classic tab count is unchanged — the additional tab never had
            // a classic Tab to remove.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"closing the additional tab must not change the classic tab count");

            // Host still Collapsed.
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across a tab close");
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

            // INVISIBLE + classic intact: one classic Tab, host Collapsed.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"the split must not create a 2nd classic Tab");
            auto tab = page->_GetTabImpl(page->_tabs.GetAt(0));
            VERIFY_IS_NOT_NULL(tab);
            VERIFY_ARE_EQUAL(2, tab->GetLeafPaneCount(),
                             L"the classic pane tree (the visible one) still grew to two leaves");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across the split — no visible change");
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

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across a ratio change");
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

            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across the collapse");
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

            // INVISIBLE: host + tree stay Collapsed, classic Tab unchanged.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size());
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"the host must stay Collapsed — no visible change");
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

            // INVISIBLE + classic intact: one classic Tab, host Collapsed.
            VERIFY_ARE_EQUAL(1u, page->_tabs.Size(),
                             L"the split must not create a 2nd classic Tab");
            VERIFY_ARE_EQUAL(winrt::Windows::UI::Xaml::Visibility::Collapsed,
                             page->_workspaceContentHost.Visibility(),
                             L"host stays Collapsed across the split");
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
}
