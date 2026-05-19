// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Action records for the write-ahead log.
//
// Each action in WorkspaceActions.h has a corresponding record struct here
// that captures the exact parameters needed to replay the call. The set of
// record types is a closed std::variant `OpRecord`; this guarantees the
// log writer and the replay engine agree on the universe of actions
// without any string-matching at compile time.
//
// A LogEntry pairs an OpRecord with a monotonic sequence number and an
// ISO-8601 timestamp string. The WAL file is line-delimited JSON: one
// LogEntry per line.
//
// On-disk shape:
//   { "seq": <uint64>, "ts": "<iso8601>", "op": "newTab",
//     "params": { ...action-specific... } }
//
// The "op" discriminator strings match the function names in
// WorkspaceActions.h exactly.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <json/json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "WorkspaceActions.h"
#include "PaneTree.h"
#include "TabContent.h"

namespace WorkspaceModel
{
    // -------------------- record structs --------------------
    // Each record carries the exact set of params from its action's
    // signature in WorkspaceActions.h. Default-constructible so JSON
    // decoders can build them field-by-field; equality-comparable so tests
    // can write expected/actual asserts trivially.

    struct NewWorkspaceRecord
    {
        std::string name;
        TabContent initialTab{};
        std::string initialTabTitle;
        std::optional<Color> initialTabColor;
        bool initialTabPinned{ false };

        [[nodiscard]] friend bool operator==(const NewWorkspaceRecord&,
                                             const NewWorkspaceRecord&) noexcept = default;
    };

    struct CloseWorkspaceRecord
    {
        WorkspaceId id{};
        [[nodiscard]] friend bool operator==(const CloseWorkspaceRecord&,
                                             const CloseWorkspaceRecord&) noexcept = default;
    };

    struct CloseOtherWorkspacesRecord
    {
        WorkspaceId keep{};
        [[nodiscard]] friend bool operator==(const CloseOtherWorkspacesRecord&,
                                             const CloseOtherWorkspacesRecord&) noexcept = default;
    };

    struct CloseAllWorkspacesRecord
    {
        [[nodiscard]] friend bool operator==(const CloseAllWorkspacesRecord&,
                                             const CloseAllWorkspacesRecord&) noexcept = default;
    };

    struct SwitchToWorkspaceRecord
    {
        WorkspaceId id{};
        [[nodiscard]] friend bool operator==(const SwitchToWorkspaceRecord&,
                                             const SwitchToWorkspaceRecord&) noexcept = default;
    };

    struct RenameWorkspaceRecord
    {
        WorkspaceId id{};
        std::string name;
        [[nodiscard]] friend bool operator==(const RenameWorkspaceRecord&,
                                             const RenameWorkspaceRecord&) noexcept = default;
    };

    struct SetWorkspaceColorRecord
    {
        WorkspaceId id{};
        std::optional<Color> color;
        [[nodiscard]] friend bool operator==(const SetWorkspaceColorRecord&,
                                             const SetWorkspaceColorRecord&) noexcept = default;
    };

    struct SetWorkspaceDescriptionRecord
    {
        WorkspaceId id{};
        std::string description;
        [[nodiscard]] friend bool operator==(const SetWorkspaceDescriptionRecord&,
                                             const SetWorkspaceDescriptionRecord&) noexcept = default;
    };

    struct SetWorkspacePinnedRecord
    {
        WorkspaceId id{};
        bool pinned{ false };
        [[nodiscard]] friend bool operator==(const SetWorkspacePinnedRecord&,
                                             const SetWorkspacePinnedRecord&) noexcept = default;
    };

    struct ReorderWorkspaceRecord
    {
        WorkspaceId id{};
        std::size_t dstIdx{ 0 };
        [[nodiscard]] friend bool operator==(const ReorderWorkspaceRecord&,
                                             const ReorderWorkspaceRecord&) noexcept = default;
    };

    struct NewTabRecord
    {
        WorkspaceId workspaceId{};
        PaneId leafId{};
        TabContent description{};
        std::string customTitle;
        std::optional<Color> runtimeColor;
        bool pinned{ false };

        [[nodiscard]] friend bool operator==(const NewTabRecord&,
                                             const NewTabRecord&) noexcept = default;
    };

    struct CloseTabRecord
    {
        TabId id{};
        [[nodiscard]] friend bool operator==(const CloseTabRecord&,
                                             const CloseTabRecord&) noexcept = default;
    };

    struct CloseTabsRightRecord
    {
        TabId id{};
        [[nodiscard]] friend bool operator==(const CloseTabsRightRecord&,
                                             const CloseTabsRightRecord&) noexcept = default;
    };

    struct CloseOtherTabsRecord
    {
        TabId id{};
        [[nodiscard]] friend bool operator==(const CloseOtherTabsRecord&,
                                             const CloseOtherTabsRecord&) noexcept = default;
    };

