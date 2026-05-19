// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for the 10 workspace-lifecycle actions in WorkspaceActions.h.

#include "pch.h"

#include "TestHelpers.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    class WorkspaceActionTests
    {
        TEST_CLASS(WorkspaceActionTests);

        TEST_METHOD(NewWorkspace_OnEmpty_BecomesActiveAndMru);
        TEST_METHOD(NewWorkspace_AppendsAndMovesActive);
        TEST_METHOD(NewWorkspace_AllocatesMonotonicIds);
        TEST_METHOD(NewWorkspace_CarriesRichTabFields);

        TEST_METHOD(CloseWorkspace_RemovesFromList);
        TEST_METHOD(CloseWorkspace_FallsBackToMruNext);
        TEST_METHOD(CloseWorkspace_LastOne_LeavesEmptyState);
        TEST_METHOD(CloseWorkspace_UnknownId_NoChange);

        TEST_METHOD(CloseOtherWorkspaces_KeepsOnlyNamed);
        TEST_METHOD(CloseOtherWorkspaces_UnknownKeep_NoChange);
        TEST_METHOD(CloseAllWorkspaces_LeavesEmpty);

        TEST_METHOD(SwitchToWorkspace_UpdatesActiveAndMru);
        TEST_METHOD(SwitchToWorkspace_UnknownId_NoChange);

        TEST_METHOD(RenameWorkspace_UpdatesName);
        TEST_METHOD(SetWorkspaceColor_RoundTrips);
        TEST_METHOD(SetWorkspaceDescription_RoundTrips);
        TEST_METHOD(SetWorkspacePinned_RoundTrips);

        TEST_METHOD(ReorderWorkspace_MovesEntry);
        TEST_METHOD(ReorderWorkspace_ClampsOutOfRange);
        TEST_METHOD(ReorderWorkspace_SameIndex_NoChange);
    };

    // -----------------------------------------------------------------
    void WorkspaceActionTests::NewWorkspace_OnEmpty_BecomesActiveAndMru()
    {
        auto r = newWorkspace(emptyModel(), "alpha", termSpec(1));
        VERIFY_IS_FALSE(validate(*r.state).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), r.state->workspaces.size());
        VERIFY_IS_TRUE(r.state->activeWorkspaceId.has_value());
        VERIFY_IS_TRUE(*r.state->activeWorkspaceId == r.id);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), r.state->mru.size());
        VERIFY_IS_TRUE(r.state->mru.front() == r.id);
    }

    void WorkspaceActionTests::NewWorkspace_AppendsAndMovesActive()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        VERIFY_IS_FALSE(validate(*b.state).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), b.state->workspaces.size());
        // The second workspace is the active one; the first is older.
        VERIFY_IS_TRUE(*b.state->activeWorkspaceId == b.id);
        VERIFY_IS_TRUE(b.state->mru.front() == b.id);
        VERIFY_IS_TRUE(b.state->mru.back() == a.id);
    }

    void WorkspaceActionTests::NewWorkspace_AllocatesMonotonicIds()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        VERIFY_IS_TRUE(a.id.v < b.id.v);
        // After two newWorkspace calls (each allocating ws+leaf+tab), the
        // counter should be at least 6.
        VERIFY_IS_TRUE(b.state->idCounter >= 6);
    }

    void WorkspaceActionTests::NewWorkspace_CarriesRichTabFields()
    {
        Color red{ 255, 0, 0, 255 };
        auto r = newWorkspace(emptyModel(), "alpha", termSpec(7), "MyTitle", red, /*pinned=*/true);
        VERIFY_IS_FALSE(validate(*r.state).has_value());
        const auto& leaf = std::get<LeafPane>(r.state->workspaces[0].root);
        const auto& tab = leaf.tabs[0];
        VERIFY_ARE_EQUAL(std::string{ "MyTitle" }, tab.customTitle);
        VERIFY_IS_TRUE(tab.runtimeColor.has_value());
        VERIFY_IS_TRUE(*tab.runtimeColor == red);
        VERIFY_IS_TRUE(tab.pinned);
    }

    // -----------------------------------------------------------------
    void WorkspaceActionTests::CloseWorkspace_RemovesFromList()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto next = closeWorkspace(b.state, a.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
        VERIFY_IS_TRUE(next->workspaces[0].id == b.id);
    }

    void WorkspaceActionTests::CloseWorkspace_FallsBackToMruNext()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // Active is `b`. Close it — falls back to MRU-next (= `a`).
        auto next = closeWorkspace(b.state, b.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->activeWorkspaceId.has_value());
        VERIFY_IS_TRUE(*next->activeWorkspaceId == a.id);
    }

    void WorkspaceActionTests::CloseWorkspace_LastOne_LeavesEmptyState()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto next = closeWorkspace(a.state, a.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces.empty());
        VERIFY_IS_FALSE(next->activeWorkspaceId.has_value());
        VERIFY_IS_TRUE(next->mru.empty());
    }

    void WorkspaceActionTests::CloseWorkspace_UnknownId_NoChange()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto next = closeWorkspace(a.state, WorkspaceId{ 999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
    }

    // -----------------------------------------------------------------
    void WorkspaceActionTests::CloseOtherWorkspaces_KeepsOnlyNamed()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));
        auto next = closeOtherWorkspaces(c.state, b.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
        VERIFY_IS_TRUE(next->workspaces[0].id == b.id);
        VERIFY_IS_TRUE(*next->activeWorkspaceId == b.id);
    }

    void WorkspaceActionTests::CloseOtherWorkspaces_UnknownKeep_NoChange()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto next = closeOtherWorkspaces(b.state, WorkspaceId{ 999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), next->workspaces.size());
    }

    void WorkspaceActionTests::CloseAllWorkspaces_LeavesEmpty()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto next = closeAllWorkspaces(b.state);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces.empty());
        VERIFY_IS_FALSE(next->activeWorkspaceId.has_value());
        VERIFY_IS_TRUE(next->mru.empty());
    }

    // -----------------------------------------------------------------
    void WorkspaceActionTests::SwitchToWorkspace_UpdatesActiveAndMru()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // Active is b; switch to a.
        auto next = switchToWorkspace(b.state, a.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(*next->activeWorkspaceId == a.id);
        VERIFY_IS_TRUE(next->mru.front() == a.id);
    }

    void WorkspaceActionTests::SwitchToWorkspace_UnknownId_NoChange()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto next = switchToWorkspace(a.state, WorkspaceId{ 999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(*next->activeWorkspaceId == a.id);
    }

    // -----------------------------------------------------------------
    void WorkspaceActionTests::RenameWorkspace_UpdatesName()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto next = renameWorkspace(a.state, a.id, "Renamed");
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(std::string{ "Renamed" }, next->workspaces[0].name);
    }

    void WorkspaceActionTests::SetWorkspaceColor_RoundTrips()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        Color blue{ 0, 0, 255, 255 };
        auto next = setWorkspaceColor(a.state, a.id, blue);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces[0].color.has_value());
        VERIFY_IS_TRUE(*next->workspaces[0].color == blue);

        auto cleared = setWorkspaceColor(next, a.id, std::nullopt);
        VERIFY_IS_FALSE(cleared->workspaces[0].color.has_value());
    }

    void WorkspaceActionTests::SetWorkspaceDescription_RoundTrips()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto next = setWorkspaceDescription(a.state, a.id, "An alpha workspace");
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(std::string{ "An alpha workspace" }, next->workspaces[0].customDescription);
    }

    void WorkspaceActionTests::SetWorkspacePinned_RoundTrips()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto pinned = setWorkspacePinned(a.state, a.id, true);
        VERIFY_IS_FALSE(validate(*pinned).has_value());
        VERIFY_IS_TRUE(pinned->workspaces[0].pinned);
        auto unpinned = setWorkspacePinned(pinned, a.id, false);
        VERIFY_IS_FALSE(unpinned->workspaces[0].pinned);
    }

    // -----------------------------------------------------------------
    void WorkspaceActionTests::ReorderWorkspace_MovesEntry()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        auto c = newWorkspace(b.state, "c", termSpec(3));
        // workspaces is [a, b, c]. Move c to index 0.
        auto next = reorderWorkspace(c.state, c.id, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces[0].id == c.id);
        VERIFY_IS_TRUE(next->workspaces[1].id == a.id);
        VERIFY_IS_TRUE(next->workspaces[2].id == b.id);
    }

    void WorkspaceActionTests::ReorderWorkspace_ClampsOutOfRange()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // Pass an out-of-range index; should clamp to last (= 1).
        auto next = reorderWorkspace(b.state, a.id, 999);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces[0].id == b.id);
        VERIFY_IS_TRUE(next->workspaces[1].id == a.id);
    }

    void WorkspaceActionTests::ReorderWorkspace_SameIndex_NoChange()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // Move a to its current index (0).
        auto next = reorderWorkspace(b.state, a.id, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces[0].id == a.id);
        VERIFY_IS_TRUE(next->workspaces[1].id == b.id);
    }
}
