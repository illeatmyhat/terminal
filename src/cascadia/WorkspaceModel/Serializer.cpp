// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "Serializer.h"

#include "PaneTree.h"
#include "TabContent.h"
#include "WorkspaceState.h"

#include <cctype>
#include <sstream>

namespace WorkspaceModel
{
    // The bulk of the encoding logic lives in WorkspaceModel::detail so it
    // can be shared with ActionLog.cpp (op-record encoding) without a
    // separate internal header. Only the public toJson/fromJson entry
    // points sit in the outer namespace.
    namespace detail
    {
        // Field name constants. Keeping them in one place avoids typos and
        // makes the on-disk shape easy to audit at a glance.
        constexpr const char* kSchemaVersionKey = "schemaVersion";
        constexpr const char* kIdCounter = "idCounter";
        constexpr const char* kSidebarWidth = "sidebarWidth";
        constexpr const char* kActiveWorkspaceId = "activeWorkspaceId";
        constexpr const char* kMru = "mru";
        constexpr const char* kWorkspaces = "workspaces";

        constexpr const char* kId = "id";
        constexpr const char* kName = "name";
        constexpr const char* kColor = "color";
        constexpr const char* kCustomDescription = "customDescription";
        constexpr const char* kPinned = "pinned";
        constexpr const char* kActivePaneId = "activePaneId";
        constexpr const char* kRoot = "root";

        constexpr const char* kKind = "kind";
        constexpr const char* kAxis = "axis";
        constexpr const char* kRatio = "ratio";
        constexpr const char* kLeft = "left";
        constexpr const char* kRight = "right";
        constexpr const char* kActiveTabIdx = "activeTabIdx";
        constexpr const char* kTabs = "tabs";

        constexpr const char* kDescription = "description";
        constexpr const char* kCustomTitle = "customTitle";
        constexpr const char* kRuntimeColor = "runtimeColor";
        constexpr const char* kMount = "mount";

        constexpr const char* kKindSplit = "split";
        constexpr const char* kKindLeaf = "leaf";

        constexpr const char* kKindTerminal = "terminal";
        constexpr const char* kKindSettings = "settings";
        constexpr const char* kKindSnippets = "snippets";
        constexpr const char* kKindMarkdown = "markdown";
        constexpr const char* kKindScratchpad = "scratchpad";

        constexpr const char* kAxisHorizontal = "horizontal";
        constexpr const char* kAxisVertical = "vertical";

        constexpr const char* kProfile = "profile";
        constexpr const char* kFile = "file";

        // ---------- small helpers ----------

        [[noreturn]] void fail(const std::string& msg)
        {
            throw SerializerError(msg);
        }

        const Json::Value& require(const Json::Value& obj, const char* key)
        {
            if (!obj.isObject() || !obj.isMember(key))
            {
                fail(std::string{ "missing required field: " } + key);
            }
            return obj[key];
        }

        std::string requireString(const Json::Value& obj, const char* key)
        {
            const auto& v = require(obj, key);
            if (!v.isString())
            {
                fail(std::string{ "expected string for field: " } + key);
            }
            return v.asString();
        }

        bool requireBool(const Json::Value& obj, const char* key)
        {
            const auto& v = require(obj, key);
            if (!v.isBool())
            {
                fail(std::string{ "expected bool for field: " } + key);
            }
            return v.asBool();
        }

        std::uint64_t requireUInt64(const Json::Value& obj, const char* key)
        {
            const auto& v = require(obj, key);
            if (v.isUInt64())
            {
                return v.asUInt64();
            }
            if (v.isUInt())
            {
                return v.asUInt();
            }
            if (v.isInt() && v.asInt() >= 0)
            {
                return static_cast<std::uint64_t>(v.asInt());
            }
            fail(std::string{ "expected uint64 for field: " } + key);
        }

        double requireDouble(const Json::Value& obj, const char* key)
        {
            const auto& v = require(obj, key);
            if (!v.isNumeric())
            {
                fail(std::string{ "expected number for field: " } + key);
            }
            return v.asDouble();
        }

        std::size_t requireUInt(const Json::Value& obj, const char* key)
        {
            return static_cast<std::size_t>(requireUInt64(obj, key));
        }

