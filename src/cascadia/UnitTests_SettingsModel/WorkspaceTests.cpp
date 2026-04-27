// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalSettingsModel/WorkspaceState.h"
#include "../TerminalSettingsModel/WorkspacePersistence.h"
#include "../TerminalSettingsModel/WorkspaceMigration.h"

#include "JsonTestClass.h"

using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

namespace SettingsModelUnitTests
{
    namespace
    {
        WorkspaceState makeWorkspace(std::wstring title, bool pinned = false)
        {
            WorkspaceState ws;
            ws.title = std::move(title);
            ws.pinned = pinned;
            return ws;
        }

        WorkspaceState makeWorkspaceWithPaneTree(std::wstring title, std::initializer_list<std::string_view> paneActionNames)
        {
            auto ws = makeWorkspace(std::move(title));
            ws.paneTree = Json::Value{ Json::arrayValue };
            for (const auto& name : paneActionNames)
            {
                Json::Value step{ Json::objectValue };
                step["action"] = std::string{ name };
                ws.paneTree.append(std::move(step));
            }
            return ws;
        }
    }

    // (WorkspacePlacement and WorkspaceList tests live in
    // LocalTests_TerminalApp/WorkspaceListTests.cpp, alongside the
    // implementations they cover. This file keeps the persistence + migration
    // tests because those modules stay in TerminalSettingsModel.)

    // -----------------------------------------------------------------------
    // WorkspacePersistence: round-trip + corruption fallback
    // -----------------------------------------------------------------------
    class WorkspacePersistenceTests : public JsonTestClass
    {
        TEST_CLASS(WorkspacePersistenceTests);

        TEST_METHOD(EmptyRoundTrip);
        TEST_METHOD(SingleWorkspaceNoPanes);
        TEST_METHOD(MultipleWorkspacesWithPaneTrees);
        TEST_METHOD(WorkspaceMetadataPreserved);
        TEST_METHOD(SidebarWidthRoundTrips);
        TEST_METHOD(MalformedActionsReturnNullopt);
        TEST_METHOD(MalformedWindowLayoutReturnsNullopt);
    };

    void WorkspacePersistenceTests::EmptyRoundTrip()
    {
        WorkspaceListState state;
        const auto serialized = WorkspacePersistence::SerializeActions(state);
        const auto deserialized = WorkspacePersistence::DeserializeActions(serialized);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(0u, deserialized->workspaces.size());
        VERIFY_IS_FALSE(deserialized->activeIndex.has_value());
    }

    void WorkspacePersistenceTests::SingleWorkspaceNoPanes()
    {
        WorkspaceListState state;
        state.workspaces.push_back(makeWorkspace(L"Solo"));
        state.activeIndex = 0;

        const auto serialized = WorkspacePersistence::SerializeActions(state);
        const auto deserialized = WorkspacePersistence::DeserializeActions(serialized);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(1u, deserialized->workspaces.size());
        VERIFY_ARE_EQUAL(L"Solo", deserialized->workspaces[0].title);
        VERIFY_IS_TRUE(deserialized->activeIndex.has_value());
        VERIFY_ARE_EQUAL(0u, *deserialized->activeIndex);
    }

    void WorkspacePersistenceTests::MultipleWorkspacesWithPaneTrees()
    {
        WorkspaceListState state;
        state.workspaces.push_back(makeWorkspaceWithPaneTree(L"A", { "splitPane", "focusPane" }));
        state.workspaces.push_back(makeWorkspaceWithPaneTree(L"B", { "splitPane", "splitPane", "newPaneTab", "selectPaneTab" }));
        state.workspaces.push_back(makeWorkspaceWithPaneTree(L"C", {}));
        state.activeIndex = 1;

        const auto serialized = WorkspacePersistence::SerializeActions(state);
        const auto deserialized = WorkspacePersistence::DeserializeActions(serialized);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(state.workspaces.size(), deserialized->workspaces.size());
        for (size_t i = 0; i < state.workspaces.size(); ++i)
        {
            VERIFY_ARE_EQUAL(state.workspaces[i].title, deserialized->workspaces[i].title);
            VERIFY_ARE_EQUAL(state.workspaces[i].paneTree.size(), deserialized->workspaces[i].paneTree.size());
        }
        VERIFY_IS_TRUE(deserialized->activeIndex.has_value());
        VERIFY_ARE_EQUAL(*state.activeIndex, *deserialized->activeIndex);
    }

