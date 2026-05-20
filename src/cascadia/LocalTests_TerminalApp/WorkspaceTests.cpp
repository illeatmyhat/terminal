// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/TerminalPage.h"
#include "../TerminalApp/TerminalWindow.h"
#include "../TerminalApp/ContentManager.h"
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

        TEST_METHOD(SwitchToTab_FlagOn_ChangesActiveWorkspace);
        TEST_METHOD(SwitchToTab_FlagOff_ChangesSelectedTabWithoutModel);

        // Slice 6: decoration + explicit-profile dispatch.
        TEST_METHOD(RenameTab_FlagOn_UpdatesClassicTabAndModel);
        TEST_METHOD(RenameTab_FlagOff_UpdatesClassicTabOnly);
        TEST_METHOD(SetTabColor_FlagOn_UpdatesClassicTabAndModel);
        TEST_METHOD(SetTabColor_FlagOff_UpdatesClassicTabOnly);
        TEST_METHOD(NewTab_FlagOn_ExplicitProfileByName_AppendsTab);
        TEST_METHOD(NewTab_FlagOff_ExplicitProfileByName_AppendsTab);

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

        TEST_CLASS_SETUP(ClassSetup)
        {
            return true;
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
            return true;
        }

    private:
        void _initializeTerminalPageWithFlagOn(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                               CascadiaSettings initialSettings);
        void _initializeTerminalPageWithFlagOff(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
                                                CascadiaSettings initialSettings);

        winrt::com_ptr<winrt::TerminalApp::implementation::WindowProperties> _windowProperties;
        winrt::com_ptr<winrt::TerminalApp::implementation::ContentManager> _contentManager;
    };

    // Mirror of TabTests::_initializeTerminalPage, but seeds the
    // experimental.workspaces.enabled flag as ON in the global
    // settings. After Create() the page picks the flag-on path and
    // routes the startup NewTab action through WorkspaceActions ->
    // diff -> WorkspaceView -> _openDefaultTabForWorkspace.
    void WorkspaceTests::_initializeTerminalPageWithFlagOn(winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage>& page,
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
        result = RunOnUIThread([&page]() {
            VERIFY_IS_NOT_NULL(page);
            VERIFY_IS_NOT_NULL(page->_settings);
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
            SwitchToTabArgs args{ 0 };
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
            SwitchToTabArgs args{ 0 };
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
            NewTerminalArgs newTerminalArgs{};
            // Settings has exactly one active profile, so ProfileIndex
            // 999 is guaranteed out of range and
            // _shouldBailForInvalidProfileIndex returns true.
            newTerminalArgs.ProfileIndex(winrt::Windows::Foundation::IReference<int32_t>{ 999 });
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
            VERIFY_ARE_EQUAL(splitId.value, split->id.value,
                             L"split id must be preserved across resize");
            VERIFY_IS_GREATER_THAN(split->ratio, preRatio,
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
            VERIFY_IS_LESS_THAN(delta, 0.0001,
                                L"ResizeDirection::Left must shrink the first/left child's ratio back");
            VERIFY_IS_GREATER_THAN(delta, -0.0001,
                                   L"and not overshoot");
        });
        VERIFY_SUCCEEDED(result);
    }

    // Strangler-fig contract pin for resize: flag-off split must keep
    // the model machinery dormant. The classic _ResizePane direction-
    // based path is exercised by TabTests; we just verify the flag-off
    // _HandleResizePane / _ResizePane sequence leaves the model state
    // untouched.
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
            // Split classically so a separator exists; then exercise
            // _ResizePane (the same entry point _HandleResizePane uses
            // post-direction-check).
            SplitPaneArgs splitArgs{ SplitDirection::Right, NewTerminalArgs{} };
            ActionEventArgs splitEventArgs{ splitArgs };
            page->_HandleSplitPane(nullptr, splitEventArgs);

            (void)page->_ResizePane(ResizeDirection::Right);
        });
        VERIFY_SUCCEEDED(result);

        result = RunOnUIThread([&page]() {
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
                        VERIFY_ARE_EQUAL(dstLeafId.value, moved->dstLeafId.value,
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
}
