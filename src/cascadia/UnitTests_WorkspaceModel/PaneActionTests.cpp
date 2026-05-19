// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for the 4 pane actions in WorkspaceActions.h.
//
// Includes the identity-preservation test for splitPane: the original
// leaf's PaneId must be unchanged across the action, because the new
// SplitPane wraps the original leaf rather than replacing it.

#include "pch.h"

#include "TestHelpers.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    class PaneActionTests
    {
        TEST_CLASS(PaneActionTests);

        TEST_METHOD(SplitPane_PreservesOriginalLeafPaneId);
        TEST_METHOD(SplitPane_NewLeafContainsOneTab);
        TEST_METHOD(SplitPane_UnknownLeaf_NoChange);
        TEST_METHOD(SplitPane_AssignsRequestedAxisAndRatio);
        TEST_METHOD(SplitPane_AllocatesMonotonicIds);
        TEST_METHOD(SplitPane_FocusesNewPane);

        TEST_METHOD(ClosePane_CollapsesSplitToSibling);
        TEST_METHOD(ClosePane_LastLeafRemovesWorkspace);
        TEST_METHOD(ClosePane_UnknownLeaf_NoChange);

        TEST_METHOD(ResizePane_UpdatesSplitRatio);
        TEST_METHOD(ResizePane_ClampsToBounds);
        TEST_METHOD(ResizePane_OnLeafId_NoChange);

        TEST_METHOD(FocusPane_SwitchesActivePaneAndWorkspace);
        TEST_METHOD(FocusPane_UnknownLeaf_NoChange);
    };

    // -----------------------------------------------------------------
    void PaneActionTests::SplitPane_PreservesOriginalLeafPaneId()
    {
        auto f = makeSingleWorkspace();
        const auto originalLeafId = f.leafId;

        auto sp = splitPane(f.state, originalLeafId, Axis::Vertical, 0.5, termSpec(2));
        VERIFY_IS_FALSE(validate(*sp.state).has_value());

        // The original leaf must still be findable by its original id.
        const auto& ws = sp.state->workspaces[0];
        VERIFY_IS_TRUE(std::holds_alternative<SplitPane>(ws.root));
        const auto& split = std::get<SplitPane>(ws.root);

        // The original leaf should be the LEFT child (per the action's
        // documented placement). Its PaneId must equal originalLeafId.
        VERIFY_IS_TRUE(split.left != nullptr);
        const auto& leftNode = *split.left;
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(leftNode));
        const auto& leftLeaf = std::get<LeafPane>(leftNode);
        VERIFY_IS_TRUE(leftLeaf.id == originalLeafId);
    }

    void PaneActionTests::SplitPane_NewLeafContainsOneTab()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Horizontal, 0.5, termSpec(2),
                            "newtab", std::nullopt, /*pinned=*/false);
        VERIFY_IS_FALSE(validate(*sp.state).has_value());
        const auto& split = std::get<SplitPane>(sp.state->workspaces[0].root);
        VERIFY_IS_TRUE(split.right != nullptr);
        const auto& rightNode = *split.right;
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(rightNode));
        const auto& rightLeaf = std::get<LeafPane>(rightNode);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), rightLeaf.tabs.size());
        VERIFY_IS_TRUE(rightLeaf.tabs[0].id == sp.newTabId);
        VERIFY_ARE_EQUAL(std::string{ "newtab" }, rightLeaf.tabs[0].customTitle);
    }

    void PaneActionTests::SplitPane_UnknownLeaf_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, PaneId{ 9999 }, Axis::Vertical, 0.5, termSpec(2));
        VERIFY_IS_TRUE(sp.newPaneId == PaneId{ 0 });
        VERIFY_IS_TRUE(sp.newTabId == TabId{ 0 });
        VERIFY_IS_FALSE(validate(*sp.state).has_value());
    }

    void PaneActionTests::SplitPane_AssignsRequestedAxisAndRatio()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Horizontal, 0.3, termSpec(2));
        const auto& split = std::get<SplitPane>(sp.state->workspaces[0].root);
        VERIFY_IS_TRUE(split.axis == Axis::Horizontal);
        VERIFY_ARE_EQUAL(0.3, split.ratio);
    }

    void PaneActionTests::SplitPane_AllocatesMonotonicIds()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        // The new split pane id, new sibling leaf id, and new tab id are
        // all distinct and greater than the pre-split counter.
        VERIFY_IS_TRUE(sp.newPaneId.v > f.leafId.v);
        VERIFY_IS_TRUE(sp.newTabId.v > sp.newPaneId.v);
    }

    void PaneActionTests::SplitPane_FocusesNewPane()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        VERIFY_IS_TRUE(sp.state->workspaces[0].activePaneId == sp.newPaneId);
    }

    // -----------------------------------------------------------------
    void PaneActionTests::ClosePane_CollapsesSplitToSibling()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        // sp.newPaneId is the new sibling leaf. Close it.
        auto next = closePane(sp.state, sp.newPaneId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& ws = next->workspaces[0];
        // Root should be a leaf again (the survivor).
        VERIFY_IS_TRUE(std::holds_alternative<LeafPane>(ws.root));
        VERIFY_IS_TRUE(std::get<LeafPane>(ws.root).id == f.leafId);
    }

    void PaneActionTests::ClosePane_LastLeafRemovesWorkspace()
    {
        auto f = makeSingleWorkspace();
        auto next = closePane(f.state, f.leafId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(next->workspaces.empty());
        VERIFY_IS_FALSE(next->activeWorkspaceId.has_value());
    }

    void PaneActionTests::ClosePane_UnknownLeaf_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto next = closePane(f.state, PaneId{ 9999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), next->workspaces.size());
    }

    // -----------------------------------------------------------------
    void PaneActionTests::ResizePane_UpdatesSplitRatio()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        // Find the split's id (it's the root after the split).
        const auto& split = std::get<SplitPane>(sp.state->workspaces[0].root);
        const auto splitId = split.id;
        auto next = resizePane(sp.state, splitId, 0.75);
        VERIFY_IS_FALSE(validate(*next).has_value());
        const auto& split2 = std::get<SplitPane>(next->workspaces[0].root);
        VERIFY_ARE_EQUAL(0.75, split2.ratio);
    }

    void PaneActionTests::ResizePane_ClampsToBounds()
    {
        auto f = makeSingleWorkspace();
        auto sp = splitPane(f.state, f.leafId, Axis::Vertical, 0.5, termSpec(2));
        const auto splitId = std::get<SplitPane>(sp.state->workspaces[0].root).id;
        auto negative = resizePane(sp.state, splitId, -0.5);
        VERIFY_ARE_EQUAL(0.0, std::get<SplitPane>(negative->workspaces[0].root).ratio);
        auto huge = resizePane(sp.state, splitId, 5.0);
        VERIFY_ARE_EQUAL(1.0, std::get<SplitPane>(huge->workspaces[0].root).ratio);
    }

    void PaneActionTests::ResizePane_OnLeafId_NoChange()
    {
        auto f = makeSingleWorkspace();
        // Try to resize a leaf id; should be a no-op since leaves aren't
        // splits.
        auto next = resizePane(f.state, f.leafId, 0.9);
        VERIFY_IS_FALSE(validate(*next).has_value());
    }

    // -----------------------------------------------------------------
    void PaneActionTests::FocusPane_SwitchesActivePaneAndWorkspace()
    {
        // Make two workspaces; focus the leaf in the older workspace.
        auto a = newWorkspace(emptyModel(), "a", termSpec(1));
        auto b = newWorkspace(a.state, "b", termSpec(2));
        // active is b. Pull the leaf id from a.
        const auto aLeafId = std::get<LeafPane>(b.state->workspaces[0].root).id;
        auto next = focusPane(b.state, aLeafId);
        VERIFY_IS_FALSE(validate(*next).has_value());
        VERIFY_IS_TRUE(*next->activeWorkspaceId == a.id);
        VERIFY_IS_TRUE(next->mru.front() == a.id);
    }

    void PaneActionTests::FocusPane_UnknownLeaf_NoChange()
    {
        auto f = makeSingleWorkspace();
        auto next = focusPane(f.state, PaneId{ 9999 });
        VERIFY_IS_FALSE(validate(*next).has_value());
    }
}
