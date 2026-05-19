// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "Fuzzer.h"

#include "../WorkspaceModel/Mutators.h"
#include "../WorkspaceModel/PaneTree.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <type_traits>

using namespace WorkspaceModel;

namespace WorkspaceModelUnitTests
{
    namespace
    {
        // Walk a pane subtree collecting leaves + splits.
        void collectStructure(const PaneNode& node,
                              std::vector<const LeafPane*>& outLeaves,
                              std::vector<const SplitPane*>& outSplits)
        {
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, LeafPane>)
                    {
                        outLeaves.push_back(&n);
                    }
                    else
                    {
                        outSplits.push_back(&n);
                        if (n.left)
                        {
                            collectStructure(*n.left, outLeaves, outSplits);
                        }
                        if (n.right)
                        {
                            collectStructure(*n.right, outLeaves, outSplits);
                        }
                    }
                },
                node);
        }

        // Dispatch an OpRecord against a state. Mirrors Replay.cpp's
        // anonymous-namespace applyOne; reimplemented here because it
        // isn't exported, and the fuzzer wants to call mutators step by
        // step rather than going through the LogEntry replay loop.
        ModelState applyOpRecord(const ModelState& state, const OpRecord& op)
        {
            return std::visit(
                [&state](const auto& rec) -> ModelState {
                    using T = std::decay_t<decltype(rec)>;
                    if constexpr (std::is_same_v<T, NewWorkspaceRecord>)
                    {
                        return WorkspaceModel::newWorkspace(state, rec.name, rec.initialTab,
                                                            rec.initialTabTitle,
                                                            rec.initialTabColor,
                                                            rec.initialTabPinned)
                            .state;
                    }
                    else if constexpr (std::is_same_v<T, CloseWorkspaceRecord>)
                    {
                        return WorkspaceModel::closeWorkspace(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, CloseOtherWorkspacesRecord>)
                    {
                        return WorkspaceModel::closeOtherWorkspaces(state, rec.keep);
                    }
                    else if constexpr (std::is_same_v<T, CloseAllWorkspacesRecord>)
                    {
                        return WorkspaceModel::closeAllWorkspaces(state);
                    }
                    else if constexpr (std::is_same_v<T, SwitchToWorkspaceRecord>)
                    {
                        return WorkspaceModel::switchToWorkspace(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, RenameWorkspaceRecord>)
                    {
                        return WorkspaceModel::renameWorkspace(state, rec.id, rec.name);
                    }
                    else if constexpr (std::is_same_v<T, SetWorkspaceColorRecord>)
                    {
                        return WorkspaceModel::setWorkspaceColor(state, rec.id, rec.color);
                    }
                    else if constexpr (std::is_same_v<T, SetWorkspaceDescriptionRecord>)
                    {
                        return WorkspaceModel::setWorkspaceDescription(state, rec.id, rec.description);
                    }
                    else if constexpr (std::is_same_v<T, SetWorkspacePinnedRecord>)
                    {
                        return WorkspaceModel::setWorkspacePinned(state, rec.id, rec.pinned);
                    }
                    else if constexpr (std::is_same_v<T, ReorderWorkspaceRecord>)
                    {
                        return WorkspaceModel::reorderWorkspace(state, rec.id, rec.dstIdx);
                    }
                    else if constexpr (std::is_same_v<T, NewTabRecord>)
                    {
                        return WorkspaceModel::newTab(state, rec.workspaceId, rec.leafId,
                                                     rec.description,
                                                     rec.customTitle,
                                                     rec.runtimeColor,
                                                     rec.pinned)
                            .state;
                    }
                    else if constexpr (std::is_same_v<T, CloseTabRecord>)
                    {
                        return WorkspaceModel::closeTab(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, CloseTabsRightRecord>)
                    {
                        return WorkspaceModel::closeTabsRight(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, CloseOtherTabsRecord>)
                    {
                        return WorkspaceModel::closeOtherTabs(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, SelectTabRecord>)
                    {
                        return WorkspaceModel::selectTab(state, rec.id);
                    }
                    else if constexpr (std::is_same_v<T, SetTabTitleRecord>)
                    {
                        return WorkspaceModel::setTabTitle(state, rec.id, rec.customTitle);
                    }
                    else if constexpr (std::is_same_v<T, SetTabColorRecord>)
                    {
                        return WorkspaceModel::setTabColor(state, rec.id, rec.color);
                    }
                    else if constexpr (std::is_same_v<T, SetTabPinnedRecord>)
                    {
                        return WorkspaceModel::setTabPinned(state, rec.id, rec.pinned);
                    }
                    else if constexpr (std::is_same_v<T, SplitPaneRecord>)
                    {
                        return WorkspaceModel::splitPane(state, rec.leafId, rec.axis, rec.ratio,
                                                        rec.newTabDescription,
                                                        rec.newTabCustomTitle,
                                                        rec.newTabColor,
                                                        rec.newTabPinned)
                            .state;
                    }
                    else if constexpr (std::is_same_v<T, ClosePaneRecord>)
                    {
                        return WorkspaceModel::closePane(state, rec.leafId);
                    }
                    else if constexpr (std::is_same_v<T, ResizePaneRecord>)
                    {
                        return WorkspaceModel::resizePane(state, rec.splitId, rec.ratio);
                    }
                    else if constexpr (std::is_same_v<T, FocusPaneRecord>)
                    {
                        return WorkspaceModel::focusPane(state, rec.leafId);
                    }
                    else if constexpr (std::is_same_v<T, MoveTabRecord>)
                    {
                        return WorkspaceModel::moveTab(state, rec.tabId, rec.dstLeafId, rec.dstIdx);
                    }
                    else if constexpr (std::is_same_v<T, MoveTabAsSplitRecord>)
                    {
                        return WorkspaceModel::moveTabAsSplit(state, rec.tabId, rec.dstLeafId, rec.edge);
                    }
                    else if constexpr (std::is_same_v<T, SetSidebarWidthRecord>)
                    {
                        return WorkspaceModel::setSidebarWidth(state, rec.width);
                    }
                    else
                    {
                        static_assert(!std::is_same_v<T, T>, "applyOpRecord missing variant arm");
                        return state;
                    }
                },
                op);
        }

        Json::Value violationToJson(std::optional<Violation> v)
        {
            if (!v.has_value())
            {
                return Json::Value(Json::nullValue);
            }
            const char* s = "Unknown";
            switch (*v)
            {
            case Violation::LeafEmpty: s = "LeafEmpty"; break;
            case Violation::SplitArityWrong: s = "SplitArityWrong"; break;
            case Violation::WorkspaceWithoutRoot: s = "WorkspaceWithoutRoot"; break;
            case Violation::ActiveTabIdxOutOfRange: s = "ActiveTabIdxOutOfRange"; break;
            case Violation::ActivePaneIdInvalid: s = "ActivePaneIdInvalid"; break;
            case Violation::ActiveWorkspaceIdInvalid: s = "ActiveWorkspaceIdInvalid"; break;
            case Violation::MruNotPermutationOfWorkspaces: s = "MruNotPermutationOfWorkspaces"; break;
            case Violation::DuplicateContentIdMount: s = "DuplicateContentIdMount"; break;
            }
            return Json::Value(s);
        }

        std::optional<Violation> violationFromJson(const Json::Value& v)
        {
            if (v.isNull()) return std::nullopt;
            const auto s = v.asString();
            if (s == "LeafEmpty") return Violation::LeafEmpty;
            if (s == "SplitArityWrong") return Violation::SplitArityWrong;
            if (s == "WorkspaceWithoutRoot") return Violation::WorkspaceWithoutRoot;
            if (s == "ActiveTabIdxOutOfRange") return Violation::ActiveTabIdxOutOfRange;
            if (s == "ActivePaneIdInvalid") return Violation::ActivePaneIdInvalid;
            if (s == "ActiveWorkspaceIdInvalid") return Violation::ActiveWorkspaceIdInvalid;
            if (s == "MruNotPermutationOfWorkspaces") return Violation::MruNotPermutationOfWorkspaces;
            if (s == "DuplicateContentIdMount") return Violation::DuplicateContentIdMount;
            return std::nullopt;
        }
    } // namespace

    // ------------------------------------------------------------------
    WorkspaceFuzzer::WorkspaceFuzzer(std::uint64_t seed) noexcept
        : _seed{ seed }, _rng{ seed }
    {
    }

    WorkspaceFuzzer::EntityIndex
    WorkspaceFuzzer::indexEntities(const WorkspaceModelData& s)
    {
        EntityIndex idx;
        for (const auto& ws : s.workspaces)
        {
            idx.workspaces.push_back(ws.id);
            std::vector<const LeafPane*> leaves;
            std::vector<const SplitPane*> splits;
            collectStructure(ws.root, leaves, splits);
            for (const auto* l : leaves)
            {
                idx.leaves.push_back(l->id);
                idx.leafWorkspace.push_back(ws.id);
                for (const auto& t : l->tabs)
                {
                    idx.tabs.push_back(t.id);
                    idx.tabLeaf.push_back(l->id);
                }
            }
            for (const auto* sp : splits)
            {
                idx.splits.push_back(sp->id);
            }
        }
        return idx;
    }

    Color WorkspaceFuzzer::randomColor()
    {
        std::uniform_int_distribution<int> d(0, 255);
        return Color{ static_cast<std::uint8_t>(d(_rng)),
                      static_cast<std::uint8_t>(d(_rng)),
                      static_cast<std::uint8_t>(d(_rng)),
                      0xFF };
    }

    Axis WorkspaceFuzzer::randomAxis()
    {
        return (_rng() & 1) ? Axis::Horizontal : Axis::Vertical;
    }

    Edge WorkspaceFuzzer::randomEdge()
    {
        static constexpr Edge edges[] = { Edge::Left, Edge::Right, Edge::Top, Edge::Bottom };
        return edges[_rng() % 4];
    }

    double WorkspaceFuzzer::randomRatio()
    {
        std::uniform_real_distribution<double> d(0.1, 0.9);
        return d(_rng);
    }

    std::string WorkspaceFuzzer::randomName(const char* prefix)
    {
        std::ostringstream os;
        os << prefix << '-' << (_rng() % 100000);
        return os.str();
    }

    TabContent WorkspaceFuzzer::randomTabContent()
    {
        const int kind = static_cast<int>(_rng() % 5);
        switch (kind)
        {
        case 0:
        {
            TerminalSpec t{};
            t.profile[0] = static_cast<std::uint8_t>(_rng() & 0xFF);
            t.profile[1] = static_cast<std::uint8_t>((_rng() >> 8) & 0xFF);
            return t;
        }
        case 1: return SettingsSpec{};
        case 2: return SnippetsSpec{};
        case 3:
        {
            MarkdownSpec md;
            md.file = std::filesystem::path{ "C:/notes/" + randomName("doc") + ".md" };
            return md;
        }
        default: return ScratchpadSpec{};
        }
    }

    // ------------------------------------------------------------------
    OpRecord WorkspaceFuzzer::generateValidOp(const WorkspaceModelData& state)
    {
        const auto idx = indexEntities(state);

        // 25 mutator kinds. Pick one; if its required entities are absent
        // retry; after a few retries fall back to a guaranteed-valid op.
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            // Weighted op selection. We weight create-style ops heavier
            // when the state is small so the fuzzer doesn't get stuck in
            // "empty model + closeTab" no-op territory.
            const std::size_t opCount = (idx.tabs.size() + idx.leaves.size());
            // 0..30 = bucket selector across 25 mutator kinds.
            const int kind = static_cast<int>(_rng() % 30);

            switch (kind)
            {
            case 0: // newWorkspace — always valid
            case 25: // bias toward more newWorkspace when state is small
            case 26:
            {
                if (kind != 0 && opCount > 50)
                {
                    // Don't blow up unbounded.
                    break;
                }
                NewWorkspaceRecord r;
                r.name = randomName("ws");
                r.initialTab = randomTabContent();
                r.initialTabTitle = randomName("tab");
                if (_rng() & 1) r.initialTabColor = randomColor();
                r.initialTabPinned = (_rng() & 1) != 0;
                return r;
            }
            case 1: // closeWorkspace
            {
                if (idx.workspaces.empty()) break;
                CloseWorkspaceRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                return r;
            }
            case 2: // closeOtherWorkspaces
            {
                if (idx.workspaces.empty()) break;
                CloseOtherWorkspacesRecord r;
                r.keep = idx.workspaces[_rng() % idx.workspaces.size()];
                return r;
            }
            case 3: // closeAllWorkspaces — always valid, but limit frequency
            {
                if ((_rng() % 20) != 0) break;
                return CloseAllWorkspacesRecord{};
            }
            case 4: // switchToWorkspace
            {
                if (idx.workspaces.empty()) break;
                SwitchToWorkspaceRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                return r;
            }
            case 5: // renameWorkspace
            {
                if (idx.workspaces.empty()) break;
                RenameWorkspaceRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                r.name = randomName("renamed");
                return r;
            }
            case 6: // setWorkspaceColor
            {
                if (idx.workspaces.empty()) break;
                SetWorkspaceColorRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                if (_rng() & 1) r.color = randomColor();
                return r;
            }
            case 7: // setWorkspaceDescription
            {
                if (idx.workspaces.empty()) break;
                SetWorkspaceDescriptionRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                r.description = randomName("desc");
                return r;
            }
            case 8: // setWorkspacePinned
            {
                if (idx.workspaces.empty()) break;
                SetWorkspacePinnedRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                r.pinned = (_rng() & 1) != 0;
                return r;
            }
            case 9: // reorderWorkspace
            {
                if (idx.workspaces.empty()) break;
                ReorderWorkspaceRecord r;
                r.id = idx.workspaces[_rng() % idx.workspaces.size()];
                r.dstIdx = _rng() % std::max<std::size_t>(1, idx.workspaces.size());
                return r;
            }
            case 10: // newTab
            case 27:
            case 28:
            {
                if (idx.leaves.empty()) break;
                const auto pick = _rng() % idx.leaves.size();
                NewTabRecord r;
                r.workspaceId = idx.leafWorkspace[pick];
                r.leafId = idx.leaves[pick];
                r.description = randomTabContent();
                if (_rng() & 1) r.customTitle = randomName("tt");
                if (_rng() & 1) r.runtimeColor = randomColor();
                r.pinned = (_rng() & 1) != 0;
                return r;
            }
            case 11: // closeTab
            {
                if (idx.tabs.empty()) break;
                CloseTabRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                return r;
            }
            case 12: // closeTabsRight
            {
                if (idx.tabs.empty()) break;
                CloseTabsRightRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                return r;
            }
            case 13: // closeOtherTabs
            {
                if (idx.tabs.empty()) break;
                CloseOtherTabsRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                return r;
            }
            case 14: // selectTab
            {
                if (idx.tabs.empty()) break;
                SelectTabRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                return r;
            }
            case 15: // setTabTitle
            {
                if (idx.tabs.empty()) break;
                SetTabTitleRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                r.customTitle = randomName("title");
                return r;
            }
            case 16: // setTabColor
            {
                if (idx.tabs.empty()) break;
                SetTabColorRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                if (_rng() & 1) r.color = randomColor();
                return r;
            }
            case 17: // setTabPinned
            {
                if (idx.tabs.empty()) break;
                SetTabPinnedRecord r;
                r.id = idx.tabs[_rng() % idx.tabs.size()];
                r.pinned = (_rng() & 1) != 0;
                return r;
            }
            case 18: // splitPane
            case 29:
            {
                if (idx.leaves.empty()) break;
                SplitPaneRecord r;
                r.leafId = idx.leaves[_rng() % idx.leaves.size()];
                r.axis = randomAxis();
                r.ratio = randomRatio();
                r.newTabDescription = randomTabContent();
                return r;
            }
            case 19: // closePane
            {
                if (idx.leaves.empty()) break;
                ClosePaneRecord r;
                r.leafId = idx.leaves[_rng() % idx.leaves.size()];
                return r;
            }
            case 20: // resizePane
            {
                if (idx.splits.empty()) break;
                ResizePaneRecord r;
                r.splitId = idx.splits[_rng() % idx.splits.size()];
                r.ratio = randomRatio();
                return r;
            }
            case 21: // focusPane
            {
                if (idx.leaves.empty()) break;
                FocusPaneRecord r;
                r.leafId = idx.leaves[_rng() % idx.leaves.size()];
                return r;
            }
            case 22: // moveTab
            {
                if (idx.tabs.empty() || idx.leaves.empty()) break;
                MoveTabRecord r;
                r.tabId = idx.tabs[_rng() % idx.tabs.size()];
                r.dstLeafId = idx.leaves[_rng() % idx.leaves.size()];
                r.dstIdx = _rng() % 8; // mutator clamps
                return r;
            }
            case 23: // moveTabAsSplit
            {
                if (idx.tabs.empty() || idx.leaves.empty()) break;
                MoveTabAsSplitRecord r;
                r.tabId = idx.tabs[_rng() % idx.tabs.size()];
                r.dstLeafId = idx.leaves[_rng() % idx.leaves.size()];
                r.edge = randomEdge();
                return r;
            }
            case 24: // setSidebarWidth
            {
                SetSidebarWidthRecord r;
                std::uniform_real_distribution<double> d(0.0, 800.0);
                r.width = d(_rng);
                return r;
            }
            default:
                break;
            }
        }

        // Fallback: newWorkspace is always valid.
        NewWorkspaceRecord r;
        r.name = randomName("fb");
        r.initialTab = TerminalSpec{};
        return r;
    }

    // ------------------------------------------------------------------
    FuzzerOutcome WorkspaceFuzzer::runSteps(ModelState start, std::size_t numSteps)
    {
        FuzzerOutcome out;
        auto state = start ? start : std::make_shared<const WorkspaceModelData>();

        // Snapshot start state for the regression file.
        const WorkspaceModelData startSnapshot = *state;

        bool failed = false;
        for (std::size_t i = 0; i < numSteps; ++i)
        {
            auto op = generateValidOp(*state);
            out.ops.push_back(op);
            auto next = applyOpRecord(state, op);
            if (!next)
            {
                out.failingOpIdx = i;
                out.finalState = state;
                failed = true;
                break;
            }
            if (auto v = validate(*next); v.has_value())
            {
                out.failingOpIdx = i;
                out.violation = v;
                out.finalState = next;
                failed = true;
                break;
            }
            state = next;
        }
        if (!failed)
        {
            out.finalState = state;
        }

        if (out.failingOpIdx.has_value())
        {
            try
            {
                const auto j = serializeRegression(_seed, startSnapshot, out.ops,
                                                   out.failingOpIdx, out.violation);
                // Write to regressions/<timestamp>.json
                std::filesystem::path dir{ "regressions" };
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const auto now = std::chrono::system_clock::now().time_since_epoch();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
                std::ostringstream fn;
                fn << "regression-" << ms << "-seed-" << _seed << ".json";
                auto path = dir / fn.str();
                std::ofstream os(path);
                os << writePretty(j);
                out.regressionPath = path;
            }
            catch (...)
            {
                // best-effort write; don't propagate filesystem errors
                // out of the fuzzer.
            }
        }
        return out;
    }

    FuzzerOutcome WorkspaceFuzzer::runFor(ModelState start, std::chrono::milliseconds budget)
    {
        FuzzerOutcome out;
        auto state = start ? start : std::make_shared<const WorkspaceModelData>();
        const WorkspaceModelData startSnapshot = *state;

        const auto deadline = std::chrono::steady_clock::now() + budget;
        std::size_t step = 0;
        bool failed = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto op = generateValidOp(*state);
            out.ops.push_back(op);
            auto next = applyOpRecord(state, op);
            if (!next)
            {
                out.failingOpIdx = step;
                out.finalState = state;
                failed = true;
                break;
            }
            if (auto v = validate(*next); v.has_value())
            {
                out.failingOpIdx = step;
                out.violation = v;
                out.finalState = next;
                failed = true;
                break;
            }
            state = next;
            ++step;
        }
        if (!failed)
        {
            out.finalState = state;
        }

        if (out.failingOpIdx.has_value())
        {
            try
            {
                const auto j = serializeRegression(_seed, startSnapshot, out.ops,
                                                   out.failingOpIdx, out.violation);
                std::filesystem::path dir{ "regressions" };
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const auto now = std::chrono::system_clock::now().time_since_epoch();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
                std::ostringstream fn;
                fn << "regression-" << ms << "-seed-" << _seed << ".json";
                auto path = dir / fn.str();
                std::ofstream os(path);
                os << writePretty(j);
                out.regressionPath = path;
            }
            catch (...)
            {
            }
        }
        return out;
    }

    // ------------------------------------------------------------------
    Json::Value WorkspaceFuzzer::serializeRegression(
        std::uint64_t seed,
        const WorkspaceModelData& startState,
        const std::vector<OpRecord>& ops,
        std::optional<std::size_t> failingOpIdx,
        std::optional<Violation> violation)
    {
        Json::Value root(Json::objectValue);
        root["seed"] = static_cast<Json::UInt64>(seed);
        root["startState"] = toJson(startState);
        Json::Value opArr(Json::arrayValue);
        for (const auto& op : ops)
        {
            opArr.append(opRecordToJson(op));
        }
        root["ops"] = opArr;
        if (failingOpIdx.has_value())
        {
            root["failingOpIdx"] = static_cast<Json::UInt64>(*failingOpIdx);
        }
        else
        {
            root["failingOpIdx"] = Json::Value(Json::nullValue);
        }
        root["violation"] = violationToJson(violation);
        return root;
    }

    WorkspaceFuzzer::RegressionFile
    WorkspaceFuzzer::parseRegression(const Json::Value& j)
    {
        RegressionFile f;
        if (j.isMember("seed"))
        {
            f.seed = j["seed"].asUInt64();
        }
        if (j.isMember("startState"))
        {
            f.startState = fromJson(j["startState"]);
        }
        if (j.isMember("ops") && j["ops"].isArray())
        {
            for (const auto& v : j["ops"])
            {
                f.ops.push_back(opRecordFromJson(v));
            }
        }
        if (j.isMember("failingOpIdx") && !j["failingOpIdx"].isNull())
        {
            f.failingOpIdx = static_cast<std::size_t>(j["failingOpIdx"].asUInt64());
        }
        if (j.isMember("violation"))
        {
            f.violation = violationFromJson(j["violation"]);
        }
        return f;
    }

    FuzzerOutcome WorkspaceFuzzer::replayRegression(const RegressionFile& f)
    {
        FuzzerOutcome out;
        auto state = std::make_shared<const WorkspaceModelData>(f.startState);
        out.ops = f.ops;
        for (std::size_t i = 0; i < f.ops.size(); ++i)
        {
            auto next = applyOpRecord(state, f.ops[i]);
            if (!next)
            {
                out.failingOpIdx = i;
                out.finalState = state;
                return out;
            }
            if (auto v = validate(*next); v.has_value())
            {
                out.failingOpIdx = i;
                out.violation = v;
                out.finalState = next;
                return out;
            }
            state = next;
        }
        out.finalState = state;
        return out;
    }
}
