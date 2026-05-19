// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for the 10 const-member queries on WorkspaceModelData.
//
// The naming compromise (workspaces_view / activeWorkspaceId_view /
// mru_view / sidebarWidth_view) is documented in Mutators.h / WorkspaceState.h
// — these methods coexist with same-named public fields so the rich
// reading API doesn't break Slice 1's direct field access in tests.

#include "pch.h"

#include "TestHelpers.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    class QueryTests
    {
        TEST_CLASS(QueryTests);

        TEST_METHOD(Workspaces_ReturnsDisplayOrderVector);
        TEST_METHOD(Workspace_FindsById);
        TEST_METHOD(Workspace_ReturnsNullForUnknown);
        TEST_METHOD(ActiveWorkspaceId_FollowsState);
        TEST_METHOD(Mru_FrontIsMostRecent);
        TEST_METHOD(Pane_FindsLeafById);
        TEST_METHOD(Pane_FindsSplitById);
        TEST_METHOD(Pane_ReturnsNullForUnknown);
        TEST_METHOD(ParentOf_LeafChildOfSplit);
        TEST_METHOD(ParentOf_RootReturnsNull);
        TEST_METHOD(Leaves_ReturnsAllLeavesInOrder);
        TEST_METHOD(Leaves_UnknownWorkspaceReturnsEmpty);
        TEST_METHOD(Tab_FindsByIdAcrossWorkspaces);
        TEST_METHOD(Tab_ReturnsNullForUnknown);
        TEST_METHOD(FindFirstTabOfKind_FindsSettings);
        TEST_METHOD(FindFirstTabOfKind_ReturnsNullWhenAbsent);
        TEST_METHOD(SidebarWidth_RoundTrips);
    };

    // -----------------------------------------------------------------
    void QueryTests::Workspaces_ReturnsDisplayOrderVector()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto& vec = b.state->workspaces_view();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), vec.size());
        VERIFY_IS_TRUE(vec[0].id == a.id);
        VERIFY_IS_TRUE(vec[1].id == b.id);
    }

    void QueryTests::Workspace_FindsById()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto* found = b.state->workspace(a.id);
        VERIFY_IS_TRUE(found != nullptr);
        VERIFY_IS_TRUE(found->id == a.id);
    }

    void QueryTests::Workspace_ReturnsNullForUnknown()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        VERIFY_IS_TRUE(a.state->workspace(WorkspaceId{ 9999 }) == nullptr);
    }

    void QueryTests::ActiveWorkspaceId_FollowsState()
    {
        auto empty = emptyModel();
        VERIFY_IS_FALSE(empty->activeWorkspaceId_view().has_value());
        auto a = newWorkspace(empty, "a", termSpec(1));
        VERIFY_IS_TRUE(a.state->activeWorkspaceId_view().has_value());
        VERIFY_IS_TRUE(*a.state->activeWorkspaceId_view() == a.id);
    }

    void QueryTests::Mru_FrontIsMostRecent()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto& mru = b.state->mru_view();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), mru.size());
        VERIFY_IS_TRUE(mru.front() == b.id);
        VERIFY_IS_TRUE(mru.back() == a.id);
    }

    void QueryTests::Pane_FindsLeafById()
    {
        auto f = makeSingleWorkspace();
        const auto* p = f.state->pane(f.leafId);
        VERIFY_IS_TRUE(p != nullptr);
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(*p));
        VERIFY_IS_TRUE(std::get<LeafPane>(*p).id == f.leafId);
    }

    void QueryTests::Pane_FindsSplitById()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        const auto splitId = std::get<SplitPane>(sp.state->workspaces[0].root).id;
        const auto* p = sp.state->pane(splitId);
        VERIFY_IS_TRUE(p != nullptr);
        VERIFY_IS_TRUE(std::holds_alternative<SplitPane>(*p));
    }

    void QueryTests::Pane_ReturnsNullForUnknown()
    {
        auto f = makeSingleWorkspace();
        VERIFY_IS_TRUE(f.state->pane(PaneId{ 9999 }) == nullptr);
    }

    void QueryTests::ParentOf_LeafChildOfSplit()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        const auto* parent = sp.state->parentOf(sp.newPaneId);
        VERIFY_IS_TRUE(parent != nullptr);
        // The parent's right child is the new leaf.
        VERIFY_IS_TRUE(parent->right != nullptr);
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(*parent->right));
        VERIFY_IS_TRUE(std::get<LeafPane>(*parent->right).id == sp.newPaneId);
    }

    void QueryTests::ParentOf_RootReturnsNull()
    {
        auto f = makeSingleWorkspace();
        // f.leafId is the workspace's root pane; it has no parent split.
        VERIFY_IS_TRUE(f.state->parentOf(f.leafId) == nullptr);
    }

    void QueryTests::Leaves_ReturnsAllLeavesInOrder()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        const auto leaves = sp.state->leaves(f.wsId);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leaves.size());
        // Left-to-right tree order: original leaf first, then new sibling.
        VERIFY_IS_TRUE(leaves[0]->id == f.leafId);
        VERIFY_IS_TRUE(leaves[1]->id == sp.newPaneId);
    }

    void QueryTests::Leaves_UnknownWorkspaceReturnsEmpty()
    {
        auto f = makeSingleWorkspace();
        const auto leaves = f.state->leaves(WorkspaceId{ 9999 });
        VERIFY_IS_TRUE(leaves.empty());
    }

    void QueryTests::Tab_FindsByIdAcrossWorkspaces()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto aTabId = std::get<LeafPane>(b.state->workspaces[0].root).tabs[0].id;
        const auto bTabId = std::get<LeafPane>(b.state->workspaces[1].root).tabs[0].id;
        const auto* ta = b.state->tab(aTabId);
        const auto* tb = b.state->tab(bTabId);
        VERIFY_IS_TRUE(ta != nullptr);
        VERIFY_IS_TRUE(tb != nullptr);
        VERIFY_IS_TRUE(ta->id == aTabId);
        VERIFY_IS_TRUE(tb->id == bTabId);
    }

    void QueryTests::Tab_ReturnsNullForUnknown()
    {
        auto f = makeSingleWorkspace();
        VERIFY_IS_TRUE(f.state->tab(TabId{ 9999 }) == nullptr);
    }

    void QueryTests::FindFirstTabOfKind_FindsSettings()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        // Add a Settings tab in workspace a.
        const auto aLeafId = std::get<LeafPane>(a.state->workspaces[0].root).id;
        auto withSettings = newTab(a.state, a.id, aLeafId, SettingsSpec{});
        const auto* hit = withSettings.state->findFirstTabOfKind<SettingsSpec>();
        VERIFY_IS_TRUE(hit != nullptr);
        VERIFY_IS_TRUE(hit->id == withSettings.id);
    }

    void QueryTests::FindFirstTabOfKind_ReturnsNullWhenAbsent()
    {
        auto f = makeSingleWorkspace();
        // No Settings tab yet — should return nullptr.
        VERIFY_IS_TRUE(f.state->findFirstTabOfKind<SettingsSpec>() == nullptr);
    }

    void QueryTests::SidebarWidth_RoundTrips()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto next = setSidebarWidth(a.state, 420.5);
        VERIFY_ARE_EQUAL(420.5, next->sidebarWidth_view());
        auto neg = setSidebarWidth(next, -10.0);
        VERIFY_ARE_EQUAL(0.0, neg->sidebarWidth_view());
    }
}