        // ---------- Color ----------

        // Encodes a Color as "#rrggbbaa" hex. Eight hex digits (lowercase).
        // The spec calls out this encoding explicitly for both workspace and
        // tab color fields. nullopt is represented as JSON null at the call
        // site.
        std::string colorToHex(const Color& c)
        {
            char buf[10]{};
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                          static_cast<unsigned>(c.r),
                          static_cast<unsigned>(c.g),
                          static_cast<unsigned>(c.b),
                          static_cast<unsigned>(c.a));
            return std::string{ buf };
        }

        std::uint8_t hexDigit(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return static_cast<std::uint8_t>(c - '0');
            }
            const auto lo = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lo >= 'a' && lo <= 'f')
            {
                return static_cast<std::uint8_t>(10 + (lo - 'a'));
            }
            fail(std::string{ "invalid hex digit in color: " } + c);
        }

        Color hexToColor(const std::string& s)
        {
            if (s.size() != 9 || s[0] != '#')
            {
                fail("color must be #rrggbbaa (9 chars including #), got: " + s);
            }
            Color c;
            c.r = static_cast<std::uint8_t>((hexDigit(s[1]) << 4) | hexDigit(s[2]));
            c.g = static_cast<std::uint8_t>((hexDigit(s[3]) << 4) | hexDigit(s[4]));
            c.b = static_cast<std::uint8_t>((hexDigit(s[5]) << 4) | hexDigit(s[6]));
            c.a = static_cast<std::uint8_t>((hexDigit(s[7]) << 4) | hexDigit(s[8]));
            return c;
        }

        Json::Value optColorToJson(const std::optional<Color>& c)
        {
            if (!c.has_value())
            {
                return Json::Value{ Json::nullValue };
            }
            return Json::Value{ colorToHex(*c) };
        }

        std::optional<Color> optColorFromJson(const Json::Value& v)
        {
            if (v.isNull())
            {
                return std::nullopt;
            }
            if (!v.isString())
            {
                fail("color field must be null or hex string");
            }
            return hexToColor(v.asString());
        }

        // ---------- Profile guid (16 bytes) ----------

        std::string profileToHex(const std::array<std::uint8_t, 16>& p)
        {
            // 32 hex chars, lowercase. No dashes -- simpler than RFC4122 and
            // round-trip-friendly. The model treats this as opaque bytes, so
            // the canonical form here is just hex.
            std::string out(32, '0');
            static constexpr char digits[] = "0123456789abcdef";
            for (std::size_t i = 0; i < 16; ++i)
            {
                out[2 * i] = digits[(p[i] >> 4) & 0xF];
                out[2 * i + 1] = digits[p[i] & 0xF];
            }
            return out;
        }

        std::array<std::uint8_t, 16> profileFromHex(const std::string& s)
        {
            if (s.size() != 32)
            {
                fail("profile must be 32 hex chars, got length " + std::to_string(s.size()));
            }
            std::array<std::uint8_t, 16> out{};
            for (std::size_t i = 0; i < 16; ++i)
            {
                out[i] = static_cast<std::uint8_t>((hexDigit(s[2 * i]) << 4) | hexDigit(s[2 * i + 1]));
            }
            return out;
        }

        // ---------- TabContent variant ----------

        Json::Value tabContentToJson(const TabContent& tc)
        {
            Json::Value obj{ Json::objectValue };
            std::visit([&](const auto& spec) {
                using T = std::decay_t<decltype(spec)>;
                if constexpr (std::is_same_v<T, TerminalSpec>)
                {
                    obj[kKind] = kKindTerminal;
                    obj[kProfile] = profileToHex(spec.profile);
                }
                else if constexpr (std::is_same_v<T, SettingsSpec>)
                {
                    obj[kKind] = kKindSettings;
                }
                else if constexpr (std::is_same_v<T, SnippetsSpec>)
                {
                    obj[kKind] = kKindSnippets;
                }
                else if constexpr (std::is_same_v<T, MarkdownSpec>)
                {
                    obj[kKind] = kKindMarkdown;
                    // filesystem::path -> UTF-8 string. u8string() guarantees
                    // UTF-8 regardless of host narrow encoding; we then copy
                    // those bytes into a std::string for JsonCpp.
                    const auto u8 = spec.file.u8string();
                    obj[kFile] = std::string{ reinterpret_cast<const char*>(u8.data()), u8.size() };
                }
                else if constexpr (std::is_same_v<T, ScratchpadSpec>)
                {
                    obj[kKind] = kKindScratchpad;
                }
            },
                       tc);
            return obj;
        }

        TabContent tabContentFromJson(const Json::Value& v)
        {
            if (!v.isObject())
            {
                fail("tab description must be an object");
            }
            const auto kind = requireString(v, kKind);
            if (kind == kKindTerminal)
            {
                TerminalSpec ts;
                ts.profile = profileFromHex(requireString(v, kProfile));
                return ts;
            }
            if (kind == kKindSettings)
            {
                return SettingsSpec{};
            }
            if (kind == kKindSnippets)
            {
                return SnippetsSpec{};
            }
            if (kind == kKindMarkdown)
            {
                MarkdownSpec ms;
                const auto fileStr = requireString(v, kFile);
                // We persisted u8 bytes; reconstruct as filesystem::path from
                // a u8string_view so non-ASCII path components survive the
                // round trip. std::filesystem::u8path is deprecated in C++20;
                // the path(u8string) ctor is the modern equivalent.
                const std::u8string u8(reinterpret_cast<const char8_t*>(fileStr.data()), fileStr.size());
                ms.file = std::filesystem::path(u8);
                return ms;
            }
            if (kind == kKindScratchpad)
            {
                return ScratchpadSpec{};
            }
            fail("unknown tab content kind: " + kind);
        }

        // ---------- TabRecord ----------

        Json::Value tabRecordToJson(const TabRecord& t)
        {
            Json::Value o{ Json::objectValue };
            o[kId] = static_cast<Json::UInt64>(t.id.v);
            o[kDescription] = tabContentToJson(t.description);
            o[kCustomTitle] = t.customTitle;
            o[kRuntimeColor] = optColorToJson(t.runtimeColor);
            o[kPinned] = t.pinned;
            // mount is runtime-only and always serializes as null. The
            // deserializer ignores its value entirely.
            o[kMount] = Json::Value{ Json::nullValue };
            return o;
        }

        TabRecord tabRecordFromJson(const Json::Value& v)
        {
            if (!v.isObject())
            {
                fail("tab record must be an object");
            }
            TabRecord t;
            t.id = TabId{ requireUInt64(v, kId) };
            t.description = tabContentFromJson(require(v, kDescription));
            t.customTitle = requireString(v, kCustomTitle);
            t.runtimeColor = optColorFromJson(require(v, kRuntimeColor));
            t.pinned = requireBool(v, kPinned);
            // mount: intentionally dropped. The on-disk value is always
            // null; even if a hand-edited file supplied a non-null mount,
            // we'd ignore it to enforce the runtime-only invariant.
            t.mount = std::nullopt;
            return t;
        }

        // ---------- PaneNode (split | leaf) ----------

        Json::Value paneNodeToJson(const PaneNode& node);

        Json::Value splitPaneToJson(const SplitPane& s)
        {
            Json::Value o{ Json::objectValue };
            o[kKind] = kKindSplit;
            o[kId] = static_cast<Json::UInt64>(s.id.v);
            o[kAxis] = (s.axis == Axis::Horizontal ? kAxisHorizontal : kAxisVertical);
            o[kRatio] = s.ratio;
            // Invariant 2 guarantees both children are non-null in a
            // well-formed model. Defense-in-depth: if a malformed in-memory
            // state somehow reaches the serializer, emit nulls -- the
            // deserializer will reject those, producing a clear error rather
            // than a silent crash.
            o[kLeft] = s.left ? paneNodeToJson(*s.left) : Json::Value{ Json::nullValue };
            o[kRight] = s.right ? paneNodeToJson(*s.right) : Json::Value{ Json::nullValue };
            return o;
        }

        Json::Value leafPaneToJson(const LeafPane& l)
        {
            Json::Value o{ Json::objectValue };
            o[kKind] = kKindLeaf;
            o[kId] = static_cast<Json::UInt64>(l.id.v);
            o[kActiveTabIdx] = static_cast<Json::UInt64>(l.activeTabIdx);
            Json::Value arr{ Json::arrayValue };
            for (const auto& t : l.tabs)
            {
                arr.append(tabRecordToJson(t));
            }
            o[kTabs] = std::move(arr);
            return o;
        }

        Json::Value paneNodeToJson(const PaneNode& node)
        {
            return std::visit([](const auto& inner) -> Json::Value {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, LeafPane>)
                {
                    return leafPaneToJson(inner);
                }
                else
                {
                    return splitPaneToJson(inner);
                }
            },
                              node);
        }

        PaneNode paneNodeFromJson(const Json::Value& v);

        LeafPane leafFromJson(const Json::Value& v)
        {
            LeafPane l;
            l.id = PaneId{ requireUInt64(v, kId) };
            l.activeTabIdx = requireUInt(v, kActiveTabIdx);
            const auto& tabsArr = require(v, kTabs);
            if (!tabsArr.isArray())
            {
                fail("leaf.tabs must be an array");
            }
            // Defense-in-depth: the model invariants require at least one
            // tab per leaf, but a hand-edited or corrupted file might
            // violate that. We accept the empty array on deserialize (so
            // we can faithfully read whatever was written), but the
            // validator -- if the caller chooses to run it -- will reject
            // it. This avoids the deserializer making policy decisions
            // about what to do with an "impossible" leaf.
            l.tabs.reserve(tabsArr.size());
            for (const auto& t : tabsArr)
            {
                l.tabs.push_back(tabRecordFromJson(t));
            }
            return l;
        }

        SplitPane splitFromJson(const Json::Value& v)
        {
            SplitPane s;
            s.id = PaneId{ requireUInt64(v, kId) };
            const auto axisStr = requireString(v, kAxis);
            if (axisStr == kAxisHorizontal)
            {
                s.axis = Axis::Horizontal;
            }
            else if (axisStr == kAxisVertical)
            {
                s.axis = Axis::Vertical;
            }
            else
            {
                fail("unknown axis: " + axisStr);
            }
            s.ratio = requireDouble(v, kRatio);
            const auto& leftV = require(v, kLeft);
            const auto& rightV = require(v, kRight);
            if (leftV.isNull() || rightV.isNull())
            {
                fail("split children must not be null");
            }
            s.left = std::make_shared<const PaneNode>(paneNodeFromJson(leftV));
            s.right = std::make_shared<const PaneNode>(paneNodeFromJson(rightV));
            return s;
        }

        PaneNode paneNodeFromJson(const Json::Value& v)
        {
            if (!v.isObject())
            {
                fail("pane node must be an object");
            }
            const auto kind = requireString(v, kKind);
            if (kind == kKindLeaf)
            {
                return PaneNode{ leafFromJson(v) };
            }
            if (kind == kKindSplit)
            {
                return PaneNode{ splitFromJson(v) };
            }
            fail("unknown pane kind: " + kind);
        }

        // ---------- WorkspaceState ----------

        Json::Value workspaceToJson(const WorkspaceState& w)
        {
            Json::Value o{ Json::objectValue };
            o[kId] = static_cast<Json::UInt64>(w.id.v);
            o[kName] = w.name;
            o[kColor] = optColorToJson(w.color);
            o[kCustomDescription] = w.customDescription;
            o[kPinned] = w.pinned;
            o[kActivePaneId] = static_cast<Json::UInt64>(w.activePaneId.v);
            o[kRoot] = paneNodeToJson(w.root);
            return o;
        }

        WorkspaceState workspaceFromJson(const Json::Value& v)
        {
            if (!v.isObject())
            {
                fail("workspace must be an object");
            }
            WorkspaceState w;
            w.id = WorkspaceId{ requireUInt64(v, kId) };
            w.name = requireString(v, kName);
            w.color = optColorFromJson(require(v, kColor));
            w.customDescription = requireString(v, kCustomDescription);
            w.pinned = requireBool(v, kPinned);
            w.activePaneId = PaneId{ requireUInt64(v, kActivePaneId) };
            w.root = paneNodeFromJson(require(v, kRoot));
            return w;
        }
    } // namespace detail

    // -------------------- public API --------------------

    Json::Value toJson(const WorkspaceModelData& s)
    {
        using namespace detail;
        Json::Value root{ Json::objectValue };
        root[kSchemaVersionKey] = kSchemaVersion;
        root[kIdCounter] = static_cast<Json::UInt64>(s.idCounter);
        root[kSidebarWidth] = s.sidebarWidth;
        if (s.activeWorkspaceId.has_value())
        {
            root[kActiveWorkspaceId] = static_cast<Json::UInt64>(s.activeWorkspaceId->v);
        }
        else
        {
            root[kActiveWorkspaceId] = Json::Value{ Json::nullValue };
        }

        Json::Value mruArr{ Json::arrayValue };
        for (const auto& id : s.mru)
        {
            mruArr.append(static_cast<Json::UInt64>(id.v));
        }
        root[kMru] = std::move(mruArr);

        Json::Value wsArr{ Json::arrayValue };
        for (const auto& ws : s.workspaces)
        {
            wsArr.append(workspaceToJson(ws));
        }
        root[kWorkspaces] = std::move(wsArr);

        return root;
    }

    WorkspaceModelData fromJson(const Json::Value& j)
    {
        using namespace detail;
        if (!j.isObject())
        {
            fail("snapshot root must be a JSON object");
        }

        const auto& ver = require(j, kSchemaVersionKey);
        if (!ver.isInt() && !ver.isUInt())
        {
            fail("schemaVersion must be an integer");
        }
        const int v = ver.asInt();
        if (v != ::WorkspaceModel::kSchemaVersion)
        {
            fail("unsupported schemaVersion: " + std::to_string(v) +
                 " (expected " + std::to_string(::WorkspaceModel::kSchemaVersion) + ")");
        }

        WorkspaceModelData out;
        out.idCounter = requireUInt64(j, kIdCounter);
        out.sidebarWidth = requireDouble(j, kSidebarWidth);

        const auto& activeWs = require(j, kActiveWorkspaceId);
        if (activeWs.isNull())
        {
            out.activeWorkspaceId = std::nullopt;
        }
        else if (activeWs.isUInt64() || activeWs.isUInt() || activeWs.isInt())
        {
            out.activeWorkspaceId = WorkspaceId{ activeWs.asUInt64() };
        }
        else
        {
            fail("activeWorkspaceId must be null or uint64");
        }

        const auto& mruArr = require(j, kMru);
        if (!mruArr.isArray())
        {
            fail("mru must be an array");
        }
        for (const auto& entry : mruArr)
        {
            if (!entry.isUInt64() && !entry.isUInt() && !entry.isInt())
            {
                fail("mru entries must be uint64");
            }
            out.mru.push_back(WorkspaceId{ entry.asUInt64() });
        }

        const auto& wsArr = require(j, kWorkspaces);
        if (!wsArr.isArray())
        {
            fail("workspaces must be an array");
        }
        out.workspaces.reserve(wsArr.size());
        for (const auto& w : wsArr)
        {
            out.workspaces.push_back(workspaceFromJson(w));
        }

        return out;
    }

    Json::Value parseJson(const std::string& text)
    {
        Json::CharReaderBuilder builder;
        // Strict mode would reject duplicate keys and the like, but JsonCpp
        // defaults are lenient enough for our purposes. We DO want to fail
        // on actual parse errors -- those become SerializerError.
        std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
        Json::Value root;
        std::string errs;
        if (!reader->parse(text.data(), text.data() + text.size(), &root, &errs))
        {
            detail::fail("JSON parse error: " + errs);
        }
        return root;
    }

    WorkspaceModelData parseFromString(const std::string& jsonText)
    {
        return fromJson(parseJson(jsonText));
    }

    std::string writeCompact(const Json::Value& v)
    {
        Json::StreamWriterBuilder b;
        b["indentation"] = "";
        b["commentStyle"] = "None";
        return Json::writeString(b, v);
    }

    std::string writePretty(const Json::Value& v)
    {
        Json::StreamWriterBuilder b;
        b["indentation"] = "  ";
        b["commentStyle"] = "None";
        return Json::writeString(b, v);
    }
}
