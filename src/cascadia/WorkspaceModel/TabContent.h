// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Persistable description of what a tab "is", independent of any live
// IPaneContent instance. The reconciler / content registry uses this to
// materialise a live content object when the tab is mounted.
//
// Pure C++: no winrt::guid (we use a 16-byte array), no winrt::hstring,
// no Windows.h.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <variant>

namespace WorkspaceModel
{
    // A terminal tab targeting a particular profile.
    // `profile` is the 16-byte GUID of the profile in canonical big-endian
    // serialisation order (the same layout that winrt::guid uses in memory
    // on Windows). Storing as a byte array keeps this header winrt-free.
    struct TerminalSpec
    {
        std::array<std::uint8_t, 16> profile{};

        [[nodiscard]] friend bool operator==(const TerminalSpec& lhs,
                                             const TerminalSpec& rhs) noexcept = default;
    };

    // The single Settings page. Has no per-tab parameters.
    struct SettingsSpec
    {
        [[nodiscard]] friend bool operator==(const SettingsSpec&,
                                             const SettingsSpec&) noexcept = default;
    };

    // The Snippets browser. No per-tab parameters in this slice.
    struct SnippetsSpec
    {
        [[nodiscard]] friend bool operator==(const SnippetsSpec&,
                                             const SnippetsSpec&) noexcept = default;
    };

    // A markdown viewer rooted at a file on disk.
    struct MarkdownSpec
    {
        std::filesystem::path file;

        [[nodiscard]] friend bool operator==(const MarkdownSpec& lhs,
                                             const MarkdownSpec& rhs) noexcept
        {
            return lhs.file == rhs.file;
        }
    };

    // A throwaway scratchpad. No parameters.
    struct ScratchpadSpec
    {
        [[nodiscard]] friend bool operator==(const ScratchpadSpec&,
                                             const ScratchpadSpec&) noexcept = default;
    };

    using TabContent = std::variant<TerminalSpec, SettingsSpec, SnippetsSpec, MarkdownSpec, ScratchpadSpec>;
}