    void WorkspacePersistenceTests::WorkspaceMetadataPreserved()
    {
        WorkspaceListState state;
        WorkspaceState pinned;
        pinned.title = L"Pinned";
        pinned.pinned = true;
        pinned.runtimeColor = winrt::Windows::UI::Color{ 0xFF, 0x33, 0x66, 0x99 };
        pinned.customDescription = L"Long-running build watcher";
        pinned.id = 42;
        state.workspaces.push_back(std::move(pinned));
        state.workspaces.push_back(makeWorkspace(L"Plain"));
        state.activeIndex = 1;

        const auto serialized = WorkspacePersistence::SerializeActions(state);
        const auto deserialized = WorkspacePersistence::DeserializeActions(serialized);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(2u, deserialized->workspaces.size());
        VERIFY_ARE_EQUAL(L"Pinned", deserialized->workspaces[0].title);
        VERIFY_IS_TRUE(deserialized->workspaces[0].pinned);
        VERIFY_IS_TRUE(deserialized->workspaces[0].runtimeColor.has_value());
        VERIFY_ARE_EQUAL(0x33u, deserialized->workspaces[0].runtimeColor->R);
        VERIFY_ARE_EQUAL(0x66u, deserialized->workspaces[0].runtimeColor->G);
        VERIFY_ARE_EQUAL(0x99u, deserialized->workspaces[0].runtimeColor->B);
        VERIFY_ARE_EQUAL(L"Long-running build watcher", deserialized->workspaces[0].customDescription);
        VERIFY_ARE_EQUAL(42u, deserialized->workspaces[0].id);
        VERIFY_IS_FALSE(deserialized->workspaces[1].pinned);
        VERIFY_IS_TRUE(deserialized->activeIndex.has_value());
        VERIFY_ARE_EQUAL(1u, *deserialized->activeIndex);
    }

    void WorkspacePersistenceTests::SidebarWidthRoundTrips()
    {
        WorkspaceListState state;
        state.workspaces.push_back(makeWorkspace(L"A"));
        state.workspaces.push_back(makeWorkspace(L"B"));
        state.activeIndex = 0;
        state.sidebarWidth = 312.5;

        const auto serialized = WorkspacePersistence::SerializeWindowLayout(state);
        const auto deserialized = WorkspacePersistence::DeserializeWindowLayout(serialized);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_IS_TRUE(deserialized->sidebarWidth.has_value());
        VERIFY_ARE_EQUAL(312.5, *deserialized->sidebarWidth);
        VERIFY_ARE_EQUAL(2u, deserialized->workspaces.size());
    }

    void WorkspacePersistenceTests::MalformedActionsReturnNullopt()
    {
        // Not an array
        VERIFY_IS_FALSE(WorkspacePersistence::DeserializeActions(Json::Value{ "garbage" }).has_value());
        // Array of non-objects
        Json::Value badArray{ Json::arrayValue };
        badArray.append(Json::Value{ 5 });
        badArray.append(Json::Value{ "string" });
        VERIFY_IS_FALSE(WorkspacePersistence::DeserializeActions(badArray).has_value());
    }

    void WorkspacePersistenceTests::MalformedWindowLayoutReturnsNullopt()
    {
        // Not an object
        VERIFY_IS_FALSE(WorkspacePersistence::DeserializeWindowLayout(Json::Value{ Json::arrayValue }).has_value());
        // Object with non-array tabLayout
        Json::Value bad{ Json::objectValue };
        bad["tabLayout"] = "not an array";
        VERIFY_IS_FALSE(WorkspacePersistence::DeserializeWindowLayout(bad).has_value());
    }

    // -----------------------------------------------------------------------
    // WorkspaceMigration: forward 1:1 lossless
    // -----------------------------------------------------------------------
    class WorkspaceMigrationTests : public JsonTestClass
    {
        TEST_CLASS(WorkspaceMigrationTests);

