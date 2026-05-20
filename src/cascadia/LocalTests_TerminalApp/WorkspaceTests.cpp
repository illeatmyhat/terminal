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

}
