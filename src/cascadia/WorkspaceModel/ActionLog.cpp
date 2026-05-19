// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ActionLog.h"

#include "Serializer.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace WorkspaceModel
{
    namespace
    {
        // Discriminator strings on the wire. Match the function names in
        // WorkspaceActions.h exactly.
        constexpr const char* kOpNewWorkspace = "newWorkspace";
        constexpr const char* kOpCloseWorkspace = "closeWorkspace";
        constexpr const char* kOpCloseOtherWorkspaces = "closeOtherWorkspaces";
        constexpr const char* kOpCloseAllWorkspaces = "closeAllWorkspaces";
        constexpr const char* kOpSwitchToWorkspace = "switchToWorkspace";
        constexpr const char* kOpRenameWorkspace = "renameWorkspace";
        constexpr const char* kOpSetWorkspaceColor = "setWorkspaceColor";
        constexpr const char* kOpSetWorkspaceDescription = "setWorkspaceDescription";
        constexpr const char* kOpSetWorkspacePinned = "setWorkspacePinned";
        constexpr const char* kOpReorderWorkspace = "reorderWorkspace";
        constexpr const char* kOpNewTab = "newTab";
        constexpr const char* kOpCloseTab = "closeTab";
        constexpr const char* kOpCloseTabsRight = "closeTabsRight";
        constexpr const char* kOpCloseOtherTabs = "closeOtherTabs";
        constexpr const char* kOpSelectTab = "selectTab";
        constexpr const char* kOpSetTabTitle = "setTabTitle";
        constexpr const char* kOpSetTabColor = "setTabColor";
        constexpr const char* kOpSetTabPinned = "setTabPinned";
        constexpr const char* kOpSplitPane = "splitPane";
        constexpr const char* kOpClosePane = "closePane";
        constexpr const char* kOpResizePane = "resizePane";
        constexpr const char* kOpFocusPane = "focusPane";
        constexpr const char* kOpMoveTab = "moveTab";
        constexpr const char* kOpMoveTabAsSplit = "moveTabAsSplit";
        constexpr const char* kOpSetSidebarWidth = "setSidebarWidth";

        constexpr const char* kFOp = "op";
        constexpr const char* kFParams = "params";
        constexpr const char* kFSeq = "seq";
        constexpr const char* kFTs = "ts";

        // ID helpers
        Json::Value idToJson(std::uint64_t v) { return Json::Value{ static_cast<Json::UInt64>(v) }; }

        WorkspaceId workspaceIdFromJson(const Json::Value& obj, const char* key)
        {
            return WorkspaceId{ detail::requireUInt64(obj, key) };
        }
        PaneId paneIdFromJson(const Json::Value& obj, const char* key)
        {
            return PaneId{ detail::requireUInt64(obj, key) };
        }
        TabId tabIdFromJson(const Json::Value& obj, const char* key)
        {
            return TabId{ detail::requireUInt64(obj, key) };
        }

        // Axis
        Json::Value axisToJson(Axis a)
        {
            return Json::Value{ a == Axis::Horizontal ? "horizontal" : "vertical" };
        }
        Axis axisFromJson(const Json::Value& obj, const char* key)
        {
            const auto s = detail::requireString(obj, key);
            if (s == "horizontal")
            {
                return Axis::Horizontal;
            }
            if (s == "vertical")
            {
                return Axis::Vertical;
            }
            detail::fail("unknown axis: " + s);
        }

        // Edge
        Json::Value edgeToJson(Edge e)
        {
            switch (e)
            {
            case Edge::Left: return Json::Value{ "left" };
            case Edge::Right: return Json::Value{ "right" };
            case Edge::Top: return Json::Value{ "top" };
            case Edge::Bottom: return Json::Value{ "bottom" };
            }
            return Json::Value{ "right" };
        }
        Edge edgeFromJson(const Json::Value& obj, const char* key)
        {
            const auto s = detail::requireString(obj, key);
            if (s == "left")
            {
                return Edge::Left;
            }
            if (s == "right")
            {
                return Edge::Right;
            }
            if (s == "top")
            {
                return Edge::Top;
            }
            if (s == "bottom")
            {
                return Edge::Bottom;
            }
            detail::fail("unknown edge: " + s);
        }

        // ---------- per-record encoders ----------

        Json::Value paramsFor(const NewWorkspaceRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["name"] = r.name;
            p["initialTab"] = detail::tabContentToJson(r.initialTab);
            p["initialTabTitle"] = r.initialTabTitle;
            p["initialTabColor"] = detail::optColorToJson(r.initialTabColor);
            p["initialTabPinned"] = r.initialTabPinned;
            return p;
        }
        NewWorkspaceRecord decodeNewWorkspace(const Json::Value& p)
        {
            NewWorkspaceRecord r;
            r.name = detail::requireString(p, "name");
            r.initialTab = detail::tabContentFromJson(detail::require(p, "initialTab"));
            r.initialTabTitle = detail::requireString(p, "initialTabTitle");
            r.initialTabColor = detail::optColorFromJson(detail::require(p, "initialTabColor"));
            r.initialTabPinned = detail::requireBool(p, "initialTabPinned");
            return r;
        }

        Json::Value paramsFor(const CloseWorkspaceRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            return p;
        }
        CloseWorkspaceRecord decodeCloseWorkspace(const Json::Value& p)
        {
            return CloseWorkspaceRecord{ workspaceIdFromJson(p, "id") };
        }

        Json::Value paramsFor(const CloseOtherWorkspacesRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["keep"] = idToJson(r.keep.v);
            return p;
        }
        CloseOtherWorkspacesRecord decodeCloseOtherWorkspaces(const Json::Value& p)
        {
            return CloseOtherWorkspacesRecord{ workspaceIdFromJson(p, "keep") };
        }

        Json::Value paramsFor(const CloseAllWorkspacesRecord&)
        {
            return Json::Value{ Json::objectValue };
        }
        CloseAllWorkspacesRecord decodeCloseAllWorkspaces(const Json::Value&)
        {
            return CloseAllWorkspacesRecord{};
        }

        Json::Value paramsFor(const SwitchToWorkspaceRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            return p;
        }
        SwitchToWorkspaceRecord decodeSwitchToWorkspace(const Json::Value& p)
        {
            return SwitchToWorkspaceRecord{ workspaceIdFromJson(p, "id") };
        }

        Json::Value paramsFor(const RenameWorkspaceRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["name"] = r.name;
            return p;
        }
        RenameWorkspaceRecord decodeRenameWorkspace(const Json::Value& p)
        {
            RenameWorkspaceRecord r;
            r.id = workspaceIdFromJson(p, "id");
            r.name = detail::requireString(p, "name");
            return r;
        }

        Json::Value paramsFor(const SetWorkspaceColorRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["color"] = detail::optColorToJson(r.color);
            return p;
        }
        SetWorkspaceColorRecord decodeSetWorkspaceColor(const Json::Value& p)
        {
            SetWorkspaceColorRecord r;
            r.id = workspaceIdFromJson(p, "id");
            r.color = detail::optColorFromJson(detail::require(p, "color"));
            return r;
        }

        Json::Value paramsFor(const SetWorkspaceDescriptionRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["description"] = r.description;
            return p;
        }
        SetWorkspaceDescriptionRecord decodeSetWorkspaceDescription(const Json::Value& p)
        {
            SetWorkspaceDescriptionRecord r;
            r.id = workspaceIdFromJson(p, "id");
            r.description = detail::requireString(p, "description");
            return r;
        }

        Json::Value paramsFor(const SetWorkspacePinnedRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["pinned"] = r.pinned;
            return p;
        }
        SetWorkspacePinnedRecord decodeSetWorkspacePinned(const Json::Value& p)
        {
            SetWorkspacePinnedRecord r;
            r.id = workspaceIdFromJson(p, "id");
            r.pinned = detail::requireBool(p, "pinned");
            return r;
        }

        Json::Value paramsFor(const ReorderWorkspaceRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["dstIdx"] = static_cast<Json::UInt64>(r.dstIdx);
            return p;
        }
        ReorderWorkspaceRecord decodeReorderWorkspace(const Json::Value& p)
        {
            ReorderWorkspaceRecord r;
            r.id = workspaceIdFromJson(p, "id");
            r.dstIdx = static_cast<std::size_t>(detail::requireUInt64(p, "dstIdx"));
            return r;
        }

        Json::Value paramsFor(const NewTabRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["workspaceId"] = idToJson(r.workspaceId.v);
            p["leafId"] = idToJson(r.leafId.v);
            p["description"] = detail::tabContentToJson(r.description);
            p["customTitle"] = r.customTitle;
            p["runtimeColor"] = detail::optColorToJson(r.runtimeColor);
            p["pinned"] = r.pinned;
            return p;
        }
        NewTabRecord decodeNewTab(const Json::Value& p)
        {
            NewTabRecord r;
            r.workspaceId = workspaceIdFromJson(p, "workspaceId");
            r.leafId = paneIdFromJson(p, "leafId");
            r.description = detail::tabContentFromJson(detail::require(p, "description"));
            r.customTitle = detail::requireString(p, "customTitle");
            r.runtimeColor = detail::optColorFromJson(detail::require(p, "runtimeColor"));
            r.pinned = detail::requireBool(p, "pinned");
            return r;
        }

        Json::Value paramsFor(const CloseTabRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            return p;
        }
        CloseTabRecord decodeCloseTab(const Json::Value& p)
        {
            return CloseTabRecord{ tabIdFromJson(p, "id") };
        }

        Json::Value paramsFor(const CloseTabsRightRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            return p;
        }
        CloseTabsRightRecord decodeCloseTabsRight(const Json::Value& p)
        {
            return CloseTabsRightRecord{ tabIdFromJson(p, "id") };
        }

        Json::Value paramsFor(const CloseOtherTabsRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            return p;
        }
        CloseOtherTabsRecord decodeCloseOtherTabs(const Json::Value& p)
        {
            return CloseOtherTabsRecord{ tabIdFromJson(p, "id") };
        }

        Json::Value paramsFor(const SelectTabRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            return p;
        }
        SelectTabRecord decodeSelectTab(const Json::Value& p)
        {
            return SelectTabRecord{ tabIdFromJson(p, "id") };
        }

        Json::Value paramsFor(const SetTabTitleRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["customTitle"] = r.customTitle;
            return p;
        }
        SetTabTitleRecord decodeSetTabTitle(const Json::Value& p)
        {
            SetTabTitleRecord r;
            r.id = tabIdFromJson(p, "id");
            r.customTitle = detail::requireString(p, "customTitle");
            return r;
        }

        Json::Value paramsFor(const SetTabColorRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["color"] = detail::optColorToJson(r.color);
            return p;
        }
        SetTabColorRecord decodeSetTabColor(const Json::Value& p)
        {
            SetTabColorRecord r;
            r.id = tabIdFromJson(p, "id");
            r.color = detail::optColorFromJson(detail::require(p, "color"));
            return r;
        }

        Json::Value paramsFor(const SetTabPinnedRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["id"] = idToJson(r.id.v);
            p["pinned"] = r.pinned;
            return p;
        }
        SetTabPinnedRecord decodeSetTabPinned(const Json::Value& p)
        {
            SetTabPinnedRecord r;
            r.id = tabIdFromJson(p, "id");
            r.pinned = detail::requireBool(p, "pinned");
            return r;
        }

        Json::Value paramsFor(const SplitPaneRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["leafId"] = idToJson(r.leafId.v);
            p["axis"] = axisToJson(r.axis);
            p["ratio"] = r.ratio;
            p["newTabDescription"] = detail::tabContentToJson(r.newTabDescription);
            p["newTabCustomTitle"] = r.newTabCustomTitle;
            p["newTabColor"] = detail::optColorToJson(r.newTabColor);
            p["newTabPinned"] = r.newTabPinned;
            return p;
        }
        SplitPaneRecord decodeSplitPane(const Json::Value& p)
        {
            SplitPaneRecord r;
            r.leafId = paneIdFromJson(p, "leafId");
            r.axis = axisFromJson(p, "axis");
            r.ratio = detail::requireDouble(p, "ratio");
            r.newTabDescription = detail::tabContentFromJson(detail::require(p, "newTabDescription"));
            r.newTabCustomTitle = detail::requireString(p, "newTabCustomTitle");
            r.newTabColor = detail::optColorFromJson(detail::require(p, "newTabColor"));
            r.newTabPinned = detail::requireBool(p, "newTabPinned");
            return r;
        }

        Json::Value paramsFor(const ClosePaneRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["leafId"] = idToJson(r.leafId.v);
            return p;
        }
        ClosePaneRecord decodeClosePane(const Json::Value& p)
        {
            return ClosePaneRecord{ paneIdFromJson(p, "leafId") };
        }

        Json::Value paramsFor(const ResizePaneRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["splitId"] = idToJson(r.splitId.v);
            p["ratio"] = r.ratio;
            return p;
        }
        ResizePaneRecord decodeResizePane(const Json::Value& p)
        {
            ResizePaneRecord r;
            r.splitId = paneIdFromJson(p, "splitId");
            r.ratio = detail::requireDouble(p, "ratio");
            return r;
        }

        Json::Value paramsFor(const FocusPaneRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["leafId"] = idToJson(r.leafId.v);
            return p;
        }
        FocusPaneRecord decodeFocusPane(const Json::Value& p)
        {
            return FocusPaneRecord{ paneIdFromJson(p, "leafId") };
        }

        Json::Value paramsFor(const MoveTabRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["tabId"] = idToJson(r.tabId.v);
            p["dstLeafId"] = idToJson(r.dstLeafId.v);
            p["dstIdx"] = static_cast<Json::UInt64>(r.dstIdx);
            return p;
        }
        MoveTabRecord decodeMoveTab(const Json::Value& p)
        {
            MoveTabRecord r;
            r.tabId = tabIdFromJson(p, "tabId");
            r.dstLeafId = paneIdFromJson(p, "dstLeafId");
            r.dstIdx = static_cast<std::size_t>(detail::requireUInt64(p, "dstIdx"));
            return r;
        }

        Json::Value paramsFor(const MoveTabAsSplitRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["tabId"] = idToJson(r.tabId.v);
            p["dstLeafId"] = idToJson(r.dstLeafId.v);
            p["edge"] = edgeToJson(r.edge);
            return p;
        }
        MoveTabAsSplitRecord decodeMoveTabAsSplit(const Json::Value& p)
        {
            MoveTabAsSplitRecord r;
            r.tabId = tabIdFromJson(p, "tabId");
            r.dstLeafId = paneIdFromJson(p, "dstLeafId");
            r.edge = edgeFromJson(p, "edge");
            return r;
        }

        Json::Value paramsFor(const SetSidebarWidthRecord& r)
        {
            Json::Value p{ Json::objectValue };
            p["width"] = r.width;
            return p;
        }
        SetSidebarWidthRecord decodeSetSidebarWidth(const Json::Value& p)
        {
            SetSidebarWidthRecord r;
            r.width = detail::requireDouble(p, "width");
            return r;
        }

        // ---------- variant visitor: name + params ----------

        struct OpDescriptor
        {
            const char* name;
            Json::Value params;
        };

        OpDescriptor describe(const OpRecord& op)
        {
            return std::visit([](const auto& rec) -> OpDescriptor {
                using T = std::decay_t<decltype(rec)>;
                if constexpr (std::is_same_v<T, NewWorkspaceRecord>)
                    return { kOpNewWorkspace, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, CloseWorkspaceRecord>)
                    return { kOpCloseWorkspace, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, CloseOtherWorkspacesRecord>)
                    return { kOpCloseOtherWorkspaces, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, CloseAllWorkspacesRecord>)
                    return { kOpCloseAllWorkspaces, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SwitchToWorkspaceRecord>)
                    return { kOpSwitchToWorkspace, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, RenameWorkspaceRecord>)
                    return { kOpRenameWorkspace, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetWorkspaceColorRecord>)
                    return { kOpSetWorkspaceColor, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetWorkspaceDescriptionRecord>)
                    return { kOpSetWorkspaceDescription, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetWorkspacePinnedRecord>)
                    return { kOpSetWorkspacePinned, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, ReorderWorkspaceRecord>)
                    return { kOpReorderWorkspace, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, NewTabRecord>)
                    return { kOpNewTab, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, CloseTabRecord>)
                    return { kOpCloseTab, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, CloseTabsRightRecord>)
                    return { kOpCloseTabsRight, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, CloseOtherTabsRecord>)
                    return { kOpCloseOtherTabs, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SelectTabRecord>)
                    return { kOpSelectTab, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetTabTitleRecord>)
                    return { kOpSetTabTitle, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetTabColorRecord>)
                    return { kOpSetTabColor, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetTabPinnedRecord>)
                    return { kOpSetTabPinned, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SplitPaneRecord>)
                    return { kOpSplitPane, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, ClosePaneRecord>)
                    return { kOpClosePane, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, ResizePaneRecord>)
                    return { kOpResizePane, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, FocusPaneRecord>)
                    return { kOpFocusPane, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, MoveTabRecord>)
                    return { kOpMoveTab, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, MoveTabAsSplitRecord>)
                    return { kOpMoveTabAsSplit, paramsFor(rec) };
                else if constexpr (std::is_same_v<T, SetSidebarWidthRecord>)
                    return { kOpSetSidebarWidth, paramsFor(rec) };
                else
                {
                    // Exhaustive over OpRecord variants. If a new arm is
                    // added without a case above, this static_assert fires
                    // at compile time so the omission is caught immediately.
                    static_assert(!std::is_same_v<T, T>, "describe() missing an OpRecord variant arm");
                    return OpDescriptor{};
                }
            },
                              op);
        }

        // Dispatch from the on-disk discriminator string to the matching
        // record decoder.
        OpRecord decodeOp(const std::string& name, const Json::Value& params)
        {
            if (name == kOpNewWorkspace) return OpRecord{ decodeNewWorkspace(params) };
            if (name == kOpCloseWorkspace) return OpRecord{ decodeCloseWorkspace(params) };
            if (name == kOpCloseOtherWorkspaces) return OpRecord{ decodeCloseOtherWorkspaces(params) };
            if (name == kOpCloseAllWorkspaces) return OpRecord{ decodeCloseAllWorkspaces(params) };
            if (name == kOpSwitchToWorkspace) return OpRecord{ decodeSwitchToWorkspace(params) };
            if (name == kOpRenameWorkspace) return OpRecord{ decodeRenameWorkspace(params) };
            if (name == kOpSetWorkspaceColor) return OpRecord{ decodeSetWorkspaceColor(params) };
            if (name == kOpSetWorkspaceDescription) return OpRecord{ decodeSetWorkspaceDescription(params) };
            if (name == kOpSetWorkspacePinned) return OpRecord{ decodeSetWorkspacePinned(params) };
            if (name == kOpReorderWorkspace) return OpRecord{ decodeReorderWorkspace(params) };
            if (name == kOpNewTab) return OpRecord{ decodeNewTab(params) };
            if (name == kOpCloseTab) return OpRecord{ decodeCloseTab(params) };
            if (name == kOpCloseTabsRight) return OpRecord{ decodeCloseTabsRight(params) };
            if (name == kOpCloseOtherTabs) return OpRecord{ decodeCloseOtherTabs(params) };
            if (name == kOpSelectTab) return OpRecord{ decodeSelectTab(params) };
            if (name == kOpSetTabTitle) return OpRecord{ decodeSetTabTitle(params) };
            if (name == kOpSetTabColor) return OpRecord{ decodeSetTabColor(params) };
            if (name == kOpSetTabPinned) return OpRecord{ decodeSetTabPinned(params) };
            if (name == kOpSplitPane) return OpRecord{ decodeSplitPane(params) };
            if (name == kOpClosePane) return OpRecord{ decodeClosePane(params) };
            if (name == kOpResizePane) return OpRecord{ decodeResizePane(params) };
            if (name == kOpFocusPane) return OpRecord{ decodeFocusPane(params) };
            if (name == kOpMoveTab) return OpRecord{ decodeMoveTab(params) };
            if (name == kOpMoveTabAsSplit) return OpRecord{ decodeMoveTabAsSplit(params) };
            if (name == kOpSetSidebarWidth) return OpRecord{ decodeSetSidebarWidth(params) };
            detail::fail("unknown op name in log: " + name);
        }
    } // namespace

    Json::Value opRecordToJson(const OpRecord& op)
    {
        // Outer envelope is just the discriminator + params object. The
        // logEntryToJson wrapper adds seq + ts on top.
        const auto d = describe(op);
        Json::Value out{ Json::objectValue };
        out[kFOp] = d.name;
        out[kFParams] = d.params;
        return out;
    }

    OpRecord opRecordFromJson(const Json::Value& v)
    {
        if (!v.isObject())
        {
            detail::fail("op record must be a JSON object");
        }
        const auto name = detail::requireString(v, kFOp);
        const auto& params = detail::require(v, kFParams);
        if (!params.isObject())
        {
            detail::fail("op params must be a JSON object");
        }
        return decodeOp(name, params);
    }

    Json::Value logEntryToJson(const LogEntry& e)
    {
        Json::Value out{ Json::objectValue };
        out[kFSeq] = static_cast<Json::UInt64>(e.seq);
        out[kFTs] = e.timestamp;
        const auto d = describe(e.op);
        out[kFOp] = d.name;
        out[kFParams] = d.params;
        return out;
    }

    LogEntry logEntryFromJson(const Json::Value& v)
    {
        if (!v.isObject())
        {
            detail::fail("log entry must be a JSON object");
        }
        LogEntry e;
        e.seq = detail::requireUInt64(v, kFSeq);
        e.timestamp = detail::requireString(v, kFTs);
        const auto name = detail::requireString(v, kFOp);
        const auto& params = detail::require(v, kFParams);
        if (!params.isObject())
        {
            detail::fail("op params must be a JSON object");
        }
        e.op = decodeOp(name, params);
        return e;
    }

    std::string nowIso8601()
    {
        // UTC ISO-8601 with millisecond precision: "2026-05-19T14:23:01.123Z".
        // Uses std::chrono::system_clock + gmtime_s for cross-host
        // portability without pulling in fmt or std::format-with-tz.
        using namespace std::chrono;
        const auto tp = system_clock::now();
        const auto secs = time_point_cast<seconds>(tp);
        const auto ms = duration_cast<milliseconds>(tp - secs).count();

        const std::time_t tt = system_clock::to_time_t(tp);
        std::tm tm_buf{};
#if defined(_WIN32)
        ::gmtime_s(&tm_buf, &tt);
#else
        ::gmtime_r(&tt, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
        oss << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
        return oss.str();
    }
}