        TEST_METHOD(EmptyLegacyBlobYieldsEmpty);
        TEST_METHOD(SingleTabBecomesSingleWorkspace);
        TEST_METHOD(MultipleTabsBecomeMultipleWorkspaces);
        TEST_METHOD(TabTitleAndColorHoistedToWorkspace);
        TEST_METHOD(DeeplyNestedSplitsPreserved);
        TEST_METHOD(IsLegacyShapeDetection);
        TEST_METHOD(WorkspaceShapeIsPassedThroughUnchanged);
    };

    void WorkspaceMigrationTests::EmptyLegacyBlobYieldsEmpty()
    {
        const auto migrated = WorkspaceMigration::MigrateLegacyTabLayout(Json::Value{ Json::arrayValue });
        VERIFY_IS_TRUE(migrated.isArray());
        VERIFY_ARE_EQUAL(0u, migrated.size());
    }

    void WorkspaceMigrationTests::SingleTabBecomesSingleWorkspace()
    {
        Json::Value legacy{ Json::arrayValue };
        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        newTab["profile"] = "{guid}";
        legacy.append(newTab);

        const auto migrated = WorkspaceMigration::MigrateLegacyTabLayout(legacy);
        const auto state = WorkspacePersistence::DeserializeActions(migrated);
        VERIFY_IS_TRUE(state.has_value());
        VERIFY_ARE_EQUAL(1u, state->workspaces.size());
    }

    void WorkspaceMigrationTests::MultipleTabsBecomeMultipleWorkspaces()
    {
        Json::Value legacy{ Json::arrayValue };
        for (const auto& title : { "alpha", "beta", "gamma" })
        {
            Json::Value newTab{ Json::objectValue };
            newTab["action"] = "newTab";
            newTab["tabTitle"] = title;
            legacy.append(newTab);
            // Each tab has a split.
            Json::Value split{ Json::objectValue };
            split["action"] = "splitPane";
            legacy.append(split);
        }

        const auto migrated = WorkspaceMigration::MigrateLegacyTabLayout(legacy);
        const auto state = WorkspacePersistence::DeserializeActions(migrated);
        VERIFY_IS_TRUE(state.has_value());
        VERIFY_ARE_EQUAL(3u, state->workspaces.size());
        VERIFY_ARE_EQUAL(L"alpha", state->workspaces[0].title);
        VERIFY_ARE_EQUAL(L"beta", state->workspaces[1].title);
        VERIFY_ARE_EQUAL(L"gamma", state->workspaces[2].title);
        for (const auto& ws : state->workspaces)
        {
            // Each workspace's pane tree contains the original newTab (carrying
            // profile/cwd) and the splitPane that followed.
            VERIFY_IS_TRUE(ws.paneTree.isArray());
            VERIFY_ARE_EQUAL(2u, ws.paneTree.size());
        }
    }

    void WorkspaceMigrationTests::TabTitleAndColorHoistedToWorkspace()
    {
        Json::Value legacy{ Json::arrayValue };
        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        newTab["tabTitle"] = "with-color";
        newTab["tabColor"] = "#3B82F6";
        legacy.append(newTab);

        const auto migrated = WorkspaceMigration::MigrateLegacyTabLayout(legacy);
        const auto state = WorkspacePersistence::DeserializeActions(migrated);
        VERIFY_IS_TRUE(state.has_value());
        VERIFY_ARE_EQUAL(1u, state->workspaces.size());
        VERIFY_ARE_EQUAL(L"with-color", state->workspaces[0].title);
        VERIFY_IS_TRUE(state->workspaces[0].runtimeColor.has_value());
        VERIFY_ARE_EQUAL(0x3Bu, state->workspaces[0].runtimeColor->R);
        VERIFY_ARE_EQUAL(0x82u, state->workspaces[0].runtimeColor->G);
        VERIFY_ARE_EQUAL(0xF6u, state->workspaces[0].runtimeColor->B);
    }

