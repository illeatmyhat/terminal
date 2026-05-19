// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Validator fixture tests. For each of the 8 invariants enforced by
// WorkspaceModel::validate(), we provide one passing and one failing
// fixture state.
//
// Helper builders (`makeLeaf`, `makeSplit`, `makeWorkspace`, `makeModel`)
// at the top of the file produce minimal well-formed shapes. Each test
// then mutates a copy into the exact failing or passing state it wants
// to assert.

#include "pch.h"

#include "../WorkspaceModel/WorkspaceState.h"
#include "../WorkspaceModel/Validator.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Builds a TerminalSpec tab with the given TabId and an optional
        // mount handle. The profile bytes are deterministic so equality
        // checks behave.
        TabRecord makeTab(std::uint64_t tabId, std::optional<std::uint64_t> mount = std::nullopt)
        {
            TabRecord t;
            t.id = TabId{ tabId };
            TerminalSpec spec{};
            spec.profile[0] = static_cast<std::uint8_t>(tabId & 0xFF);
            t.description = spec;
            if (mount.has_value())
            {
                t.mount = ContentId{ *mount };
            }
            return t;
        }

        // Builds a leaf with `tabCount` distinct tabs, with the given PaneId.
        // Tab ids are paneId * 100 + i so they don't collide across panes
        // built with different paneIds.
        LeafPane makeLeaf(std::uint64_t paneId, std::size_t tabCount = 1)
        {
            LeafPane leaf;
            leaf.id = PaneId{ paneId };
            for (std::size_t i = 0; i < tabCount; ++i)
            {
                leaf.tabs.push_back(makeTab(paneId * 100 + i + 1));
            }
            leaf.activeTabIdx = 0;
            return leaf;
        }

        // Wraps two PaneNode subtrees into a shared SplitPane.
        SplitPane makeSplit(std::uint64_t paneId,
                            const PaneNode& left,
                            const PaneNode& right,
                            Axis axis = Axis::Vertical,
                            double ratio = 0.5)
        {
            SplitPane s;
            s.id = PaneId{ paneId };
            s.axis = axis;
            s.ratio = ratio;
            s.left = std::make_shared<PaneNode>(left);
            s.right = std::make_shared<PaneNode>(right);
            return s;
        }

        // Builds a workspace whose root is a single leaf with the given
        // PaneId and `tabCount` tabs. The activePaneId is set to the leaf.
        WorkspaceState makeWorkspace(std::uint64_t workspaceId,
                                     std::uint64_t paneId,
                                     std::size_t tabCount = 1)
        {
            WorkspaceState ws;
            ws.id = WorkspaceId{ workspaceId };
            ws.name = "ws";
            ws.root = makeLeaf(paneId, tabCount);
            ws.activePaneId = PaneId{ paneId };
            return ws;
        }

        // Builds a model with a single one-leaf workspace and a single
        // MRU entry. Active workspace is the only workspace.
        WorkspaceModelData makeModel()
        {
            WorkspaceModelData m;
            auto ws = makeWorkspace(1, 1);
            m.workspaces.push_back(ws);
            m.activeWorkspaceId = ws.id;
            m.mru.push_back(ws.id);
            m.idCounter = 10;
            return m;
        }
    }

    class ValidatorTests
    {
        TEST_CLASS(ValidatorTests);

        // -------------------------------------------------------------
        // Sanity: a freshly-built model from makeModel() is valid.
        // -------------------------------------------------------------
        TEST_METHOD(FreshModel_PassesValidation);

        // Invariant 1: every leaf has at least one tab.
        TEST_METHOD(LeafEmpty_FailsValidation);
        TEST_METHOD(LeafEmpty_PassesWhenNonEmpty);

        // Invariant 2: every split has exactly two children, both non-null.
        TEST_METHOD(SplitArityWrong_FailsWhenChildMissing);
        TEST_METHOD(SplitArityWrong_PassesWithTwoChildren);

        // Invariant 3: every workspace has a root pane with a valid id.
        TEST_METHOD(WorkspaceWithoutRoot_FailsWhenRootIdZero);
        TEST_METHOD(WorkspaceWithoutRoot_PassesWithValidRootId);

        // Invariant 4: activeTabIdx in [0, tabs.size()).
        TEST_METHOD(ActiveTabIdxOutOfRange_FailsWhenPastEnd);
        TEST_METHOD(ActiveTabIdxOutOfRange_PassesWhenZero);

        // Invariant 5: activePaneId references an existing leaf.
        TEST_METHOD(ActivePaneIdInvalid_FailsWhenUnknown);
        TEST_METHOD(ActivePaneIdInvalid_PassesWhenReferencesLeaf);

        // Invariant 6: activeWorkspaceId is nullopt or refers to a workspace.
        TEST_METHOD(ActiveWorkspaceIdInvalid_FailsWhenUnknown);
        TEST_METHOD(ActiveWorkspaceIdInvalid_PassesWhenNullopt);
        TEST_METHOD(ActiveWorkspaceIdInvalid_PassesWhenValid);

        // Invariant 7: MRU is a permutation of workspace ids.
        TEST_METHOD(MruNotPermutationOfWorkspaces_FailsOnSizeMismatch);
        TEST_METHOD(MruNotPermutationOfWorkspaces_FailsOnUnknownId);
        TEST_METHOD(MruNotPermutationOfWorkspaces_PassesWhenPermutation);

        // Invariant 8: every set TabRecord.mount ContentId is unique.
        TEST_METHOD(DuplicateContentIdMount_FailsOnSameMount);
        TEST_METHOD(DuplicateContentIdMount_PassesWhenDistinct);
        TEST_METHOD(DuplicateContentIdMount_PassesWhenAllUnset);
    };

    // -----------------------------------------------------------------
    void ValidatorTests::FreshModel_PassesValidation()
    {
        const auto m = makeModel();
        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 1: LeafEmpty
    // -----------------------------------------------------------------
    void ValidatorTests::LeafEmpty_FailsValidation()
    {
        auto m = makeModel();
        // Drop the leaf's only tab; activeTabIdx becomes simultaneously
        // out of range but invariant 1 is checked first.
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        leaf.tabs.clear();
        leaf.activeTabIdx = 0;

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::LeafEmpty), static_cast<int>(*v));
    }

    void ValidatorTests::LeafEmpty_PassesWhenNonEmpty()
    {
        auto m = makeModel();
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        // Confirm the precondition that the leaf is non-empty.
        VERIFY_IS_FALSE(leaf.tabs.empty());

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 2: SplitArityWrong
    // -----------------------------------------------------------------
    void ValidatorTests::SplitArityWrong_FailsWhenChildMissing()
    {
        auto m = makeModel();
        // Replace the single-leaf root with a split whose right child is
        // null. Both children must be non-null for a well-formed split.
        auto leftLeaf = makeLeaf(2);
        auto split = makeSplit(3, PaneNode{ leftLeaf }, PaneNode{ makeLeaf(4) });
        split.right.reset(); // <- the deliberate corruption

        m.workspaces[0].root = split;
        m.workspaces[0].activePaneId = PaneId{ 2 };

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::SplitArityWrong), static_cast<int>(*v));
    }

    void ValidatorTests::SplitArityWrong_PassesWithTwoChildren()
    {
        auto m = makeModel();
        auto left = makeLeaf(2);
        auto right = makeLeaf(3);
        m.workspaces[0].root = makeSplit(4, PaneNode{ left }, PaneNode{ right });
        m.workspaces[0].activePaneId = PaneId{ 2 };

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 3: WorkspaceWithoutRoot
    // -----------------------------------------------------------------
    void ValidatorTests::WorkspaceWithoutRoot_FailsWhenRootIdZero()
    {
        auto m = makeModel();
        // Zero-valued PaneId means "no pane assigned" — a workspace must
        // have a real root pane, so this is a violation.
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        leaf.id = PaneId{ 0 };
        m.workspaces[0].activePaneId = PaneId{ 0 };

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::WorkspaceWithoutRoot), static_cast<int>(*v));
    }

    void ValidatorTests::WorkspaceWithoutRoot_PassesWithValidRootId()
    {
        auto m = makeModel();
        const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        VERIFY_IS_TRUE(leaf.id.valid());

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 4: ActiveTabIdxOutOfRange
    // -----------------------------------------------------------------
    void ValidatorTests::ActiveTabIdxOutOfRange_FailsWhenPastEnd()
    {
        auto m = makeModel();
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), leaf.tabs.size());
        leaf.activeTabIdx = 5;

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::ActiveTabIdxOutOfRange), static_cast<int>(*v));
    }

    void ValidatorTests::ActiveTabIdxOutOfRange_PassesWhenZero()
    {
        auto m = makeModel();
        // Add a second tab and point activeTabIdx at it explicitly. Both
        // 0 and 1 are valid for a 2-tab leaf.
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        leaf.tabs.push_back(makeTab(999));
        leaf.activeTabIdx = 1;

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 5: ActivePaneIdInvalid
    // -----------------------------------------------------------------
    void ValidatorTests::ActivePaneIdInvalid_FailsWhenUnknown()
    {
        auto m = makeModel();
        // The only pane in this workspace has PaneId{1}; pointing at 99
        // is not a valid leaf reference.
        m.workspaces[0].activePaneId = PaneId{ 99 };

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::ActivePaneIdInvalid), static_cast<int>(*v));
    }

    void ValidatorTests::ActivePaneIdInvalid_PassesWhenReferencesLeaf()
    {
        auto m = makeModel();
        // Promote the root into a split. activePaneId must reference one
        // of the two leaves, not the split node itself.
        auto left = makeLeaf(7);
        auto right = makeLeaf(8);
        m.workspaces[0].root = makeSplit(9, PaneNode{ left }, PaneNode{ right });
        m.workspaces[0].activePaneId = PaneId{ 8 };

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 6: ActiveWorkspaceIdInvalid
    // -----------------------------------------------------------------
    void ValidatorTests::ActiveWorkspaceIdInvalid_FailsWhenUnknown()
    {
        auto m = makeModel();
        m.activeWorkspaceId = WorkspaceId{ 42 };

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::ActiveWorkspaceIdInvalid),
                         static_cast<int>(*v));
    }

    void ValidatorTests::ActiveWorkspaceIdInvalid_PassesWhenNullopt()
    {
        // An empty model (no workspaces, activeWorkspaceId == nullopt)
        // is explicitly allowed.
        WorkspaceModelData m;
        VERIFY_IS_FALSE(validate(m).has_value());
    }

    void ValidatorTests::ActiveWorkspaceIdInvalid_PassesWhenValid()
    {
        const auto m = makeModel();
        VERIFY_IS_TRUE(m.activeWorkspaceId.has_value());
        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 7: MruNotPermutationOfWorkspaces
    // -----------------------------------------------------------------
    void ValidatorTests::MruNotPermutationOfWorkspaces_FailsOnSizeMismatch()
    {
        auto m = makeModel();
        // Add a second workspace but forget to add it to the MRU. The MRU
        // is then shorter than the workspaces vector.
        auto ws2 = makeWorkspace(2, 5);
        m.workspaces.push_back(ws2);
        // m.mru is still { WorkspaceId{1} }

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::MruNotPermutationOfWorkspaces),
                         static_cast<int>(*v));
    }

    void ValidatorTests::MruNotPermutationOfWorkspaces_FailsOnUnknownId()
    {
        auto m = makeModel();
        // Replace the single MRU entry with a workspace id that doesn't
        // exist. Sizes match but the set of ids does not.
        m.mru.clear();
        m.mru.push_back(WorkspaceId{ 999 });

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::MruNotPermutationOfWorkspaces),
                         static_cast<int>(*v));
    }

    void ValidatorTests::MruNotPermutationOfWorkspaces_PassesWhenPermutation()
    {
        auto m = makeModel();
        // Add a second workspace and a corresponding MRU entry; permute
        // the MRU so its order differs from the workspaces vector.
        auto ws2 = makeWorkspace(2, 5);
        m.workspaces.push_back(ws2);
        m.mru.clear();
        m.mru.push_back(WorkspaceId{ 2 });
        m.mru.push_back(WorkspaceId{ 1 });

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    // -----------------------------------------------------------------
    // Invariant 8: DuplicateContentIdMount
    // -----------------------------------------------------------------
    void ValidatorTests::DuplicateContentIdMount_FailsOnSameMount()
    {
        auto m = makeModel();
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        leaf.tabs[0].mount = ContentId{ 7 };
        leaf.tabs.push_back(makeTab(998, /*mount=*/7));

        const auto v = validate(m);
        VERIFY_IS_TRUE(v.has_value());
        VERIFY_ARE_EQUAL(static_cast<int>(Violation::DuplicateContentIdMount),
                         static_cast<int>(*v));
    }

    void ValidatorTests::DuplicateContentIdMount_PassesWhenDistinct()
    {
        auto m = makeModel();
        auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        leaf.tabs[0].mount = ContentId{ 1 };
        leaf.tabs.push_back(makeTab(998, /*mount=*/2));

        VERIFY_IS_FALSE(validate(m).has_value());
    }

    void ValidatorTests::DuplicateContentIdMount_PassesWhenAllUnset()
    {
        const auto m = makeModel();
        const auto& leaf = std::get<LeafPane>(m.workspaces[0].root);
        for (const auto& t : leaf.tabs)
        {
            VERIFY_IS_FALSE(t.mount.has_value());
        }
        VERIFY_IS_FALSE(validate(m).has_value());
    }
}
