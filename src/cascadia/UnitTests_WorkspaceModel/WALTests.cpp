// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// File-backed ActionLog (WAL) tests. Each test runs against a per-test
// temp directory under
//   %TEMP%/WorkspaceModelTests/WAL_<TestName>/
// and cleans up at the end.

#include "pch.h"

#include "TestHelpers.h"

#include "../WorkspaceModel/ActionLog.h"
#include "../WorkspaceModel/WAL.h"

#include <fstream>

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        std::filesystem::path tempDirFor(const char* testName)
        {
            auto d = std::filesystem::temp_directory_path() /
                     "WorkspaceModelTests" /
                     (std::string{ "WAL_" } + testName);
            std::error_code ec;
            std::filesystem::remove_all(d, ec);
            std::filesystem::create_directories(d, ec);
            return d;
        }

        OpRecord sampleOp(std::uint64_t v)
        {
            // A simple, deterministic op for testing append/readAll: we
            // reuse SetSidebarWidthRecord which has no ID dependencies.
            return OpRecord{ SetSidebarWidthRecord{ static_cast<double>(v) } };
        }
    }

    class WALTests
    {
        TEST_CLASS(WALTests);

        TEST_METHOD(AppendThenReadAll_PreservesOrder);
        TEST_METHOD(AppendThenReadAll_PreservesSeqMonotonic);
        TEST_METHOD(Truncate_ClearsContents);
        TEST_METHOD(Reopen_ResumesSeqFromExistingFile);
        TEST_METHOD(MalformedTrailingLine_IsSkipped);
        TEST_METHOD(EveryOpRecordKind_RoundTripsThroughWAL);
    };

    void WALTests::AppendThenReadAll_PreservesOrder()
    {
        const auto dir = tempDirFor("AppendThenReadAll_PreservesOrder");
        const auto logPath = dir / "actions.log";

        ActionLog log{ logPath };
        for (std::uint64_t i = 0; i < 5; ++i)
        {
            log.append(sampleOp(i * 10));
        }

        const auto entries = log.readAll();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(5), entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto* rec = std::get_if<SetSidebarWidthRecord>(&entries[i].op);
            VERIFY_IS_NOT_NULL(rec);
            VERIFY_ARE_EQUAL(static_cast<double>(i * 10), rec->width);
        }
    }

    void WALTests::AppendThenReadAll_PreservesSeqMonotonic()
    {
        const auto dir = tempDirFor("AppendThenReadAll_PreservesSeqMonotonic");
        const auto logPath = dir / "actions.log";

        ActionLog log{ logPath };
        log.append(sampleOp(0));
        log.append(sampleOp(1));
        log.append(sampleOp(2));

        const auto entries = log.readAll();
        VERIFY_ARE_EQUAL(static_cast<std::uint64_t>(1), entries[0].seq);
        VERIFY_ARE_EQUAL(static_cast<std::uint64_t>(2), entries[1].seq);
        VERIFY_ARE_EQUAL(static_cast<std::uint64_t>(3), entries[2].seq);
    }

    void WALTests::Truncate_ClearsContents()
    {
        const auto dir = tempDirFor("Truncate_ClearsContents");
        const auto logPath = dir / "actions.log";

        ActionLog log{ logPath };
        for (std::uint64_t i = 0; i < 3; ++i)
        {
            log.append(sampleOp(i));
        }
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(3), log.readAll().size());

        log.truncate();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(0), log.readAll().size());

        // After truncate, the next append starts seq from 1 again.
        log.append(sampleOp(99));
        const auto entries = log.readAll();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(1), entries.size());
        VERIFY_ARE_EQUAL(static_cast<std::uint64_t>(1), entries[0].seq);
    }

    void WALTests::Reopen_ResumesSeqFromExistingFile()
    {
        const auto dir = tempDirFor("Reopen_ResumesSeqFromExistingFile");
        const auto logPath = dir / "actions.log";

        {
            ActionLog log{ logPath };
            log.append(sampleOp(1));
            log.append(sampleOp(2));
        }

        // Reopen: nextSeq() should reflect the existing max + 1.
        ActionLog log2{ logPath };
        VERIFY_ARE_EQUAL(static_cast<std::uint64_t>(3), log2.nextSeq());
        log2.append(sampleOp(3));
        const auto entries = log2.readAll();
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(3), entries.size());
        VERIFY_ARE_EQUAL(static_cast<std::uint64_t>(3), entries[2].seq);
    }

    void WALTests::MalformedTrailingLine_IsSkipped()
    {
        const auto dir = tempDirFor("MalformedTrailingLine_IsSkipped");
        const auto logPath = dir / "actions.log";

        ActionLog log{ logPath };
        log.append(sampleOp(1));
        log.append(sampleOp(2));

        // Manually append a half-written line, simulating a torn write.
        {
            std::ofstream out{ logPath, std::ios::out | std::ios::app | std::ios::binary };
            const std::string bad = R"({"seq": 99, "ts": "2026-)";
            out.write(bad.data(), static_cast<std::streamsize>(bad.size()));
        }

        const auto entries = log.readAll();
        // Only the 2 well-formed entries survive.
        VERIFY_ARE_EQUAL(static_cast<std::size_t>(2), entries.size());
    }

    void WALTests::EveryOpRecordKind_RoundTripsThroughWAL()
    {
        const auto dir = tempDirFor("EveryOpRecordKind_RoundTripsThroughWAL");
        const auto logPath = dir / "actions.log";

        // Construct one of each record kind.
        std::vector<OpRecord> ops;
        ops.emplace_back(NewWorkspaceRecord{ "alpha", TerminalSpec{}, "title", Color{ 1, 2, 3, 4 }, true });
        ops.emplace_back(CloseWorkspaceRecord{ WorkspaceId{ 1 } });
        ops.emplace_back(CloseOtherWorkspacesRecord{ WorkspaceId{ 2 } });
        ops.emplace_back(CloseAllWorkspacesRecord{});
        ops.emplace_back(SwitchToWorkspaceRecord{ WorkspaceId{ 3 } });
        ops.emplace_back(RenameWorkspaceRecord{ WorkspaceId{ 4 }, "renamed" });
        ops.emplace_back(SetWorkspaceColorRecord{ WorkspaceId{ 5 }, Color{ 10, 20, 30, 40 } });
        ops.emplace_back(SetWorkspaceDescriptionRecord{ WorkspaceId{ 6 }, "desc" });
        ops.emplace_back(SetWorkspacePinnedRecord{ WorkspaceId{ 7 }, true });
        ops.emplace_back(ReorderWorkspaceRecord{ WorkspaceId{ 8 }, 2 });
        ops.emplace_back(NewTabRecord{ WorkspaceId{ 1 }, PaneId{ 2 }, SettingsSpec{}, "Settings", std::nullopt, false });
        ops.emplace_back(CloseTabRecord{ TabId{ 9 } });
        ops.emplace_back(CloseTabsRightRecord{ TabId{ 10 } });
        ops.emplace_back(CloseOtherTabsRecord{ TabId{ 11 } });
        ops.emplace_back(SelectTabRecord{ TabId{ 12 } });
        ops.emplace_back(SetTabTitleRecord{ TabId{ 13 }, "new title" });
        ops.emplace_back(SetTabColorRecord{ TabId{ 14 }, std::nullopt });
        ops.emplace_back(SetTabPinnedRecord{ TabId{ 15 }, false });
        MarkdownSpec md;
        md.file = std::filesystem::path{ "C:/x.md" };
        ops.emplace_back(SplitPaneRecord{ PaneId{ 16 }, Axis::Horizontal, 0.4, md, "doc", Color{ 0xAA, 0xBB, 0xCC, 0xDD }, true });
        ops.emplace_back(ClosePaneRecord{ PaneId{ 17 } });
        ops.emplace_back(ResizePaneRecord{ PaneId{ 18 }, 0.65 });
        ops.emplace_back(FocusPaneRecord{ PaneId{ 19 } });
        ops.emplace_back(MoveTabRecord{ TabId{ 20 }, PaneId{ 21 }, 3 });
        ops.emplace_back(MoveTabAsSplitRecord{ TabId{ 22 }, PaneId{ 23 }, Edge::Top });
        ops.emplace_back(SetSidebarWidthRecord{ 250.5 });

        ActionLog log{ logPath };
        for (const auto& op : ops)
        {
            log.append(op);
        }

        const auto entries = log.readAll();
        VERIFY_ARE_EQUAL(ops.size(), entries.size());
        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            VERIFY_IS_TRUE(ops[i] == entries[i].op);
        }
    }
}