    void WorkspaceMigrationTests::DeeplyNestedSplitsPreserved()
    {
        Json::Value legacy{ Json::arrayValue };
        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        legacy.append(newTab);
        // Five splits and a focus.
        for (int i = 0; i < 5; ++i)
        {
            Json::Value step{ Json::objectValue };
            step["action"] = "splitPane";
            step["split"] = (i % 2 == 0) ? "right" : "down";
            legacy.append(step);
        }
        Json::Value focus{ Json::objectValue };
        focus["action"] = "focusPane";
        focus["id"] = 3;
        legacy.append(focus);

        const auto migrated = WorkspaceMigration::MigrateLegacyTabLayout(legacy);
        const auto state = WorkspacePersistence::DeserializeActions(migrated);
        VERIFY_IS_TRUE(state.has_value());
        VERIFY_ARE_EQUAL(1u, state->workspaces.size());
        // 1 newTab + 5 splitPane + 1 focusPane = 7 actions in the pane tree.
        VERIFY_ARE_EQUAL(7u, state->workspaces[0].paneTree.size());
    }

    void WorkspaceMigrationTests::IsLegacyShapeDetection()
    {
        Json::Value legacyShape{ Json::arrayValue };
        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        legacyShape.append(newTab);
        VERIFY_IS_TRUE(WorkspaceMigration::IsLegacyShape(legacyShape));

        Json::Value workspaceShape{ Json::arrayValue };
        Json::Value newWorkspace{ Json::objectValue };
        newWorkspace["action"] = "newWorkspace";
        workspaceShape.append(newWorkspace);
        VERIFY_IS_FALSE(WorkspaceMigration::IsLegacyShape(workspaceShape));
    }

    void WorkspaceMigrationTests::WorkspaceShapeIsPassedThroughUnchanged()
    {
        Json::Value windowLayout{ Json::objectValue };
        Json::Value tabLayout{ Json::arrayValue };
        Json::Value newWorkspace{ Json::objectValue };
        newWorkspace["action"] = "newWorkspace";
        newWorkspace["title"] = "already-migrated";
        tabLayout.append(newWorkspace);
        windowLayout["tabLayout"] = tabLayout;

        const auto result = WorkspaceMigration::MigrateWindowLayoutIfLegacy(windowLayout);
        VERIFY_IS_TRUE(windowLayout == result);
    }

    // -----------------------------------------------------------------------
    // WindowLayout integration: SidebarWidth + WorkspaceLayout round-trip
    // through state.json's WindowLayout::ToJson / FromJson, then through
    // WorkspacePersistence::DeserializeActions.
    // -----------------------------------------------------------------------
    class WorkspaceWindowLayoutIntegrationTests : public JsonTestClass
    {
        TEST_CLASS(WorkspaceWindowLayoutIntegrationTests);

        TEST_METHOD(SidebarWidthRoundTrips);
        TEST_METHOD(WorkspaceLayoutRoundTripsThroughWindowLayout);
        TEST_METHOD(EmptyWorkspaceLayoutOmittedFromJson);
        TEST_METHOD(MigrateLegacyTabLayoutPopulatesWorkspaceLayout);
        TEST_METHOD(MigrateIsNoOpWhenWorkspaceLayoutAlreadyPresent);
        TEST_METHOD(MigrateIsNoOpWhenTabLayoutEmpty);
        TEST_METHOD(FreshDefaultCascadeShapeRoundTrips);
    };

    void WorkspaceWindowLayoutIntegrationTests::SidebarWidthRoundTrips()
    {
        winrt::Microsoft::Terminal::Settings::Model::WindowLayout layout;
        layout.SidebarWidth(220.0);

        const auto serialized = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::ToJson(layout);
        const auto parsed = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::FromJson(serialized);

        VERIFY_IS_NOT_NULL(parsed.SidebarWidth());
        VERIFY_ARE_EQUAL(220.0, parsed.SidebarWidth().Value());
    }

