// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Behavioral simulator scripts. Each TEST_METHOD here is one scripted
// user-flow sequence written against the Script DSL in
// BehavioralSimulator.h.
//
// Phase 0 stop condition: these scripts collectively cover every flow
// exercised in the running app today. See issue #14 for the flow list.

#include "pch.h"

#include "BehavioralSimulator.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Helper: verify the script completed without violation OR failure.
        void verifyClean(Script& s)
        {
            VERIFY_IS_FALSE(s.firstViolation().has_value());
            if (!s.failures().empty())
            {
                for (const auto& f : s.failures())
                {
                    Log::Error(String(f.c_str()));
                }
            }
            VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), s.failures().size());
        }
    }

    class BehavioralTests
    {
        TEST_CLASS(BehavioralTests);

        // Workspace lifecycle
        TEST_METHOD(Workspace_Create_Switch_MruAdvances);
        TEST_METHOD(Workspace_Rename_Recolor_Description_Pinned);
        TEST_METHOD(Workspace_Reorder);
        TEST_METHOD(Workspace_CloseOther_CloseAll_CloseLast_EmptyState);

        // Single-leaf single-tab
        TEST_METHOD(SingleTab_SelectIsNoOp_CloseCascadesWorkspace);

        // Multi-tab single-leaf
        TEST_METHOD(MultiTab_Open3_SelectThrough_CloseMiddle);
        TEST_METHOD(MultiTab_CloseTabsRight_CloseOtherTabs_Decoration);

        // Splits
        TEST_METHOD(Split_VerticalAndHorizontal_Resize_Close);
        TEST_METHOD(Split_CascadeCollapseWhenOneSideEmpties_FocusPane);

        // Moves
        TEST_METHOD(MoveTab_SameLeaf_Reorder);
        TEST_METHOD(MoveTab_CrossLeafSameWorkspace_PreservesTabId);
        TEST_METHOD(MoveTab_CrossWorkspace_AtomicWithSourceCascade);
        TEST_METHOD(MoveTabAsSplit_EachEdge_TopBottomLeftRight);

        // Settings as content
        TEST_METHOD(Settings_FindFirstAndSelect_NoDuplicate);

        // All five content variants
        TEST_METHOD(AllFiveContentVariants_MountContentOpEachKind);

        // Render-op assertions
        TEST_METHOD(NewWorkspace_EmitsAddAndCreateAndMount);
        TEST_METHOD(SplitPane_EmitsCreateSplitAndCreateLeafAndAddTab);

        // Cascade scenarios for the four cascade rules
        TEST_METHOD(Cascade_CloseTab_LastTab_RemovesLeafAndCollapsesSplit);
        TEST_METHOD(Cascade_CloseWorkspace_LastOne_EmptyState);
    };

    // ==================================================================
    // Workspace lifecycle
    // ==================================================================

    void BehavioralTests::Workspace_Create_Switch_MruAdvances()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "tA")
            .newWorkspace("b", "beta", termSpecSim(2), "tB")
            .newWorkspace("c", "gamma", termSpecSim(3), "tC")
            // After three creates the MRU is [c, b, a] (front = most recent).
            .switchToWorkspace("a")
            .switchToWorkspace("b")
            .switchToWorkspace("c")
            .expect([](const WorkspaceModelData& m) {
                if (m.mru.size() != 3) return false;
                if (!m.activeWorkspaceId.has_value()) return false;
                return m.mru.front() == *m.activeWorkspaceId;
            }, "MRU front = active");

        s.run();
        verifyClean(s);
        const auto& m = *s.state();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(3), m.workspaces.size());
        VERIFY_IS_TRUE(m.activeWorkspaceId.has_value());
        VERIFY_IS_TRUE(*m.activeWorkspaceId == s.workspaceFor("c"));
        VERIFY_IS_TRUE(m.mru.front() == s.workspaceFor("c"));
    }

    void BehavioralTests::Workspace_Rename_Recolor_Description_Pinned()
    {
        Script s;
        Color red{ 200, 30, 30, 0xFF };
        s.newWorkspace("a", "alpha", termSpecSim(1), "tA")
            .renameWorkspace("a", "Renamed")
            .setWorkspaceColor("a", red)
            .setWorkspaceDescription("a", "an alpha workspace")
            .setWorkspacePinned("a", true);
        s.run();
        verifyClean(s);
        const auto& ws = s.state()->workspaces[0];
        VERIFY_ARE_EQUAL(std::string{ "Renamed" }, ws.name);
        VERIFY_IS_TRUE(ws.color.has_value() && *ws.color == red);
        VERIFY_ARE_EQUAL(std::string{ "an alpha workspace" }, ws.customDescription);
        VERIFY_IS_TRUE(ws.pinned);
    }

    void BehavioralTests::Workspace_Reorder()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "tA")
            .newWorkspace("b", "beta", termSpecSim(2), "tB")
            .newWorkspace("c", "gamma", termSpecSim(3), "tC")
            .reorderWorkspace("c", 0); // move c to front
        s.run();
        verifyClean(s);
        const auto& m = *s.state();
        VERIFY_IS_TRUE(m.workspaces[0].id == s.workspaceFor("c"));
        VERIFY_IS_TRUE(m.workspaces[1].id == s.workspaceFor("a"));
        VERIFY_IS_TRUE(m.workspaces[2].id == s.workspaceFor("b"));
    }

    void BehavioralTests::Workspace_CloseOther_CloseAll_CloseLast_EmptyState()
    {
        // First: closeOtherWorkspaces keeps just one. Then closeAll. Then
        // verify empty-state invariant.
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "tA")
            .newWorkspace("b", "beta", termSpecSim(2), "tB")
            .newWorkspace("c", "gamma", termSpecSim(3), "tC")
            .closeOtherWorkspaces("b")
            .expect([&s](const WorkspaceModelData& m) {
                return m.workspaces.size() == 1 &&
                       m.workspaces[0].id == s.workspaceFor("b");
            }, "only b remains")
            .closeAllWorkspaces()
            .expect([](const WorkspaceModelData& m) {
                return m.workspaces.empty() && !m.activeWorkspaceId.has_value() && m.mru.empty();
            }, "empty state");
        s.run();
        verifyClean(s);
    }

    // ==================================================================
    // Single-leaf single-tab
    // ==================================================================

    void BehavioralTests::SingleTab_SelectIsNoOp_CloseCascadesWorkspace()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0")
            .selectTab("t0")
            .expect([&s](const WorkspaceModelData& m) {
                if (m.workspaces.size() != 1) return false;
                const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
                return leaf.tabs.size() == 1 && leaf.activeTabIdx == 0;
            }, "single tab still selected")
            .closeTab("t0")
            // Closing the last tab in the only workspace cascades the
            // workspace away → empty state.
            .expect([](const WorkspaceModelData& m) {
                return m.workspaces.empty() && !m.activeWorkspaceId.has_value();
            }, "workspace cascaded to empty state");
        s.run();
        verifyClean(s);
    }

    // ==================================================================
    // Multi-tab single-leaf
    // ==================================================================

    void BehavioralTests::MultiTab_Open3_SelectThrough_CloseMiddle()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .newTab("t1", "a", "leafA", termSpecSim(2))
            .newTab("t2", "a", "leafA", termSpecSim(3))
            .selectTab("t0")
            .selectTab("t1")
            .selectTab("t2")
            .closeTab("t1");
        s.run();
        verifyClean(s);
        const auto& leaf = std::get<LeafPane>(s.state()->workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leaf.tabs.size());
        // t0 and t2 remain.
        VERIFY_IS_TRUE(leaf.tabs[0].id == s.tabFor("t0"));
        VERIFY_IS_TRUE(leaf.tabs[1].id == s.tabFor("t2"));
    }

    void BehavioralTests::MultiTab_CloseTabsRight_CloseOtherTabs_Decoration()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .newTab("t1", "a", "leafA", termSpecSim(2))
            .newTab("t2", "a", "leafA", termSpecSim(3))
            .newTab("t3", "a", "leafA", termSpecSim(4))
            .closeTabsRight("t1")
            // Now [t0, t1] remain.
            .expect([&s](const WorkspaceModelData& m) {
                const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
                return leaf.tabs.size() == 2 &&
                       leaf.tabs[0].id == s.tabFor("t0") &&
                       leaf.tabs[1].id == s.tabFor("t1");
            }, "closeTabsRight kept first two")
            .closeOtherTabs("t0")
            .expect([&s](const WorkspaceModelData& m) {
                const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
                return leaf.tabs.size() == 1 && leaf.tabs[0].id == s.tabFor("t0");
            }, "closeOtherTabs kept just t0")
            .setTabTitle("t0", "renamed")
            .setTabColor("t0", Color{ 1, 2, 3, 0xFF })
            .setTabPinned("t0", true);
        s.run();
        verifyClean(s);
        const auto& leaf = std::get<LeafPane>(s.state()->workspaces[0].root);
        VERIFY_ARE_EQUAL(std::string{ "renamed" }, leaf.tabs[0].customTitle);
        VERIFY_IS_TRUE(leaf.tabs[0].pinned);
        VERIFY_IS_TRUE(leaf.tabs[0].runtimeColor.has_value());
    }

    // ==================================================================
    // Splits
    // ==================================================================

    void BehavioralTests::Split_VerticalAndHorizontal_Resize_Close()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .splitPane("leafA", Axis::Vertical, 0.4, termSpecSim(2), "leafB", "tB")
            // Root is now a SplitPane.
            .expect([](const WorkspaceModelData& m) {
                return std::holds_alternative<SplitPane>(m.workspaces[0].root);
            }, "root is split after splitPane")
            .splitPane("leafB", Axis::Horizontal, 0.7, termSpecSim(3), "leafC", "tC")
            .closePane("leafC")
            // Closing leafC cascades the inner split away; leafB returns
            // to being the sibling under the outer split.
            .expect([](const WorkspaceModelData& m) {
                return std::holds_alternative<SplitPane>(m.workspaces[0].root);
            }, "outer split survives leafC close");
        s.run();
        verifyClean(s);
    }

    void BehavioralTests::Split_CascadeCollapseWhenOneSideEmpties_FocusPane()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .splitPane("leafA", Axis::Vertical, 0.5, termSpecSim(2), "leafB", "tB")
            .focusPane("leafA")
            .expect([&s](const WorkspaceModelData& m) {
                return m.workspaces[0].activePaneId == s.leafFor("leafA");
            }, "leafA is now active pane")
            // Close leafB. Its only tab goes; leaf disappears; split collapses
            // to leafA, which is now the workspace root.
            .closePane("leafB")
            .expect([&s](const WorkspaceModelData& m) {
                if (!std::holds_alternative<LeafPane>(m.workspaces[0].root)) return false;
                const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
                return leaf.id == s.leafFor("leafA");
            }, "split collapsed; leafA is root");
        s.run();
        verifyClean(s);
    }

    // ==================================================================
    // Moves
    // ==================================================================

    void BehavioralTests::MoveTab_SameLeaf_Reorder()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .newTab("t1", "a", "leafA", termSpecSim(2))
            .newTab("t2", "a", "leafA", termSpecSim(3))
            // Move t2 to index 0.
            .moveTab("t2", "leafA", 0);
        s.run();
        verifyClean(s);
        const auto& leaf = std::get<LeafPane>(s.state()->workspaces[0].root);
        VERIFY_IS_TRUE(leaf.tabs[0].id == s.tabFor("t2"));
        VERIFY_IS_TRUE(leaf.tabs[1].id == s.tabFor("t0"));
        VERIFY_IS_TRUE(leaf.tabs[2].id == s.tabFor("t1"));
    }

    void BehavioralTests::MoveTab_CrossLeafSameWorkspace_PreservesTabId()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .newTab("t1", "a", "leafA", termSpecSim(2))
            .splitPane("leafA", Axis::Vertical, 0.5, termSpecSim(3), "leafB", "tB")
            // Move t1 from leafA to leafB at idx 0. t1's id should survive.
            .moveTab("t1", "leafB", 0);
        s.run();
        verifyClean(s);
        // After the move, leafA has [t0], leafB has [t1, tB].
        const auto& ws = s.state()->workspaces[0];
        // Walk the tree.
        const SplitPane* split = std::get_if<SplitPane>(&ws.root);
        VERIFY_IS_NOT_NULL(split);
        const auto& l = std::get<LeafPane>(*split->left);
        const auto& r = std::get<LeafPane>(*split->right);
        const LeafPane& leftLeaf = (l.id == s.leafFor("leafA")) ? l : r;
        const LeafPane& rightLeaf = (l.id == s.leafFor("leafA")) ? r : l;
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leftLeaf.tabs.size());
        VERIFY_IS_TRUE(leftLeaf.tabs[0].id == s.tabFor("t0"));
        // leafB now has [t1, tB] (insertion at idx 0).
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), rightLeaf.tabs.size());
        VERIFY_IS_TRUE(rightLeaf.tabs[0].id == s.tabFor("t1"));
        VERIFY_IS_TRUE(rightLeaf.tabs[1].id == s.tabFor("tB"));
    }

    void BehavioralTests::MoveTab_CrossWorkspace_AtomicWithSourceCascade()
    {
        Script s;
        // ws "a" has [tA0, tA1]; ws "b" has [tB0]. Move tA0 across.
        s.newWorkspace("a", "alpha", termSpecSim(1), "tA0", "leafA")
            .newTab("tA1", "a", "leafA", termSpecSim(2))
            .newWorkspace("b", "beta", termSpecSim(3), "tB0", "leafB")
            // After this move, ws a keeps [tA1], ws b becomes [tA0, tB0].
            .moveTab("tA0", "leafB", 0);
        s.run();
        verifyClean(s);
        const auto& m = *s.state();
        const auto* wsA = m.workspace(s.workspaceFor("a"));
        const auto* wsB = m.workspace(s.workspaceFor("b"));
        VERIFY_IS_NOT_NULL(wsA);
        VERIFY_IS_NOT_NULL(wsB);
        const auto& leafA = std::get<LeafPane>(wsA->root);
        const auto& leafB = std::get<LeafPane>(wsB->root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leafA.tabs.size());
        VERIFY_IS_TRUE(leafA.tabs[0].id == s.tabFor("tA1"));
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), leafB.tabs.size());
        VERIFY_IS_TRUE(leafB.tabs[0].id == s.tabFor("tA0"));
        VERIFY_IS_TRUE(leafB.tabs[1].id == s.tabFor("tB0"));
    }

    void BehavioralTests::MoveTabAsSplit_EachEdge_TopBottomLeftRight()
    {
        // Build a stable scenario and re-run for each edge.
        for (auto edge : { Edge::Top, Edge::Bottom, Edge::Left, Edge::Right })
        {
            Script s;
            s.newWorkspace("a", "alpha", termSpecSim(1), "tA0", "leafA")
                .newTab("tA1", "a", "leafA", termSpecSim(2))
                .newWorkspace("b", "beta", termSpecSim(3), "tB0", "leafB")
                .moveTabAsSplit("tA1", "leafB", edge, "newLeaf");
            s.run();
            verifyClean(s);
            const auto& m = *s.state();
            const auto* wsB = m.workspace(s.workspaceFor("b"));
            VERIFY_IS_NOT_NULL(wsB);
            VERIFY_IS_TRUE(std::holds_alternative<SplitPane>(wsB->root));
            const auto& sp = std::get<SplitPane>(wsB->root);
            const Axis expectedAxis = (edge == Edge::Top || edge == Edge::Bottom)
                                        ? Axis::Horizontal
                                        : Axis::Vertical;
            VERIFY_IS_TRUE(sp.axis == expectedAxis);
        }
    }

    // ==================================================================
    // Settings-as-content
    // ==================================================================

    void BehavioralTests::Settings_FindFirstAndSelect_NoDuplicate()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "term", "leafA")
            .newTab("settings", "a", "leafA", SettingsSpec{}, "Settings");
        s.run();
        verifyClean(s);
        const auto& m = *s.state();
        // findFirstTabOfKind<SettingsSpec> returns the existing Settings
        // tab; the UI policy is then to selectTab(existing) instead of
        // newTab again — i.e. the model holds exactly one Settings tab.
        const TabRecord* found = m.findFirstTabOfKind<SettingsSpec>();
        VERIFY_IS_NOT_NULL(found);
        VERIFY_IS_TRUE(found->id == s.tabFor("settings"));

        // Now exercise the "select existing instead of duplicate" path:
        Script s2;
        s2.newWorkspace("a", "alpha", termSpecSim(1), "term", "leafA")
            .newTab("settings", "a", "leafA", SettingsSpec{}, "Settings")
            // Imagine the UI handler. It queries; finds one; selectTab.
            .selectTab("settings");
        s2.run();
        verifyClean(s2);
        const auto& m2 = *s2.state();
        // Still exactly one Settings tab.
        int settingsCount = 0;
        for (const auto& w : m2.workspaces)
        {
            const auto& leaf = std::get<LeafPane>(w.root);
            for (const auto& t : leaf.tabs)
            {
                if (std::holds_alternative<SettingsSpec>(t.description)) settingsCount++;
            }
        }
        VERIFY_ARE_EQUAL(1, settingsCount);
    }

    // ==================================================================
    // All five content variants
    // ==================================================================

    void BehavioralTests::AllFiveContentVariants_MountContentOpEachKind()
    {
        // The model accepts all 5 TabContent variants as first-class tab
        // descriptions, but MountContent is registry-driven (Slice 7) and
        // not emitted from pure mutator chains. The model-side proof that
        // each variant works end-to-end is: newTab succeeds, the resulting
        // state passes validate(), and the final state's tabs contain a
        // TabRecord per variant kind. We assert on the state directly
        // rather than on MountContent ops, which won't fire until cutover.
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(7), "term", "leafA")
            .newTab("settings", "a", "leafA", SettingsSpec{}, "Settings")
            .newTab("snippets", "a", "leafA", SnippetsSpec{}, "Snippets")
            .newTab("md", "a", "leafA",
                    MarkdownSpec{ std::filesystem::path{ "C:/docs/readme.md" } }, "Readme")
            .newTab("scratch", "a", "leafA", ScratchpadSpec{}, "Scratchpad");
        s.run();
        verifyClean(s);

        const auto& state = s.state();
        VERIFY_IS_NOT_NULL(state.get());

        bool sawTerminal = false, sawSettings = false, sawSnippets = false;
        bool sawMarkdown = false, sawScratchpad = false;
        for (const auto& ws : state->workspaces)
        {
            const auto* leaf = std::get_if<LeafPane>(&ws.root);
            if (!leaf)
            {
                continue;
            }
            for (const auto& tab : leaf->tabs)
            {
                if (std::holds_alternative<TerminalSpec>(tab.description))
                    sawTerminal = true;
                else if (std::holds_alternative<SettingsSpec>(tab.description))
                    sawSettings = true;
                else if (std::holds_alternative<SnippetsSpec>(tab.description))
                    sawSnippets = true;
                else if (std::holds_alternative<MarkdownSpec>(tab.description))
                    sawMarkdown = true;
                else if (std::holds_alternative<ScratchpadSpec>(tab.description))
                    sawScratchpad = true;
            }
        }
        VERIFY_IS_TRUE(sawTerminal);
        VERIFY_IS_TRUE(sawSettings);
        VERIFY_IS_TRUE(sawSnippets);
        VERIFY_IS_TRUE(sawMarkdown);
        VERIFY_IS_TRUE(sawScratchpad);
    }

    // ==================================================================
    // Render-op assertions
    // ==================================================================

    void BehavioralTests::NewWorkspace_EmitsAddAndCreateAndMount()
    {
        // The model never sets TabRecord.mount on creation - ContentId
        // allocation is the future ContentRegistry's job (Slice 7). So the
        // reconciler emits AddWorkspace + CreateLeafPane + AddTab on
        // newWorkspace, but NOT MountContent. MountContent fires only on a
        // mount-field transition, which the cutover layer triggers when it
        // instantiates the live IPaneContent and assigns a ContentId.
        Script s;
        s.snapshot()
            .newWorkspace("a", "alpha", termSpecSim(1), "t0")
            .expectRenderOpSinceLast(Script::isOp<AddWorkspace>(), "AddWorkspace")
            .expectRenderOpSinceLast(Script::isOp<CreateLeafPane>(), "CreateLeafPane")
            .expectRenderOpSinceLast(Script::isOp<AddTab>(), "AddTab");
        s.run();
        verifyClean(s);
    }

    void BehavioralTests::SplitPane_EmitsCreateSplitAndCreateLeafAndAddTab()
    {
        // MountContent is registry-driven (Slice 7) and not emitted from
        // pure mutator chains; see NewWorkspace_EmitsAddAndCreateAndMount.
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .snapshot()
            .splitPane("leafA", Axis::Vertical, 0.5, termSpecSim(2), "leafB", "tB")
            .expectRenderOpSinceLast(Script::isOp<CreateSplitPane>(), "CreateSplitPane")
            .expectRenderOpSinceLast(Script::isOp<CreateLeafPane>(), "CreateLeafPane (new)")
            .expectRenderOpSinceLast(Script::isOp<AddTab>(), "AddTab (new tab in new leaf)");
        s.run();
        verifyClean(s);
    }

    // ==================================================================
    // Cascade rules
    // ==================================================================

    void BehavioralTests::Cascade_CloseTab_LastTab_RemovesLeafAndCollapsesSplit()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .splitPane("leafA", Axis::Vertical, 0.5, termSpecSim(2), "leafB", "tB")
            // leafA has 1 tab. Close it → leaf disappears → split
            // collapses → leafB becomes workspace root.
            .closeTab("t0")
            .expect([&s](const WorkspaceModelData& m) {
                if (m.workspaces.size() != 1) return false;
                if (!std::holds_alternative<LeafPane>(m.workspaces[0].root)) return false;
                const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
                return leaf.id == s.leafFor("leafB");
            }, "split collapsed; leafB is root");
        s.run();
        verifyClean(s);
    }

    void BehavioralTests::Cascade_CloseWorkspace_LastOne_EmptyState()
    {
        Script s;
        s.newWorkspace("a", "alpha", termSpecSim(1), "t0", "leafA")
            .snapshot()
            .closeWorkspace("a")
            .expect([](const WorkspaceModelData& m) {
                return m.workspaces.empty() && !m.activeWorkspaceId.has_value() && m.mru.empty();
            }, "empty state")
            .expectRenderOpSinceLast(Script::isOp<RemoveWorkspace>(), "RemoveWorkspace")
            .expectRenderOpSinceLast(
                [](const RenderOp& op) {
                    const auto* saw = std::get_if<SetActiveWorkspace>(&op);
                    return saw && !saw->id.has_value();
                },
                "SetActiveWorkspace(nullopt)");
        s.run();
        verifyClean(s);
    }
}
