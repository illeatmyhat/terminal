// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "WAL.h"

#include "Serializer.h"

#include <fstream>
#include <sstream>
#include <string>

namespace WorkspaceModel
{
    namespace
    {
        // Try to parse one JSON line into a LogEntry. Returns nullopt on
        // any failure (parse error, schema error, missing fields). The WAL
        // writer treats failed lines as "skip"; in particular a malformed
        // trailing line from a torn write is silently dropped.
        std::optional<LogEntry> tryParseLine(const std::string& line)
        {
            // Skip empty / whitespace-only lines, which can appear after
            // truncate() leaves a trailing newline on some platforms or
            // when a previous incomplete write left only '\n'.
            const auto firstNonWs = line.find_first_not_of(" \t\r\n");
            if (firstNonWs == std::string::npos)
            {
                return std::nullopt;
            }
            try
            {
                const auto v = parseJson(line);
                return logEntryFromJson(v);
            }
            catch (const SerializerError&)
            {
                return std::nullopt;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }
    } // namespace

    ActionLog::ActionLog(std::filesystem::path logPath) :
        _path{ std::move(logPath) },
        _seq{ 0 }
    {
        // Pre-existing log: scan to find the max seq so resume is correct.
        // We read line by line; well-formed lines bump _seq, malformed are
        // skipped. The constructor never creates the file -- append() does
        // that lazily on first write.
        if (!std::filesystem::exists(_path))
        {
            return;
        }
        std::ifstream in{ _path, std::ios::in | std::ios::binary };
        if (!in.is_open())
        {
            return;
        }
        std::string line;
        while (std::getline(in, line))
        {
            const auto entry = tryParseLine(line);
            if (entry.has_value() && entry->seq > _seq)
            {
                _seq = entry->seq;
            }
        }
    }

    void ActionLog::append(const OpRecord& op)
    {
        LogEntry entry;
        entry.seq = _seq + 1;
        entry.timestamp = nowIso8601();
        entry.op = op;

        const auto j = logEntryToJson(entry);
        std::string line = writeCompact(j);
        line.push_back('\n');

        // Open in append + binary mode. Append mode means we don't have to
        // seek to end; binary mode means no platform-specific newline
        // translation gets between us and the byte we wrote. We perform a
        // single write() call so the OS sees one I/O for the whole line,
        // satisfying the "atomic per record" contract on Windows.
        std::ofstream out{ _path,
                           std::ios::out | std::ios::app | std::ios::binary };
        if (!out.is_open())
        {
            // We can't recover here without imposing a policy; surface
            // the failure as an exception so callers (the future
            // mutation loop) can route it through their error handler.
            throw SerializerError("failed to open WAL for append: " +
                                  _path.string());
        }
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.flush();

        _seq = entry.seq;
    }

    std::vector<LogEntry> ActionLog::readAll() const
    {
        std::vector<LogEntry> out;
        if (!std::filesystem::exists(_path))
        {
            return out;
        }
        std::ifstream in{ _path, std::ios::in | std::ios::binary };
        if (!in.is_open())
        {
            return out;
        }
        std::string line;
        while (std::getline(in, line))
        {
            auto entry = tryParseLine(line);
            if (entry.has_value())
            {
                out.push_back(std::move(*entry));
            }
        }
        return out;
    }

    void ActionLog::truncate()
    {
        // Truncating via std::ofstream with trunc mode is the simplest
        // way to zero the file without unlinking it. The next append()
        // will start writing at offset 0 again.
        std::ofstream out{ _path,
                           std::ios::out | std::ios::trunc | std::ios::binary };
        out.close();
        _seq = 0;
    }
}