    void WorkspaceWindowLayoutIntegrationTests::WorkspaceLayoutRoundTripsThroughWindowLayout()
    {
        WorkspaceListState state;
        state.workspaces.push_back(makeWorkspaceWithPaneTree(L"alpha", { "splitPane", "focusPane" }));
        state.workspaces.push_back(makeWorkspaceWithPaneTree(L"beta", {}));
        state.activeIndex = 1;
        state.sidebarWidth = 312.5;

        const auto layoutJson = WorkspacePersistence::SerializeWindowLayout(state);
        const auto serializedBlob = JsonTestClass::toString(layoutJson);

        winrt::Microsoft::Terminal::Settings::Model::WindowLayout layout;
        layout.SidebarWidth(312.5);
        layout.WorkspaceLayout(winrt::hstring{ til::u8u16(serializedBlob) });

        const auto serialized = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::ToJson(layout);
        const auto parsed = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::FromJson(serialized);

        VERIFY_IS_FALSE(parsed.WorkspaceLayout().empty());

        // Re-parse the round-tripped blob and assert the workspace state survived.
        const auto roundTrippedJson = JsonTestClass::VerifyParseSucceeded(til::u16u8(parsed.WorkspaceLayout()));
        const auto deserialized = WorkspacePersistence::DeserializeWindowLayout(roundTrippedJson);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(2u, deserialized->workspaces.size());
        VERIFY_ARE_EQUAL(L"alpha", deserialized->workspaces[0].title);
        VERIFY_ARE_EQUAL(L"beta", deserialized->workspaces[1].title);
        VERIFY_IS_TRUE(deserialized->activeIndex.has_value());
        VERIFY_ARE_EQUAL(1u, *deserialized->activeIndex);
        VERIFY_IS_TRUE(deserialized->sidebarWidth.has_value());
        VERIFY_ARE_EQUAL(312.5, *deserialized->sidebarWidth);
    }

    void WorkspaceWindowLayoutIntegrationTests::EmptyWorkspaceLayoutOmittedFromJson()
    {
        // A WindowLayout with no workspace state should not emit the
        // workspaceLayout key at all — non-workspace-mode users shouldn't
        // accumulate empty keys in their state.json.
        winrt::Microsoft::Terminal::Settings::Model::WindowLayout layout;
        layout.WorkspaceLayout(L"");

        const auto serialized = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::ToJson(layout);
        const auto parsed = JsonTestClass::VerifyParseSucceeded(til::u16u8(serialized));
        VERIFY_IS_FALSE(parsed.isMember("workspaceLayout"));
    }

