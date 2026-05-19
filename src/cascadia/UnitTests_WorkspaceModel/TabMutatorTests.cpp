// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for the 8 tab mutators in Mutators.h, including the cascade-edge
// scenarios called out specifically in issue #11 (last-tab-in-leaf
// removes leaf and collapses parent split).

#include "pch.h"

#include "TestHelpers.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    class TabMutatorTests
    {
        TEST_CLASS(TabMutatorTests);

        TEST_METHOD(NewTab_AddsToLeafAndBecomesActive);
        TEST_METHOD(NewTab_UnknownLeaf_NoChange);
        TEST_METHOD(NewTab_AllocatesMonotonicTabId);

        TEST_METHOD(CloseTab_RemovesFromLeaf);
        TEST_METHOD(CloseTab_LastTabInLeaf_RemovesWorkspace);
        TEST_METHOD(CloseTab_CascadeCollapsesSplit);
        TEST_METHOD(CloseTab_ShiftsActiveIdxLeft);
        TEST_METHOD(CloseTab_UnknownTabId_NoChange);

        TEST_METHOD(CloseTabsRight_TrimsTrailing);
        TEST_METHOD(CloseTabsRight_OnLast_NoOp);

        TEST_METHOD(CloseOtherTabs_KeepsOnlyNamed);
        TEST_METHOD(CloseOtherTabs_AdjustsActiveIdx);

        TEST_METHOD(SelectTab_UpdatesActiveTabAndWorkspace);
        TEST_METHOD(SelectTab_UnknownTabId_NoChange);

        TEST_METHOD(SetTabTitle_RoundTrips);
        TEST_METHOD(SetTabColor_RoundTrips);
        TEST_METHOD(SetTabPinned_RoundTrips);
    };

    // -----------------------------------------------------------------
    void TabMutatorTests::NewTab_AddsToLeafAndBecomesActive()
    {
        auto f = makeSingleWorkspace();
        auto r = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        VERIFY_IS_FALSE(validate(*r.state).has_value());
        const auto& leaf = std::get<LeafPane>(r.state->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leaf.tabs.size());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leaf.activeTabIdx);
        VERIFY_IS_TRUE(leaf.tabs[1].id == r.id);
    }

    void TabMutatorTests::NewTab_UnknownLeaf_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto r = newTab(f.state, f.wsId, PaneId{ 999 }, termSpec(2));
        VERIFY_IS_TRUE(r.id == TabId{ 0 });
        // The state should be unchanged (same shared_ptr or, at worst, a
        // semantically equal copy).
        VERIFY_IS_FALSE(validate(*r.state).has_value());
        const auto& leaf = std::get<LeafPane>(r.state->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leaf.tabs.size());
    }

    void TabMutatorTests::NewTab_AllocatesMonotonicTabId()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto b = newTab(a.state, f.wsId, f.leafId, termSpec(3));
        VERIFY_IS_TRUE(a.id.v < b.id.v);
    }

    // -----------------------------------------------------------------
    void TabMutatorTests::CloseTab_RemovesFromLeaf()
    {
        auto f = makeSingleWorkspace();
        auto extra = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        // Now leaf has 2 tabs (f.tabId, extra.id).
        auto next = closeTab(extra.state, extra.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leaf.tabs.size());
        VERIFY_IS_TRUE(leaf.tabs[0].id == f.tabId);
    }

    void TabMutatorTests::CloseTab_LastTabInLeaf_RemovesWorkspace()
    {
        // A workspace with one leaf and one tab. closeTab on it should
        // cascade all the way: leaf gone → root gone → workspace gone
        // → activeWorkspaceId falls back to nullopt.
        auto f = makeSingleWorkspace();
        auto next = closeTab(f.state, f.tabId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces.empty());
        VERIFY_IS_FALSE(next->activeWorkspaceId.has_value());
    }

    void TabMutatorTests::CloseTab_CascadeCollapsesSplit()
    {
        // Build a workspace with a 2-pane split. Close the only tab in
        // the right pane and verify:
        //   - the right leaf disappears
        //   - the parent split disappears (collapsed to the surviving sibling)
        //   - the workspace's root pane is now the surviving leaf
        //   - activePaneId points at the surviving leaf
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        VERIFY_IS_FALSE(validate(*sp.state).has_value());
        // After splitPane, root is a SplitPane. Original f.leafId is on
        // the left; new pane (sp.newPaneId) on the right with sp.newTabId.
        {
            const auto& ws = sp.state->workspaces[0];
            VERIFY_IS_TRUE(std::holds_alternative<SplitPane>(ws.root));
        }

        // Close the tab in the right (new) pane.
        auto next = closeTab(sp.state, sp.newTabId);
        VERIFY_IS_FALSE(validate(*next).has_value());

        const auto& ws = next->workspaces[0];
        // Root should now be a LeafPane (the surviving sibling).
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(ws.root));
        const auto& survivingLeaf = std::get<LeafPane>(ws.root);
        // Surviving sibling's PaneId is the original f.leafId.
        VERIFY_IS_TRUE(survivingLeaf.id == f.leafId);
        VERIFY_IS_TRUE(ws.activePaneId == f.leafId);
    }

    void TabMutatorTests::CloseTab_ShiftsActiveIdxLeft()
    {
        // Three tabs; active = tab 2 (idx 2). Close tab 0; active should
        // shift from 2 to 1.
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto b = newTab(a.state, f.wsId, f.leafId, termSpec(3));
        // newTab makes the new tab active, so active is now idx 2.
        const auto& leafBefore = std::get<LeafPane>(b.state->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leafBefore.activeTabIdx);

        auto next = closeTab(b.state, f.tabId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leafAfter = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leafAfter.tabs.size());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leafAfter.activeTabIdx);
    }

    void TabMutatorTests::CloseTab_UnknownTabId_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto next = closeTab(f.state, TabId{ 9999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
    }

    // -----------------------------------------------------------------
    void TabMutatorTests::CloseTabsRight_TrimsTrailing()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto b = newTab(a.state, f.wsId, f.leafId, termSpec(3));
        // Leaf has [f.tabId, a.id, b.id]. Close right of a.id.
        auto next = closeTabsRight(b.state, a.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leaf.tabs.size());
        VERIFY_IS_TRUE(leaf.tabs[0].id == f.tabId);
        VERIFY_IS_TRUE(leaf.tabs[1].id == a.id);
    }

    void TabMutatorTests::CloseTabsRight_OnLast_NoOp()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto next = closeTabsRight(a.state, a.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leaf.tabs.size());
    }

    // -----------------------------------------------------------------
    void TabMutatorTests::CloseOtherTabs_KeepsOnlyNamed()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto b = newTab(a.state, f.wsId, f.leafId, termSpec(3));
        auto next = closeOtherTabs(b.state, a.id);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leaf.tabs.size());
        VERIFY_IS_TRUE(leaf.tabs[0].id == a.id);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), leaf.activeTabIdx);
    }

    void TabMutatorTests::CloseOtherTabs_AdjustsActiveIdx()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto next = closeOtherTabs(a.state, f.tabId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leaf.tabs.size());
        VERIFY_IS_TRUE(leaf.tabs[0].id == f.tabId);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), leaf.activeTabIdx);
    }

    // -----------------------------------------------------------------
    void TabMutatorTests::SelectTab_UpdatesActiveTabAndWorkspace()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // active is currently b; pull the tab id from workspace a's leaf.
        const auto& aLeaf = std::get<LeafPane>(b.state->workspaces[0].root);
        const auto aTabId = aLeaf.tabs[0].id;
        auto next = selectTab(b.state, aTabId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        // Active workspace should have flipped to a.
        VERIFY_IS_TRUE(*next->activeWorkspaceId == a.id);
        VERIFY_IS_TRUE(next->mru.front() == a.id);
    }

    void TabMutatorTests::SelectTab_UnknownTabId_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto next = selectTab(f.state, TabId{ 9999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
    }

    // -----------------------------------------------------------------
    void TabMutatorTests::SetTabTitle_RoundTrips()
    {
        auto f = makeSingleWorkspace();
        auto next = setTabTitle(f.state, f.tabId, "Hello");
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(std::string{ "Hello" }, leaf.tabs[0].customTitle);
    }

    void TabMutatorTests::SetTabColor_RoundTrips()
    {
        auto f = makeSingleWorkspace();
        Color green{ 0, 255, 0, 255 };
        auto next = setTabColor(f.state, f.tabId, green);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_IS_TRUE(leaf.tabs[0].runtimeColor.has_value());
        VERIFY_IS_TRUE(*leaf.tabs[0].runtimeColor == green);

        auto cleared = setTabColor(next, f.tabId, std::nullopt);
        const auto& leaf2 = std::get<LeafPane>(cleared->workspaces[0].root);
        VERIFY_IS_FALSE(leaf2.tabs[0].runtimeColor.has_value());
    }

    void TabMutatorTests::SetTabPinned_RoundTrips()
    {
        auto f = makeSingleWorkspace();
        auto next = setTabPinned(f.state, f.tabId, true);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_IS_TRUE(leaf.tabs[0].pinned);
    }
}
