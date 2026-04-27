// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/WorkspacePlacement.h"
#include "../TerminalApp/WorkspaceList.h"
#include "../TerminalSettingsModel/WorkspaceState.h"

using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace TerminalApp;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

namespace TerminalAppLocalTests
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

        std::vector<bool> pinnedFlags(std::initializer_list<bool> flags)
        {
            return std::vector<bool>{ flags };
        }
    }

    // -----------------------------------------------------------------------
    // WorkspacePlacement: table-driven over policy x pinned-set x current-position
    // -----------------------------------------------------------------------
    class WorkspacePlacementTests
    {
        BEGIN_TEST_CLASS(WorkspacePlacementTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(EmptyList);
        TEST_METHOD(NoPinned_TopAfterCurrentEnd);
        TEST_METHOD(AllPinned_NewUnpinnedAlwaysAtEnd);
        TEST_METHOD(MixedRegions_NewPinnedStaysInPinnedRegion);
        TEST_METHOD(MixedRegions_NewUnpinnedStaysInUnpinnedRegion);
        TEST_METHOD(AfterCurrent_FallsBackWhenCurrentInOtherRegion);
        TEST_METHOD(AfterCurrent_NoCurrent);
    };

    void WorkspacePlacementTests::EmptyList()
    {
        const std::vector<bool> empty;
        for (const auto policy : { WorkspacePlacementPolicy::Top, WorkspacePlacementPolicy::AfterCurrent, WorkspacePlacementPolicy::End })
        {
            VERIFY_ARE_EQUAL(0u, WorkspacePlacement::ResolveInsertionIndex(policy, empty, std::nullopt, false));
            VERIFY_ARE_EQUAL(0u, WorkspacePlacement::ResolveInsertionIndex(policy, empty, std::nullopt, true));
        }
    }

    void WorkspacePlacementTests::NoPinned_TopAfterCurrentEnd()
    {
        const auto flags = pinnedFlags({ false, false, false });

        VERIFY_ARE_EQUAL(0u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::Top, flags, 1u, false));
        VERIFY_ARE_EQUAL(2u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 1u, false));
        VERIFY_ARE_EQUAL(3u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::End, flags, 1u, false));
    }

    void WorkspacePlacementTests::AllPinned_NewUnpinnedAlwaysAtEnd()
    {
        const auto flags = pinnedFlags({ true, true, true });

        // The new (unpinned) workspace can't enter the pinned region, so Top
        // means "top of the unpinned region" which is right after the pinned
        // tail (index 3), and End is also 3.
        VERIFY_ARE_EQUAL(3u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::Top, flags, 0u, false));
        VERIFY_ARE_EQUAL(3u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::End, flags, 2u, false));
        // AfterCurrent: current is inside the pinned region, so we fall back
        // to the end of the unpinned (empty) region.
        VERIFY_ARE_EQUAL(3u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 1u, false));
    }

    void WorkspacePlacementTests::MixedRegions_NewPinnedStaysInPinnedRegion()
    {
        // Layout: [pinned, pinned, unpinned, unpinned, unpinned]
        const auto flags = pinnedFlags({ true, true, false, false, false });

        // Inserting another pinned workspace must end up inside [0, 2].
        VERIFY_ARE_EQUAL(0u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::Top, flags, 3u, true));
        VERIFY_ARE_EQUAL(2u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::End, flags, 3u, true));
        // Current is in the unpinned region — AfterCurrent falls back to the
        // end of the pinned region.
        VERIFY_ARE_EQUAL(2u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 3u, true));

        // When current points inside the pinned region, AfterCurrent inserts
        // immediately after it.
        VERIFY_ARE_EQUAL(2u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 1u, true));
        VERIFY_ARE_EQUAL(1u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 0u, true));
    }

    void WorkspacePlacementTests::MixedRegions_NewUnpinnedStaysInUnpinnedRegion()
    {
        // Layout: [pinned, pinned, unpinned, unpinned, unpinned]
        const auto flags = pinnedFlags({ true, true, false, false, false });

        VERIFY_ARE_EQUAL(2u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::Top, flags, 3u, false));
        VERIFY_ARE_EQUAL(5u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::End, flags, 3u, false));
        // AfterCurrent inside the unpinned region.
        VERIFY_ARE_EQUAL(4u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 3u, false));
        // Current pointing at the last unpinned: clamped to regionEnd.
        VERIFY_ARE_EQUAL(5u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 4u, false));
    }

    void WorkspacePlacementTests::AfterCurrent_FallsBackWhenCurrentInOtherRegion()
    {
        const auto flags = pinnedFlags({ true, true, false, false });

        // Pinned new workspace, current in unpinned region: fall back to end of pinned region.
        VERIFY_ARE_EQUAL(2u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 2u, true));
        // Unpinned new workspace, current in pinned region: fall back to end of unpinned region.
        VERIFY_ARE_EQUAL(4u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, 0u, false));
    }

    void WorkspacePlacementTests::AfterCurrent_NoCurrent()
    {
        const auto flags = pinnedFlags({ true, false, false });
        VERIFY_ARE_EQUAL(1u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, std::nullopt, true));
        VERIFY_ARE_EQUAL(3u, WorkspacePlacement::ResolveInsertionIndex(WorkspacePlacementPolicy::AfterCurrent, flags, std::nullopt, false));
    }

    // -----------------------------------------------------------------------
    // WorkspaceList: ordering, pin region migration, MRU
    // -----------------------------------------------------------------------
    class WorkspaceListTests
    {
        BEGIN_TEST_CLASS(WorkspaceListTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(InsertAndActivate);
        TEST_METHOD(InsertWithPlacementPolicy);
        TEST_METHOD(TogglePinMovesAcrossRegions);
        TEST_METHOD(SelectLastActiveTogglesBetweenTwo);
        TEST_METHOD(NextPrevWrapAround);
        TEST_METHOD(RemoveActiveSlidesToNeighbor);
        TEST_METHOD(MoveWithinRegion);
        TEST_METHOD(FromStatePreservesPersistedShape);
    };

    void WorkspaceListTests::InsertAndActivate()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::End, true);

        VERIFY_ARE_EQUAL(3u, list.Count());
        VERIFY_IS_TRUE(list.ActiveIndex().has_value());
        VERIFY_ARE_EQUAL(2u, *list.ActiveIndex());
        VERIFY_ARE_EQUAL(L"C", list.At(*list.ActiveIndex()).title);
    }

    void WorkspaceListTests::InsertWithPlacementPolicy()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        // active is now B (index 1)
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::AfterCurrent, false);
        // C should land at index 2 (right after B) without changing active
        VERIFY_ARE_EQUAL(L"C", list.At(2).title);
        VERIFY_ARE_EQUAL(1u, *list.ActiveIndex());

        list.Insert(makeWorkspace(L"Top"), WorkspacePlacementPolicy::Top, false);
        VERIFY_ARE_EQUAL(L"Top", list.At(0).title);
        // Active index B was at 1, now shifted to 2
        VERIFY_ARE_EQUAL(2u, *list.ActiveIndex());
    }

    void WorkspaceListTests::TogglePinMovesAcrossRegions()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::End, true);

        // Pin B: it should jump to the start of the pinned region
        const auto newIndex = list.TogglePin(1);
        VERIFY_ARE_EQUAL(0u, newIndex);
        VERIFY_ARE_EQUAL(L"B", list.At(0).title);
        VERIFY_IS_TRUE(list.At(0).pinned);
        VERIFY_ARE_EQUAL(L"A", list.At(1).title);
        VERIFY_ARE_EQUAL(L"C", list.At(2).title);

        // Active was C at index 2 — still at index 2 (no shift, since B's
        // original index 1 < 2 and new index 0 also < 2 — net zero shift).
        VERIFY_ARE_EQUAL(2u, *list.ActiveIndex());

        // Unpin B: should move back to the end of the unpinned region
        const auto unpinned = list.TogglePin(0);
        VERIFY_ARE_EQUAL(2u, unpinned);
        VERIFY_ARE_EQUAL(L"B", list.At(2).title);
        VERIFY_IS_FALSE(list.At(2).pinned);
    }

    void WorkspaceListTests::SelectLastActiveTogglesBetweenTwo()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::End, true);
        // active = C (index 2), previous = B (index 1)

        list.Activate(0); // A
        // active = A, previous = C
        const auto first = list.SelectLastActive();
        VERIFY_IS_TRUE(first.has_value());
        VERIFY_ARE_EQUAL(2u, *first); // back to C
        // After the toggle, previous should now be A — so a second toggle
        // brings us back to A.
        const auto second = list.SelectLastActive();
        VERIFY_IS_TRUE(second.has_value());
        VERIFY_ARE_EQUAL(0u, *second);
    }

    void WorkspaceListTests::NextPrevWrapAround()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::End, true);

        list.Activate(2);
        VERIFY_ARE_EQUAL(0u, *list.NextIndex());
        VERIFY_ARE_EQUAL(1u, *list.PrevIndex());

        list.Activate(0);
        VERIFY_ARE_EQUAL(1u, *list.NextIndex());
        VERIFY_ARE_EQUAL(2u, *list.PrevIndex());
    }

    void WorkspaceListTests::RemoveActiveSlidesToNeighbor()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::End, true);
        list.Activate(1); // B

        VERIFY_IS_TRUE(list.Remove(1));
        VERIFY_ARE_EQUAL(2u, list.Count());
        VERIFY_IS_TRUE(list.ActiveIndex().has_value());
        // Slot 1 now contains what was at slot 2 (C); active should follow.
        VERIFY_ARE_EQUAL(1u, *list.ActiveIndex());
        VERIFY_ARE_EQUAL(L"C", list.At(*list.ActiveIndex()).title);

        // Removing the very last workspace empties the active slot.
        VERIFY_IS_TRUE(list.Remove(0));
        VERIFY_IS_TRUE(list.Remove(0));
        VERIFY_ARE_EQUAL(0u, list.Count());
        VERIFY_IS_FALSE(list.ActiveIndex().has_value());
    }

    void WorkspaceListTests::MoveWithinRegion()
    {
        WorkspaceList list;
        list.Insert(makeWorkspace(L"A"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"B"), WorkspacePlacementPolicy::End, true);
        list.Insert(makeWorkspace(L"C"), WorkspacePlacementPolicy::End, true);
        list.Activate(0);

        // Move A from 0 → 2
        VERIFY_IS_TRUE(list.Move(0, 2));
        VERIFY_ARE_EQUAL(L"B", list.At(0).title);
        VERIFY_ARE_EQUAL(L"C", list.At(1).title);
        VERIFY_ARE_EQUAL(L"A", list.At(2).title);
        VERIFY_ARE_EQUAL(2u, *list.ActiveIndex());

        // Cross-region moves are rejected (one pinned, one unpinned)
        list.At(0).pinned = true;
        VERIFY_IS_FALSE(list.Move(0, 2));
    }

    void WorkspaceListTests::FromStatePreservesPersistedShape()
    {
        // The persistence read path (TerminalPage::_HydrateWorkspacesFrom
        // PersistedState) hands a `WorkspaceListState` produced by
        // WorkspacePersistence::DeserializeWindowLayout straight into
        // WorkspaceList::FromState. This test pins what gets preserved
        // bit-for-bit through that handoff: workspace count + order, the
        // pin/unpin partition (pinned-prefix-then-unpinned), the persisted
        // active index, and the per-workspace metadata
        // (title/color/description/pinned/id/paneTree). Insert-based
        // hydration would re-sort across the pin boundary and lose the
        // exact persisted order — FromState avoids that, and this guards
        // the choice.
        WorkspaceListState state;

        WorkspaceState pinnedHead;
        pinnedHead.id = 7;
        pinnedHead.title = L"Pinned-Head";
        pinnedHead.pinned = true;
        pinnedHead.runtimeColor = winrt::Windows::UI::Color{ 255, 0x12, 0x34, 0x56 };
        pinnedHead.customDescription = L"first pinned";
        Json::Value pinnedHeadAction{ Json::objectValue };
        pinnedHeadAction["action"] = "newTab";
        pinnedHeadAction["profile"] = "{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}";
        pinnedHead.paneTree.append(std::move(pinnedHeadAction));
        state.workspaces.push_back(std::move(pinnedHead));

        WorkspaceState unpinnedMid;
        unpinnedMid.id = 4;
        unpinnedMid.title = L"Unpinned-Middle";
        unpinnedMid.pinned = false;
        Json::Value mid0{ Json::objectValue };
        mid0["action"] = "newTab";
        mid0["profile"] = "{bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb}";
        Json::Value mid1{ Json::objectValue };
        mid1["action"] = "splitPane";
        mid1["split"] = "right";
        unpinnedMid.paneTree.append(std::move(mid0));
        unpinnedMid.paneTree.append(std::move(mid1));
        state.workspaces.push_back(std::move(unpinnedMid));

        WorkspaceState unpinnedTail;
        unpinnedTail.id = 9;
        unpinnedTail.title = L"Unpinned-Tail";
        unpinnedTail.pinned = false;
        state.workspaces.push_back(std::move(unpinnedTail));

        state.activeIndex = 1; // Unpinned-Middle
        state.previousActiveIndex = 0; // Pinned-Head
        state.sidebarWidth = 314.0;

        const auto list = WorkspaceList::FromState(state);

        VERIFY_ARE_EQUAL(3u, list.Count());
        VERIFY_ARE_EQUAL(L"Pinned-Head", list.At(0).title);
        VERIFY_IS_TRUE(list.At(0).pinned);
        VERIFY_ARE_EQUAL(7u, list.At(0).id);
        VERIFY_IS_TRUE(list.At(0).runtimeColor.has_value());
        VERIFY_ARE_EQUAL(L"first pinned", list.At(0).customDescription);
        VERIFY_ARE_EQUAL(1u, list.At(0).paneTree.size());

        VERIFY_ARE_EQUAL(L"Unpinned-Middle", list.At(1).title);
        VERIFY_IS_FALSE(list.At(1).pinned);
        VERIFY_ARE_EQUAL(4u, list.At(1).id);
        VERIFY_ARE_EQUAL(2u, list.At(1).paneTree.size());
        VERIFY_ARE_EQUAL("splitPane", list.At(1).paneTree[1u]["action"].asString());

        VERIFY_ARE_EQUAL(L"Unpinned-Tail", list.At(2).title);
        VERIFY_ARE_EQUAL(9u, list.At(2).id);

        VERIFY_IS_TRUE(list.ActiveIndex().has_value());
        VERIFY_ARE_EQUAL(1u, *list.ActiveIndex());
        VERIFY_IS_TRUE(list.PreviousActiveIndex().has_value());
        VERIFY_ARE_EQUAL(0u, *list.PreviousActiveIndex());
        VERIFY_IS_TRUE(list.SidebarWidth().has_value());
        VERIFY_ARE_EQUAL(314.0, *list.SidebarWidth());
    }
}
