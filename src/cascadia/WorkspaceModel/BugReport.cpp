// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "BugReport.h"

#include "Serializer.h"

#include <fstream>
#include <system_error>

namespace WorkspaceModel
{
    void dumpBugReport(const WorkspaceModelData& state,
                       const std::filesystem::path& logPath,
                       const std::filesystem::path& destDir)
    {
        std::error_code ec;
        std::filesystem::create_directories(destDir, ec);
        if (ec && !std::filesystem::exists(destDir))
        {
            throw SerializerError("dumpBugReport: failed to create destDir: " +
                                  ec.message());
        }

        // 1. state.json: serialize fresh from the in-memory state.
        const auto snapshotPath = destDir / "state.json";
        {
            std::ofstream out{ snapshotPath,
                               std::ios::out | std::ios::trunc | std::ios::binary };
            if (!out.is_open())
            {
                throw SerializerError("dumpBugReport: failed to open " +
                                      snapshotPath.string());
            }
            const auto pretty = writePretty(toJson(state));
            out.write(pretty.data(), static_cast<std::streamsize>(pretty.size()));
            out.flush();
        }

        // 2. actions.log: copy the live log byte-for-byte. We *copy* rather
        // than reference so subsequent mutations to the live log don't
        // appear in the bundle. If the live log doesn't exist, we write
        // an empty actions.log so the bundle is always a complete
        // 2-file pair.
        const auto destLog = destDir / "actions.log";
        if (std::filesystem::exists(logPath))
        {
            std::filesystem::copy_file(logPath,
                                       destLog,
                                       std::filesystem::copy_options::overwrite_existing,
                                       ec);
            if (ec)
            {
                throw SerializerError("dumpBugReport: copy_file failed: " +
                                      ec.message());
            }
        }
        else
        {
            // Touch an empty file so callers can assume the path exists.
            std::ofstream{ destLog,
                           std::ios::out | std::ios::trunc | std::ios::binary }
                .close();
        }
    }
}
