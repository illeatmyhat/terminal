// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// File-backed append-only write-ahead log for OpRecord mutations.
//
// The log file is line-delimited JSON: each LogEntry is one line, written
// in a single std::ostream::write() call (record JSON + '\n'). This gives
// us the "atomic per-record" property the PRD asks for on Windows -- the
// OS buffers the write but never interleaves bytes from another writer's
// single write call. We do NOT fsync per record; that's an explicit PRD
// trade-off and means a hard crash MAY lose the tail of the file. The
// replay engine treats a malformed trailing line as "the last record never
// finished writing" and stops cleanly there.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "ActionLog.h"

namespace WorkspaceModel
{
    class ActionLog
    {
    public:
        // Open / create the log at `logPath`. If the file already exists,
        // its current sequence numbers are scanned so subsequent append()
        // calls produce monotonically increasing seq numbers. If the file
        // does not exist, the first appended record gets seq == 1.
        //
        // The constructor does NOT validate the file's contents beyond
        // reading the max seq number; corrupt lines are tolerated (the
        // last well-formed entry's seq is what we resume from).
        explicit ActionLog(std::filesystem::path logPath);

        // Append a single OpRecord to the log. The record is wrapped in a
        // LogEntry (seq auto-assigned, ts == nowIso8601()) and written as
        // one JSON line followed by '\n' in a single std::ofstream::write
        // call. The stream is flushed before returning -- there is no
        // explicit fsync.
        void append(const OpRecord& op);

        // Read every well-formed LogEntry from the file, in the order they
        // appear. Malformed lines are skipped (with no callback to the
        // caller). A malformed trailing line is treated identically to a
        // missing line -- so a hard crash that left a half-written record
        // produces a clean replay up to the last good entry.
        [[nodiscard]] std::vector<LogEntry> readAll() const;

        // Truncate the log to zero bytes. Used by squash().
        void truncate();

        // The seq number that the NEXT append() will assign.
        [[nodiscard]] std::uint64_t nextSeq() const noexcept { return _seq + 1; }

        // The path the log is bound to. Useful for the bug-report bundler.
        [[nodiscard]] const std::filesystem::path& path() const noexcept { return _path; }

    private:
        std::filesystem::path _path;
        // Highest seq number observed; the next append() assigns _seq + 1.
        std::uint64_t _seq{ 0 };
    };
}
