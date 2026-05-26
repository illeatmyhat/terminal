// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CrashCapture.h"
#include "CppWinrtTailored.h"

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/TerminalApp.h>

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

namespace TerminalAppLocalTests
{
    // -- small file helpers (read raw bytes so we can search for ASCII needles
    //    regardless of the on-disk UTF-8 encoding) ------------------------- //
    static std::string readAllBytes(const std::wstring& path)
    {
        std::string out;
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        char buf[4096];
        DWORD read = 0;
        while (::ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0)
        {
            out.append(buf, read);
        }
        ::CloseHandle(h);
        return out;
    }

    // Polls a file (worker-thread side) until it contains `needle` or we time
    // out. The live TestHostApp UI thread auto-dispatches queued ops, so we
    // never have to pump it ourselves -- which keeps this hang-free even if the
    // survive-the-throw hypothesis turns out false.
    static bool waitForFileContaining(const std::wstring& path, const std::string& needle, int timeoutMs)
    {
        constexpr int step = 50;
        for (int waited = 0; waited <= timeoutMs; waited += step)
        {
            if (readAllBytes(path).find(needle) != std::string::npos)
            {
                return true;
            }
            ::Sleep(step);
        }
        return false;
    }

    static std::wstring toWide(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    // Echo a (UTF-8) artifact file into the TAEF log. The TestHostApp runs in a
    // transient AppContainer that is unregistered after the run, taking its
    // AC\Temp (where the artifacts live) with it -- so this is how the
    // diagnostic context survives for post-hoc (agent) inspection: TAEF marshals
    // Log output to the durable te.log OUTSIDE the container. (For the eventual
    // live dev-app smoke the artifacts persist on disk in the permanently
    // installed package's AC\Temp; this echo is the TAEF-context exfiltration.)
    static void dumpFileToLog(const std::wstring& path, const wchar_t* label)
    {
        const std::string bytes = readAllBytes(path);
        Log::Comment(NoThrowString().Format(L"===== %ls (%zu bytes) =====", label, bytes.size()));
        Log::Comment(bytes.empty() ? L"(file is empty or unreadable)" : toWide(bytes).c_str());
    }

    static std::wstring crashLogPath()
    {
        std::wstring p = TerminalApp::Diagnostics::ArtifactDir();
        p += L"\\crash-";
        p += std::to_wstring(::GetCurrentProcessId());
        p += L".log";
        return p;
    }

    static std::wstring breadcrumbsPath()
    {
        std::wstring p = TerminalApp::Diagnostics::ArtifactDir();
        p += L"\\breadcrumbs-";
        p += std::to_wstring(::GetCurrentProcessId());
        p += L".log";
        return p;
    }

    // Dedicated, drag-only rig for iterating on MUX TabView drag/drop with crash
    // containment. Run ISOLATED so a fail-fast here can't take down the suite:
    //   TE.exe TerminalApp.LocalTests.dll /name:*TabStripDrag* /isolationLevel:class
    //
    // Slice 1 (this file) proves the diagnostic PIPELINE ONLY (no TabView yet):
    // install CrashCapture, then deliberately fail and confirm artifacts land
    // with NO modal dialog and the process survives the catchable case.
    class TabStripDragTests
    {
        BEGIN_TEST_CLASS(TabStripDragTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_CLASS_SETUP(ClassSetup)
        {
            wchar_t tmp[MAX_PATH]{};
            ::GetTempPathW(MAX_PATH, tmp);
            const std::wstring artifactDir = std::wstring(tmp) + L"TabStripDrag-crash";

            TerminalApp::Diagnostics::InstallCrashCapture(artifactDir);
            Log::Comment(NoThrowString().Format(L"CrashCapture artifacts: %ls", artifactDir.c_str()));

            // Attach the XAML surface to the running TestHostApp App, on the UI
            // thread (Application::UnhandledException + DebugSettings tracing).
            const auto hr = RunOnUIThread([]() {
                TerminalApp::Diagnostics::AttachXamlDiagnostics(
                    winrt::Windows::UI::Xaml::Application::Current());
            });
            VERIFY_SUCCEEDED(hr);
            return true;
        }

        // Proves the uncatchable-tier lifeline: breadcrumbs are flushed to disk
        // the instant they're written (the only context that survives a true
        // fail-fast, because no code runs after it).
        TEST_METHOD(Pipeline_Breadcrumbs_AreFlushedToDisk)
        {
            const std::string nonce = std::to_string(::GetTickCount64());
            const std::string a = "BREADCRUMB_A_" + nonce;
            const std::string b = "BREADCRUMB_B_" + nonce;
            const std::string c = "BREADCRUMB_C_" + nonce;

            TerminalApp::Diagnostics::Breadcrumb(toWide(a), L"detail-1");
            TerminalApp::Diagnostics::Breadcrumb(toWide(b), L"detail-2");
            TerminalApp::Diagnostics::Breadcrumb(toWide(c), L"detail-3");

            // No flush call needed: each Breadcrumb FlushFileBuffers'd already.
            const std::string bytes = readAllBytes(breadcrumbsPath());
            VERIFY_IS_FALSE(bytes.empty(), L"breadcrumb log should exist and be non-empty");
            VERIFY_IS_TRUE(bytes.find(a) != std::string::npos, L"first breadcrumb missing");
            VERIFY_IS_TRUE(bytes.find(b) != std::string::npos, L"second breadcrumb missing");
            VERIFY_IS_TRUE(bytes.find(c) != std::string::npos, L"third breadcrumb missing");

            dumpFileToLog(breadcrumbsPath(), L"breadcrumbs.log");
        }

        // Proves the catchable tier: a XAML/WinRT exception surfaced through
        // Application::UnhandledException is logged AND the iteration survives
        // (Handled=true) AND no modal dialog appears (the test simply keeps
        // running -- a modal would have frozen us before this returns).
        TEST_METHOD(Pipeline_XamlUnhandledException_CaughtAndLogged_NoDialog)
        {
            const std::string nonce = std::to_string(::GetTickCount64());
            const std::string needle = "CrashCapture self-test " + nonce;
            const std::wstring message = toWide(needle);

            // Post a RAW dispatcher callback that throws. We deliberately do NOT
            // use RunOnUIThread here: its WEX::SafeInvoke wrapper would swallow
            // the exception before it could reach Application::UnhandledException.
            auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
            dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [message]() {
                throw winrt::hresult_error(E_FAIL, message);
            });

            // The live UI thread dispatches the throwing op on its own; the
            // UnhandledException handler writes crash.log synchronously during
            // that dispatch. Poll for it from this (worker) thread.
            const bool logged = waitForFileContaining(crashLogPath(), needle, 5000);
            VERIFY_IS_TRUE(logged, L"crash.log should contain the thrown message (Application::UnhandledException did not route, or the surface did not log)");

            // Reaching here at all == we survived the throw with no fail-fast and
            // no modal box. Confirm the post-mortem is correctly classified.
            const std::string bytes = readAllBytes(crashLogPath());
            VERIFY_IS_TRUE(bytes.find("XAML.UnhandledException") != std::string::npos, L"crash.log should be the XAML-routed kind");

            dumpFileToLog(crashLogPath(), L"crash.log");
        }

        // Slice 3b: stand up a REAL TabStripView (the control the drag handlers
        // live on) under the crash-capture surface, with NO TerminalPage -- a
        // lightweight, standalone drag rig (vs. WorkspaceTests, which hosts the
        // strip through a full flag-on page). Prove the bare strip materializes
        // its inner MUX TabView, arms the three drag flags (the
        // AllowDropTabs+reorder combo that is the 0xc000027b drag-out failfast
        // surface; the strip's ctor wires TabDroppedOutside to satisfy it), and
        // projects one Tag-carrying TabViewItem per PaneTabViewModel. This is the
        // host that slice 3c (programmatic _onTabStripDrop) and 3d (synthesized
        // drag-out) will drive.
        TEST_METHOD(HostRealTabStripView_ProjectsItems_DragFlagsArmed)
        {
            using namespace TerminalApp::Diagnostics;
            namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;

            Breadcrumb(L"3b.Begin");
            const auto result = RunOnUIThread([]() {
                Breadcrumb(L"3b.ConstructStrip");
                winrt::TerminalApp::TabStripView strip{};

                const auto tabView = strip.TabViewControl();
                VERIFY_IS_NOT_NULL(tabView, L"the bare TabStripView must materialize its inner MUX TabView");

                // The crash-relevant configuration: all three armed in XAML. The
                // AllowDropTabs(true) demands a wired TabDroppedOutside handler or a
                // drag-out is the 0xc000027b failfast (the ctor wires it).
                VERIFY_IS_TRUE(tabView.CanReorderTabs(), L"CanReorderTabs armed (within-leaf reorder)");
                VERIFY_IS_TRUE(tabView.CanDragTabs(), L"CanDragTabs armed (cross-leaf drag source)");
                VERIFY_IS_TRUE(tabView.AllowDropTabs(), L"AllowDropTabs armed (cross-leaf drop target)");

                Breadcrumb(L"3b.ProjectVMs");
                auto source = winrt::single_threaded_observable_vector<winrt::TerminalApp::PaneTabViewModel>();
                for (uint64_t i = 0; i < 3; ++i)
                {
                    winrt::TerminalApp::PaneTabViewModel vm{};
                    vm.Id(0x3B00 + i);
                    vm.Title(winrt::hstring{ L"3b tab " + std::to_wstring(i) });
                    source.Append(vm);
                }
                strip.ItemsSource(source);

                VERIFY_ARE_EQUAL(3u, tabView.TabItems().Size(), L"the strip projects one TabViewItem per VM");

                // Each TabViewItem carries its VM in Tag -- the drag-identity path
                // that _onTabStripDrop (3c) reads back from the DataPackage.
                const auto item0 = tabView.TabItems().GetAt(0).try_as<MUXC::TabViewItem>();
                VERIFY_IS_NOT_NULL(item0, L"projected item is a TabViewItem");
                const auto vm0 = item0.Tag().try_as<winrt::TerminalApp::PaneTabViewModel>();
                VERIFY_IS_NOT_NULL(vm0, L"each TabViewItem.Tag is its PaneTabViewModel");
                VERIFY_ARE_EQUAL(static_cast<uint64_t>(0x3B00), vm0.Id());

                Breadcrumb(L"3b.OK");
            });
            VERIFY_SUCCEEDED(result);
        }
    };
}
