// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "Squash.h"

#include "Serializer.h"

#include <fstream>
#include <system_error>

namespace WorkspaceModel
{
    void squash(const WorkspaceModelData& state,
                const std::filesystem::path& snapshotPath,
                const std::filesystem::path& logPath)
    {
        // Ensure parent dir exists. create_directories is a no-op when
        // the directory already exists, so this is idempotent.
        std::error_code ec;
        if (snapshotPath.has_parent_path())
        {
            std::filesystem::create_directories(snapshotPath.parent_path(), ec);
            // create_directories sets ec on real failure; we don't treat
            // "already exists" as an error (it doesn't set ec for that).
            if (ec && !std::filesystem::exists(snapshotPath.parent_path()))
            {
                throw SerializerError("squash: failed to create snapshot dir: " +
                                      ec.message());
            }
        }

        const auto tmp = std::filesystem::path{ snapshotPath }.concat(".tmp");

        // Write the snapshot to the tmp path.
        {
            std::ofstream out{ tmp, std::ios::out | std::ios::trunc | std::ios::binary };
            if (!out.is_open())
            {
                throw SerializerError("squash: failed to open tmp snapshot: " +
                                      tmp.string());
            }
            const auto pretty = writePretty(toJson(state));
            out.write(pretty.data(), static_cast<std::streamsize>(pretty.size()));
            out.flush();
            // out's destructor closes the file.
        }

        // Atomic rename over the live snapshot. std::filesystem::rename
        // is documented to replace the destination on Windows when
        // implemented via ReplaceFileW or equivalent.
        std::filesystem::rename(tmp, snapshotPath, ec);
        if (ec)
        {
            // Fall back to remove+rename. Some Windows file systems
            // reject rename-over-existing in particular conditions; this
            // is a defensive retry, not a guarantee of atomicity if it
            // fires.
            std::filesystem::remove(snapshotPath, ec);
            ec.clear();
            std::filesystem::rename(tmp, snapshotPath, ec);
            if (ec)
            {
                throw SerializerError("squash: rename failed: " + ec.message());
            }
        }

        // Now truncate the log. If the log doesn't exist, that's fine --
        // a freshly-installed app may run squash before any append().
        if (std::filesystem::exists(logPath))
        {
            std::ofstream{ logPath,
                           std::ios::out | std::ios::trunc | std::ios::binary }
                .close();
        }
    }
}
