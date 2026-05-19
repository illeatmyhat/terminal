// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for moveTab and moveTabAsSplit, including the identity-preservation
// and cross-workspace cases explicitly called out in issue #11.

#include "pch.h"

#include "TestHelpers.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    class MoveMutatorTests
    {
        TEST_CLASS(MoveMutatorTests);

        TEST_METHOD(MoveTab_WithinSameLeaf_Reorders);
        TEST_METHOD(MoveTab_AcrossLeafs_PreservesTabId);
        TEST_METHOD(MoveTab_CrossWorkspace_PreservesTabIdAndMoves);
        TEST_METHOD(MoveTab_CascadesEmptySource);
        TEST_METHOD(MoveTab_ClampsDstIdx);
        TEST_METHOD(MoveTab_UnknownTab_NoChange);
        TEST_METHOD(MoveTab_UnknownDst_NoChange);

        TEST_METHOD(MoveTabAsSplit_WrapsDestinationLeaf);
        TEST_METHOD(MoveTabAsSplit_PreservesDstPaneIdAndTabId);
        TEST_METHOD(MoveTabAsSplit_CascadesEmptySource);
        TEST_METHOD(MoveTabAsSplit_PlacesNewLeafByEdge);
        TEST_METHOD(MoveTabAsSplit_SameLeaf_NoOp);
    };

    // -----------------------------------------------------------------
    void MoveMutatorTests::MoveTab_WithinSameLeaf_Reorders()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto b = newTab(a.state, f.wsId, f.leafId, termSpec(3));
        // Leaf has [f.tabId, a.id, b.id]. Move b.id to idx 0.
        auto next = moveTab(b.state, b.id, f.leafId, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(3), leaf.tabs.size());
        VERIFY_IS_TRUE(leaf.tabs[0].id == b.id);
        VERIFY_IS_TRUE(leaf.tabs[1].id == f.tabId);
        VERIFY_IS_TRUE(leaf.tabs[2].id == a.id);
    }

    void MoveMutatorTests::MoveTab_AcrossLeafs_PreservesTabId()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        // sp.newPaneId is the right leaf; sp.newTabId is the tab in it.
        const auto srcLeafId = sp.newPaneId;
        const auto srcTabId = sp.newTabId;
        // Move srcTabId to the left leaf (f.leafId) at index 0.
        auto next = moveTab(sp.state, srcTabId, f.leafId, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());

        // The destination workspace should still exist with both panes
        // gone-to-one: source leaf had only that one tab, so it cascades
        // away, leaving the original left leaf as the new root.
        const auto& ws = next->workspaces[0];
        // After source-leaf cascade, root is the surviving sibling
        // (originally f.leafId).
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(ws.root));
        const auto& leaf = std::get<LeafPane>(ws.root);
        VERIFY_IS_TRUE(leaf.id == f.leafId);
        // The moved tab has been inserted at idx 0 of the surviving leaf.
        // Crucially: the moved tab's TabId is still srcTabId.
        VERIFY_IS_TRUE(leaf.tabs[0].id == srcTabId);
    }

    void MoveMutatorTests::MoveTab_CrossWorkspace_PreservesTabIdAndMoves()
    {
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // Inside workspace a, add an extra tab so closing the source
        // doesn't cascade the workspace away.
        const auto aLeafId = std::get<LeafPane>(b.state->workspaces[0].root).id;
        const auto bLeafId = std::get<LeafPane>(b.state->workspaces[1].root).id;
        auto extra = newTab(b.state, a.id, aLeafId, termSpec(3));
        // Move extra.id from workspace a to workspace b's leaf at idx 0.
        auto next = moveTab(extra.state, extra.id, bLeafId, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());

        // The tab should no longer appear in workspace a.
        const auto& wsA = next->workspaces[0];
        const auto& wsB = next->workspaces[1];
        const auto& leafA = std::get<LeafPane>(wsA.root);
        bool foundInA = false;
        for (const auto& t : leafA.tabs)
        {
            if (t.id == extra.id)
            {
                foundInA = true;
            }
        }
        VERIFY_IS_FALSE(foundInA);

        const auto& leafB = std::get<LeafPane>(wsB.root);
        bool foundInB = false;
        for (const auto& t : leafB.tabs)
        {
            if (t.id == extra.id)
            {
                foundInB = true;
            }
        }
        VERIFY_IS_TRUE(foundInB);
    }

    void MoveMutatorTests::MoveTab_CascadesEmptySource()
    {
        // Two workspaces, each with a single-tab leaf. Move workspace a's
        // tab into workspace b — workspace a should disappear entirely
        // because its sole leaf empties.
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto aTabId = std::get<LeafPane>(b.state->workspaces[0].root).tabs[0].id;
        const auto bLeafId = std::get<LeafPane>(b.state->workspaces[1].root).id;
        auto next = moveTab(b.state, aTabId, bLeafId, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
        VERIFY_IS_TRUE(next->workspaces[0].id == b.id);
    }

    void MoveMutatorTests::MoveTab_ClampsDstIdx()
    {
        auto f = makeSingleWorkspace();
        auto a = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto b = newTab(a.state, f.wsId, f.leafId, termSpec(3));
        // Pass an absurdly large dstIdx; should clamp to end-of-vector.
        auto next = moveTab(b.state, f.tabId, f.leafId, 9999);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& leaf = std::get<LeafPane>(next->workspaces[0].root);
        VERIFY_IS_TRUE(leaf.tabs.back().id == f.tabId);
    }

    void MoveMutatorTests::MoveTab_UnknownTab_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto next = moveTab(f.state, TabId{ 9999 }, f.leafId, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());
    }

    void MoveMutatorTests::MoveTab_UnknownDst_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto next = moveTab(f.state, f.tabId, PaneId{ 9999 }, 0);
        VERIFY_IS_FALSE(validate(*next).has_value());
    }

    // -----------------------------------------------------------------
    void MoveMutatorTests::MoveTabAsSplit_WrapsDestinationLeaf()
    {
        auto f = makeSingleWorkspace();
        // Add a tab so the source leaf can give one up without cascading
        // its workspace away. Then split so we have two leaves.
        auto extra = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto sp = splitPane(extra.state, f.leafId, Axis::Vertical, 0.5, termSpec(3));
        // sp.newPaneId is right leaf (one tab). f.leafId has 2 tabs.
        const auto srcLeafId = f.leafId; // 2 tabs
        const auto dstLeafId = sp.newPaneId;
        const auto srcTabId = extra.id;

        auto next = moveTabAsSplit(sp.state, srcTabId, dstLeafId, Edge::Right);
        VERIFY_IS_FALSE(validate(*next).has_value());

        // The dst leaf should be wrapped in a new split. Walk the tree
        // and find dstLeafId — its parent should be a SplitPane.
        VERIFY_IS_TRUE(next->parentOf(dstLeafId) != nullptr);
    }

    void MoveMutatorTests::MoveTabAsSplit_PreservesDstPaneIdAndTabId()
    {
        auto f = makeSingleWorkspace();
        auto extra = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto sp = splitPane(extra.state, f.leafId, Axis::Vertical, 0.5, termSpec(3));
        const auto dstLeafId = sp.newPaneId;
        const auto srcTabId = extra.id;

        auto next = moveTabAsSplit(sp.state, srcTabId, dstLeafId, Edge::Bottom);
        VERIFY_IS_FALSE(validate(*next).has_value());

        // dstLeafId must still exist (PaneId preserved).
        VERIFY_IS_TRUE(next->pane(dstLeafId) != nullptr);
        // srcTabId must still exist somewhere.
        VERIFY_IS_TRUE(next->tab(srcTabId) != nullptr);
    }

    void MoveMutatorTests::MoveTabAsSplit_CascadesEmptySource()
    {
        // Workspace a has one tab. Workspace b has one tab. Move a's
        // single tab into b as a split. Source workspace a disappears.
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        const auto aTabId = std::get<LeafPane>(b.state->workspaces[0].root).tabs[0].id;
        const auto bLeafId = std::get<LeafPane>(b.state->workspaces[1].root).id;

        auto next = moveTabAsSplit(b.state, aTabId, bLeafId, Edge::Right);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
        VERIFY_IS_TRUE(next->workspaces[0].id == b.id);
        // bLeafId still findable + tab present.
        VERIFY_IS_TRUE(next->pane(bLeafId) != nullptr);
        VERIFY_IS_TRUE(next->tab(aTabId) != nullptr);
    }

    void MoveMutatorTests::MoveTabAsSplit_PlacesNewLeafByEdge()
    {
        auto f = makeSingleWorkspace();
        auto extra = newTab(f.state, f.wsId, f.leafId, termSpec(2));
        auto sp = splitPane(extra.state, f.leafId, Axis::Vertical, 0.5, termSpec(3));
        const auto dstLeafId = sp.newPaneId;
        const auto srcTabId = extra.id;

        // Edge::Left → new leaf on the LEFT of dst (= split.left). Axis vertical.
        auto next = moveTabAsSplit(sp.state, srcTabId, dstLeafId, Edge::Left);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto* parent = next->parentOf(dstLeafId);
        VERIFY_IS_TRUE(parent != nullptr);
        VERIFY_IS_TRUE(parent->axis == Axis::Vertical);
        // dst leaf should be split.right; the new leaf with srcTabId
        // should be split.left.
        VERIFY_IS_TRUE(parent->right != nullptr);
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(*parent->right));
        VERIFY_IS_TRUE(std::get<LeafPane>(*parent->right).id == dstLeafId);
    }

    void MoveMutatorTests::MoveTabAsSplit_SameLeaf_NoOp()
    {
        auto f = makeSingleWorkspace();
        auto next = moveTabAsSplit(f.state, f.tabId, f.leafId, Edge::Right);
        VERIFY_IS_FALSE(validate(*next).has_value());
        // Same-leaf target should be treated as a degenerate input —
        // structure unchanged.
        const auto& ws = next->workspaces[0];
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(ws.root));
    }
}