    void WorkspaceWindowLayoutIntegrationTests::MigrateLegacyTabLayoutPopulatesWorkspaceLayout()
    {
        // Construct a legacy-shape WindowLayout via FromJson — two top-level
        // newTab actions, each with a tabTitle. After migration the
        // WorkspaceLayout field should hold a workspace-shape action array
        // with two newWorkspace headers carrying those titles.
        const auto legacyBlob = LR"({
            "tabLayout": [
                { "action": "newTab", "tabTitle": "alpha" },
                { "action": "splitPane", "split": "right" },
                { "action": "newTab", "tabTitle": "beta" }
            ]
        })";

        auto layout = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::FromJson(legacyBlob);
        VERIFY_IS_TRUE(layout.WorkspaceLayout().empty());

        const auto migrated = layout.MigrateLegacyTabLayoutToWorkspaceLayout();
        VERIFY_IS_TRUE(migrated);
        VERIFY_IS_FALSE(layout.WorkspaceLayout().empty());

        // Re-parse the migrated blob and assert it round-trips through
        // WorkspacePersistence as two named workspaces.
        const auto reparsed = JsonTestClass::VerifyParseSucceeded(til::u16u8(layout.WorkspaceLayout()));
        const auto deserialized = WorkspacePersistence::DeserializeActions(reparsed);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(2u, deserialized->workspaces.size());
        VERIFY_ARE_EQUAL(L"alpha", deserialized->workspaces[0].title);
        VERIFY_ARE_EQUAL(L"beta", deserialized->workspaces[1].title);
    }

    void WorkspaceWindowLayoutIntegrationTests::MigrateIsNoOpWhenWorkspaceLayoutAlreadyPresent()
    {
        // If WorkspaceLayout is already populated, migration must not
        // overwrite it — that would be data loss for users already in
        // workspace mode.
        const auto legacyBlob = LR"({
            "tabLayout": [
                { "action": "newTab", "tabTitle": "should-be-ignored" }
            ],
            "workspaceLayout": "[{\"action\":\"newWorkspace\",\"title\":\"preserved\"}]"
        })";

        auto layout = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::FromJson(legacyBlob);
        const auto preMigration = layout.WorkspaceLayout();
        VERIFY_IS_FALSE(preMigration.empty());

        const auto migrated = layout.MigrateLegacyTabLayoutToWorkspaceLayout();
        VERIFY_IS_FALSE(migrated);
        VERIFY_ARE_EQUAL(preMigration, layout.WorkspaceLayout());
    }

    void WorkspaceWindowLayoutIntegrationTests::MigrateIsNoOpWhenTabLayoutEmpty()
    {
        // A blank-slate WindowLayout (no tabs persisted yet) has nothing to
        // migrate; the WorkspaceLayout should stay empty so non-workspace
        // users don't accumulate the key in their state.json.
        winrt::Microsoft::Terminal::Settings::Model::WindowLayout layout;

        const auto migrated = layout.MigrateLegacyTabLayoutToWorkspaceLayout();
        VERIFY_IS_FALSE(migrated);
        VERIFY_IS_TRUE(layout.WorkspaceLayout().empty());
    }

    // (JSON-level idempotency on already-workspace-shaped tabLayout is covered
    // by WorkspaceMigrationTests::WorkspaceShapeIsPassedThroughUnchanged.
    // We don't repeat it through WindowLayout because the typed
    // IVector<ActionAndArgs> path would silently drop unknown action names
    // like `newWorkspace` during deserialization — that scenario can't
    // arise from real persisted state, where workspace data lives in the
    // WorkspaceLayout blob instead of the typed tabLayout.)

    void WorkspaceWindowLayoutIntegrationTests::FreshDefaultCascadeShapeRoundTrips()
    {
        // Spec line 196: on persisted-state corruption, the app falls back to
        // a fresh window with one default-profile workspace. This test pins
        // the *shape* that fallback produces (mirroring TerminalPage::
        // _BootstrapDefaultWorkspace) and verifies it survives a serialize →
        // WindowLayout → reparse → deserialize round-trip without losing the
        // profile-guid-bearing newTab action. If the bootstrap shape ever
        // diverges from what persistence can carry, the cascade-endpoint's
        // next-launch state will silently drop the profile and this test
        // will catch it before users do.
        WorkspaceListState state;

        WorkspaceState ws;
        ws.id = 1;
        ws.pinned = false;
        ws.title = L"Default";
        Json::Value newTab{ Json::objectValue };
        newTab["action"] = "newTab";
        newTab["profile"] = "{61c54bbd-c2c6-5271-96e7-009a87ff44bf}";
        ws.paneTree = Json::Value{ Json::arrayValue };
        ws.paneTree.append(std::move(newTab));
        state.workspaces.push_back(std::move(ws));
        state.activeIndex = 0;

        const auto layoutJson = WorkspacePersistence::SerializeWindowLayout(state);
        const auto serializedBlob = JsonTestClass::toString(layoutJson);

        winrt::Microsoft::Terminal::Settings::Model::WindowLayout layout;
        layout.WorkspaceLayout(winrt::hstring{ til::u8u16(serializedBlob) });

        const auto serialized = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::ToJson(layout);
        const auto parsed = winrt::Microsoft::Terminal::Settings::Model::WindowLayout::FromJson(serialized);

        VERIFY_IS_FALSE(parsed.WorkspaceLayout().empty());

        const auto roundTrippedJson = JsonTestClass::VerifyParseSucceeded(til::u16u8(parsed.WorkspaceLayout()));
        const auto deserialized = WorkspacePersistence::DeserializeWindowLayout(roundTrippedJson);
        VERIFY_IS_TRUE(deserialized.has_value());
        VERIFY_ARE_EQUAL(1u, deserialized->workspaces.size());

        const auto& roundTripped = deserialized->workspaces[0];
        VERIFY_ARE_EQUAL(L"Default", roundTripped.title);
        VERIFY_IS_FALSE(roundTripped.pinned);
        VERIFY_IS_TRUE(roundTripped.paneTree.isArray());
        VERIFY_ARE_EQUAL(1u, roundTripped.paneTree.size());
        VERIFY_ARE_EQUAL("newTab", roundTripped.paneTree[0u]["action"].asString());
        VERIFY_ARE_EQUAL("{61c54bbd-c2c6-5271-96e7-009a87ff44bf}",
                         roundTripped.paneTree[0u]["profile"].asString());
        VERIFY_IS_TRUE(deserialized->activeIndex.has_value());
        VERIFY_ARE_EQUAL(0u, *deserialized->activeIndex);
    }

    // ApplicationState corruption-fallback unit tests are deferred — they
    // require constructing an `ApplicationState` impl in the test process,
    // which pulls in til/mutex.h and til/throttled_func.h transitively.
    // Those headers don't have `#pragma once` and the test pch can't safely
    // include them without colliding with the lib's own pch chain. Cover
    // the corruption path with an integration test once the toast UI lands.
}

