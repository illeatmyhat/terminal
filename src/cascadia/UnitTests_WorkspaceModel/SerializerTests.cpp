// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Round-trip and schema tests for Serializer.h.

#include "pch.h"

#include "TestHelpers.h"

#include "../WorkspaceModel/Serializer.h"

using namespace WorkspaceModel;
using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace WorkspaceModelUnitTests;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Build a model with one workspace, one split pane, both leaves
        // populated with several tabs covering every TabContent arm.
        ModelState makeRichModel()
        {
            auto m = emptyModel();
            auto a = newWorkspace(m, "alpha", termSpec(1));

            // Add a markdown tab.
            MarkdownSpec md;
            md.file = std::filesystem::path{ "C:/some/path with spaces/readme.md" };
            auto r1 = newTab(a.state, a.id, std::get<LeafPane>(a.state->workspaces[0].root).id, md, "doc", Color{ 0xAB, 0xCD, 0xEF, 0xFF }, true);

            // Add a settings + snippets + scratchpad tab.
            auto leafId = std::get<LeafPane>(r1.state->workspaces[0].root).id;
            auto r2 = newTab(r1.state, a.id, leafId, SettingsSpec{}, "Settings");
            auto r3 = newTab(r2.state, a.id, leafId, SnippetsSpec{}, "");
            auto r4 = newTab(r3.state, a.id, leafId, ScratchpadSpec{}, "scratch");

            // Split into two panes.
            auto s = splitPane(r4.state, leafId, Axis::Horizontal, 0.4, termSpec(7), "sibling");

            // Customize the workspace a bit.
            auto c = setWorkspaceColor(s.state, a.id, Color{ 0x10, 0x20, 0x30, 0x80 });
            auto d = setWorkspaceDescription(c, a.id, "ws description text");
            auto p = setWorkspacePinned(d, a.id, true);
            auto sw = setSidebarWidth(p, 312.5);

            // Add a second workspace so MRU has multiple entries.
            auto b = newWorkspace(sw, "beta", termSpec(2));
            return b.state;
        }

        // Equality-modulo-mount: compare two WorkspaceModelData values
        // ignoring any TabRecord.mount (which is runtime-only and reset
        // to nullopt on deserialize).
        WorkspaceModelData clearMounts(const WorkspaceModelData& s)
        {
            WorkspaceModelData out = s;
            std::function<PaneNode(const PaneNode&)> walk = [&](const PaneNode& n) -> PaneNode {
                if (std::holds_alternative<LeafPane>(n))
                {
                    auto leaf = std::get<LeafPane>(n);
                    for (auto& t : leaf.tabs)
                    {
                        t.mount = std::nullopt;
                    }
                    return PaneNode{ leaf };
                }
                auto sp = std::get<SplitPane>(n);
                if (sp.left)
                {
                    sp.left = std::make_shared<const PaneNode>(walk(*sp.left));
                }
                if (sp.right)
                {
                    sp.right = std::make_shared<const PaneNode>(walk(*sp.right));
                }
                return PaneNode{ sp };
            };
            for (auto& ws : out.workspaces)
            {
                ws.root = walk(ws.root);
            }
            return out;
        }
    }

    class SerializerTests
    {
        TEST_CLASS(SerializerTests);

        TEST_METHOD(EmptyModel_RoundTrips);
        TEST_METHOD(SingleWorkspace_RoundTrips);
        TEST_METHOD(RichModel_RoundTrips);
        TEST_METHOD(EveryTabContentArm_RoundTrips);
        TEST_METHOD(EveryAxis_RoundTrips);
        TEST_METHOD(ColorEncoding_IsRrggbbaaHex);
        TEST_METHOD(NullColor_RoundTripsAsNullopt);
        TEST_METHOD(MountAlwaysSerializesAsNull);
        TEST_METHOD(MountInInputState_DroppedOnRoundTrip);
        TEST_METHOD(SchemaVersion_Present);
        TEST_METHOD(UnknownSchemaVersion_Throws);
        TEST_METHOD(MissingRequiredField_Throws);
        TEST_METHOD(MalformedJson_Throws);
    };

    void SerializerTests::EmptyModel_RoundTrips()
    {
        auto m = emptyModel();
        const auto j = toJson(*m);
        const auto back = fromJson(j);
        VERIFY_ARE_EQUAL(*m, back);
    }

    void SerializerTests::SingleWorkspace_RoundTrips()
    {
        auto f = makeSingleWorkspace();
        const auto j = toJson(*f.state);
        const auto back = fromJson(j);
        VERIFY_ARE_EQUAL(*f.state, back);
    }

    void SerializerTests::RichModel_RoundTrips()
    {
        auto m = makeRichModel();
        const auto cleaned = clearMounts(*m);
        const auto j = toJson(*m);
        const auto back = fromJson(j);
        VERIFY_ARE_EQUAL(cleaned, back);
    }

    void SerializerTests::EveryTabContentArm_RoundTrips()
    {
        struct Case
        {
            const char* label;
            TabContent tc;
        };
        std::vector<Case> cases;
        cases.push_back({ "terminal", TerminalSpec{ { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } } });
        cases.push_back({ "settings", SettingsSpec{} });
        cases.push_back({ "snippets", SnippetsSpec{} });
        MarkdownSpec md;
        md.file = std::filesystem::path(std::u8string(u8"C:/temp/notes-éè.md"));
        cases.push_back({ "markdown", md });
        cases.push_back({ "scratchpad", ScratchpadSpec{} });

        for (const auto& c : cases)
        {
            auto m = emptyModel();
            auto r = newWorkspace(m, "ws", c.tc);
            const auto j = toJson(*r.state);
            const auto back = fromJson(j);
            const auto& backLeaf = std::get<LeafPane>(back.workspaces[0].root);
            VERIFY_ARE_EQUAL(c.tc.index(), backLeaf.tabs[0].description.index());
            VERIFY_IS_TRUE(c.tc == backLeaf.tabs[0].description);
        }
    }

    void SerializerTests::EveryAxis_RoundTrips()
    {
        for (auto a : { Axis::Horizontal, Axis::Vertical })
        {
            auto f = makeSingleWorkspace();
            auto s = splitPane(f.state, f.leafId, a, 0.3, termSpec(9));
            const auto j = toJson(*s.state);
            const auto back = fromJson(j);
            const auto& split = std::get<SplitPane>(back.workspaces[0].root);
            VERIFY_IS_TRUE(split.axis == a);
            VERIFY_ARE_EQUAL(0.3, split.ratio);
        }
    }

    void SerializerTests::ColorEncoding_IsRrggbbaaHex()
    {
        auto f = makeSingleWorkspace();
        auto withColor = setWorkspaceColor(f.state, f.wsId, Color{ 0x12, 0x34, 0x56, 0x78 });
        const auto j = toJson(*withColor);
        const auto& wsJson = j["workspaces"][0u];
        const auto colorStr = wsJson["color"].asString();
        VERIFY_ARE_EQUAL(std::string{ "#12345678" }, colorStr);
    }

    void SerializerTests::NullColor_RoundTripsAsNullopt()
    {
        auto f = makeSingleWorkspace();
        const auto j = toJson(*f.state);
        VERIFY_IS_TRUE(j["workspaces"][0u]["color"].isNull());
        const auto back = fromJson(j);
        VERIFY_IS_FALSE(back.workspaces[0].color.has_value());
    }

    void SerializerTests::MountAlwaysSerializesAsNull()
    {
        // Mount is runtime-only; even when set, it serializes as JSON null.
        auto f = makeSingleWorkspace();
        // Pollute mount in a copy. This is contrived because actions
        // never set mount, but defense-in-depth.
        auto patched = std::make_shared<WorkspaceModelData>(*f.state);
        auto& leaf = std::get<LeafPane>(patched->workspaces[0].root);
        leaf.tabs[0].mount = ContentId{ 42 };
        const auto j = toJson(*patched);
        VERIFY_IS_TRUE(j["workspaces"][0u]["root"]["tabs"][0u]["mount"].isNull());
    }

    void SerializerTests::MountInInputState_DroppedOnRoundTrip()
    {
        auto f = makeSingleWorkspace();
        auto patched = std::make_shared<WorkspaceModelData>(*f.state);
        auto& leaf = std::get<LeafPane>(patched->workspaces[0].root);
        leaf.tabs[0].mount = ContentId{ 99 };
        const auto j = toJson(*patched);
        const auto back = fromJson(j);
        const auto& backLeaf = std::get<LeafPane>(back.workspaces[0].root);
        VERIFY_IS_FALSE(backLeaf.tabs[0].mount.has_value());
    }

    void SerializerTests::SchemaVersion_Present()
    {
        auto m = emptyModel();
        const auto j = toJson(*m);
        VERIFY_IS_TRUE(j.isMember("schemaVersion"));
        VERIFY_ARE_EQUAL(kSchemaVersion, j["schemaVersion"].asInt());
    }

    void SerializerTests::UnknownSchemaVersion_Throws()
    {
        auto m = emptyModel();
        auto j = toJson(*m);
        j["schemaVersion"] = 999;
        bool threw = false;
        try
        {
            (void)fromJson(j);
        }
        catch (const SerializerError&)
        {
            threw = true;
        }
        VERIFY_IS_TRUE(threw);
    }

    void SerializerTests::MissingRequiredField_Throws()
    {
        auto m = emptyModel();
        auto j = toJson(*m);
        j.removeMember("idCounter");
        bool threw = false;
        try
        {
            (void)fromJson(j);
        }
        catch (const SerializerError&)
        {
            threw = true;
        }
        VERIFY_IS_TRUE(threw);
    }

    void SerializerTests::MalformedJson_Throws()
    {
        const std::string garbage = "{ this is not json";
        bool threw = false;
        try
        {
            (void)parseFromString(garbage);
        }
        catch (const SerializerError&)
        {
            threw = true;
        }
        VERIFY_IS_TRUE(threw);
    }
}