    struct SelectTabRecord
    {
        TabId id{};
        [[nodiscard]] friend bool operator==(const SelectTabRecord&,
                                             const SelectTabRecord&) noexcept = default;
    };

    struct SetTabTitleRecord
    {
        TabId id{};
        std::string customTitle;
        [[nodiscard]] friend bool operator==(const SetTabTitleRecord&,
                                             const SetTabTitleRecord&) noexcept = default;
    };

    struct SetTabColorRecord
    {
        TabId id{};
        std::optional<Color> color;
        [[nodiscard]] friend bool operator==(const SetTabColorRecord&,
                                             const SetTabColorRecord&) noexcept = default;
    };

    struct SetTabPinnedRecord
    {
        TabId id{};
        bool pinned{ false };
        [[nodiscard]] friend bool operator==(const SetTabPinnedRecord&,
                                             const SetTabPinnedRecord&) noexcept = default;
    };

    struct SplitPaneRecord
    {
        PaneId leafId{};
        Axis axis{ Axis::Vertical };
        double ratio{ 0.5 };
        TabContent newTabDescription{};
        std::string newTabCustomTitle;
        std::optional<Color> newTabColor;
        bool newTabPinned{ false };

        [[nodiscard]] friend bool operator==(const SplitPaneRecord&,
                                             const SplitPaneRecord&) noexcept = default;
    };

    struct ClosePaneRecord
    {
        PaneId leafId{};
        [[nodiscard]] friend bool operator==(const ClosePaneRecord&,
                                             const ClosePaneRecord&) noexcept = default;
    };

    struct ResizePaneRecord
    {
        PaneId splitId{};
        double ratio{ 0.5 };
        [[nodiscard]] friend bool operator==(const ResizePaneRecord&,
                                             const ResizePaneRecord&) noexcept = default;
    };

    struct FocusPaneRecord
    {
        PaneId leafId{};
        [[nodiscard]] friend bool operator==(const FocusPaneRecord&,
                                             const FocusPaneRecord&) noexcept = default;
    };

    struct MoveTabRecord
    {
        TabId tabId{};
        PaneId dstLeafId{};
        std::size_t dstIdx{ 0 };
        [[nodiscard]] friend bool operator==(const MoveTabRecord&,
                                             const MoveTabRecord&) noexcept = default;
    };

    struct MoveTabAsSplitRecord
    {
        TabId tabId{};
        PaneId dstLeafId{};
        Edge edge{ Edge::Right };
        [[nodiscard]] friend bool operator==(const MoveTabAsSplitRecord&,
                                             const MoveTabAsSplitRecord&) noexcept = default;
    };

    struct SetSidebarWidthRecord
    {
        double width{ 0.0 };
        [[nodiscard]] friend bool operator==(const SetSidebarWidthRecord&,
                                             const SetSidebarWidthRecord&) noexcept = default;
    };

    // -------------------- OpRecord variant --------------------

    using OpRecord = std::variant<
        NewWorkspaceRecord,
        CloseWorkspaceRecord,
        CloseOtherWorkspacesRecord,
        CloseAllWorkspacesRecord,
        SwitchToWorkspaceRecord,
        RenameWorkspaceRecord,
        SetWorkspaceColorRecord,
        SetWorkspaceDescriptionRecord,
        SetWorkspacePinnedRecord,
        ReorderWorkspaceRecord,
        NewTabRecord,
        CloseTabRecord,
        CloseTabsRightRecord,
        CloseOtherTabsRecord,
        SelectTabRecord,
        SetTabTitleRecord,
        SetTabColorRecord,
        SetTabPinnedRecord,
        SplitPaneRecord,
        ClosePaneRecord,
        ResizePaneRecord,
        FocusPaneRecord,
        MoveTabRecord,
        MoveTabAsSplitRecord,
        SetSidebarWidthRecord>;

    // -------------------- LogEntry --------------------

    struct LogEntry
    {
        std::uint64_t seq{ 0 };
        OpRecord op{};
        // ISO-8601 timestamp, e.g. "2026-05-19T14:23:01.123Z". Always UTC.
        std::string timestamp;

        [[nodiscard]] friend bool operator==(const LogEntry&,
                                             const LogEntry&) noexcept = default;
    };

    // -------------------- (de)serialization --------------------

    [[nodiscard]] Json::Value opRecordToJson(const OpRecord& op);
    [[nodiscard]] OpRecord opRecordFromJson(const Json::Value& v);
    [[nodiscard]] Json::Value logEntryToJson(const LogEntry& e);
    [[nodiscard]] LogEntry logEntryFromJson(const Json::Value& v);

    // Returns "now" as an ISO-8601 UTC timestamp string. Useful when
    // constructing a LogEntry from a freshly-appended OpRecord.
    [[nodiscard]] std::string nowIso8601();
}