#if 0 // legacy: ApplicationState corruption-fallback tests, reactivate when
      // til-include scaffolding allows the test to instantiate the impl.
namespace SettingsModelUnitTests
{
    class ApplicationStateCorruptionTests
    {
        TEST_CLASS(ApplicationStateCorruptionTests);

        TEST_METHOD(QuarantineRunsOnUnparseableSharedState);
        TEST_METHOD(NoQuarantineForValidJson);
        TEST_METHOD(AcknowledgeClearsFlag);

    private:
        static std::filesystem::path makeScratch(std::wstring_view name)
        {
            auto dir = std::filesystem::temp_directory_path() /
                       (L"wt-app-state-" + std::wstring{ name } + L"-" +
                        std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count()));
            std::filesystem::create_directories(dir);
            return dir;
        }

        static void writeFile(const std::filesystem::path& path, std::string_view content)
        {
            std::ofstream out{ path, std::ios::binary };
            out.write(content.data(), gsl::narrow<std::streamsize>(content.size()));
        }
    };

    void ApplicationStateCorruptionTests::QuarantineRunsOnUnparseableSharedState()
    {
        const auto scratch = makeScratch(L"corrupt");
        const auto stateFile = scratch / L"state.json";
        writeFile(stateFile, "{ this is not json");

        {
            const auto state = winrt::make_self<implementation::ApplicationState>(scratch);
            VERIFY_IS_TRUE(state->RecoveredFromCorruption());

            // The original file should have been moved aside, and at least
            // one sibling with a `.corrupt.` infix should exist.
            VERIFY_IS_FALSE(std::filesystem::exists(stateFile));
            bool foundQuarantined = false;
            for (const auto& entry : std::filesystem::directory_iterator{ scratch })
            {
                const auto name = entry.path().filename().wstring();
                if (name.find(L"state.json.corrupt.") != std::wstring::npos)
                {
                    foundQuarantined = true;
                    break;
                }
            }
            VERIFY_IS_TRUE(foundQuarantined);
        }

        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }

    void ApplicationStateCorruptionTests::NoQuarantineForValidJson()
    {
        const auto scratch = makeScratch(L"valid");
        const auto stateFile = scratch / L"state.json";
        writeFile(stateFile, "{}");

        {
            const auto state = winrt::make_self<implementation::ApplicationState>(scratch);
            VERIFY_IS_FALSE(state->RecoveredFromCorruption());
            VERIFY_IS_TRUE(std::filesystem::exists(stateFile));
        }

        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }

    void ApplicationStateCorruptionTests::AcknowledgeClearsFlag()
    {
        const auto scratch = makeScratch(L"ack");
        const auto stateFile = scratch / L"state.json";
        writeFile(stateFile, "not json");

        {
            const auto state = winrt::make_self<implementation::ApplicationState>(scratch);
            VERIFY_IS_TRUE(state->RecoveredFromCorruption());
            state->AcknowledgeCorruptionRecovery();
            VERIFY_IS_FALSE(state->RecoveredFromCorruption());
        }

        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }
}
#endif
