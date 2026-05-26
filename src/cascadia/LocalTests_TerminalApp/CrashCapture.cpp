// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// See CrashCapture.h for the why. This is the implementation of the
// self-contained crash-diagnostic surface. It is deliberately dependency-light
// (Win32 + CRT + C++/WinRT only) so it can later move verbatim into the
// packaged dev app for live drag smoke runs.

#include "pch.h"
#include "CrashCapture.h"

#include <Windows.h>
#include <dbghelp.h>
#include <restrictederrorinfo.h>
#include <roerrorapi.h>
#include <rtcapi.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <exception>
#include <mutex>
#include <deque>
#include <atomic>

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Data.h>

#ifndef STATUS_STACK_BUFFER_OVERRUN
#define STATUS_STACK_BUFFER_OVERRUN ((DWORD)0xC0000409L)
#endif

namespace
{
    // --------------------------------------------------------------------- //
    // State. g_mutex guards g_ring ONLY. The breadcrumb file is opened with
    // FILE_APPEND_DATA, so the OS serialises appends across threads/handles --
    // we deliberately write to it WITHOUT holding g_mutex from crash paths so a
    // crash that happened while g_mutex was held cannot deadlock the handler.
    // --------------------------------------------------------------------- //
    constexpr size_t kRingCap = 64;

    std::wstring g_artifactDir;
    std::mutex g_mutex;
    std::deque<std::wstring> g_ring;
    HANDLE g_breadcrumbFile = INVALID_HANDLE_VALUE;
    std::atomic<bool> g_installed{ false };

    // --------------------------------------------------------------------- //
    // Small, allocation-frugal helpers.
    // --------------------------------------------------------------------- //
    std::wstring nowStamp()
    {
        SYSTEMTIME st{};
        ::GetSystemTime(&st); // UTC
        wchar_t buf[32]{};
        ::swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u.%03u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }

    std::wstring hex32(uint32_t v)
    {
        wchar_t b[16]{};
        ::swprintf_s(b, L"%08X", v);
        return b;
    }

    std::wstring widenUtf8(const char* s)
    {
        if (!s)
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (n <= 1)
        {
            return {};
        }
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
        return w;
    }

    void writeUtf8(HANDLE h, std::wstring_view text)
    {
        if (h == nullptr || h == INVALID_HANDLE_VALUE || text.empty())
        {
            return;
        }
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
        {
            return;
        }
        std::string buf(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buf.data(), len, nullptr, nullptr);
        DWORD written{};
        ::WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
    }

    std::wstring makeLine(std::wstring_view event, std::wstring_view detail)
    {
        std::wstring line;
        line += L'[';
        line += nowStamp();
        line += L"][pid:";
        line += std::to_wstring(::GetCurrentProcessId());
        line += L" tid:";
        line += std::to_wstring(::GetCurrentThreadId());
        line += L"] ";
        line.append(event);
        if (!detail.empty())
        {
            line += L" | ";
            line.append(detail);
        }
        line += L"\r\n";
        return line;
    }

    // Lock-free file append (see g_mutex note above): safe to call from a crash
    // handler that may be holding g_mutex.
    void appendBreadcrumbLine(const std::wstring& line)
    {
        if (g_breadcrumbFile != INVALID_HANDLE_VALUE)
        {
            writeUtf8(g_breadcrumbFile, line);
            ::FlushFileBuffers(g_breadcrumbFile);
        }
    }

    std::wstring breadcrumbPath()
    {
        std::wstring p = g_artifactDir;
        p += L"\\breadcrumbs-";
        p += std::to_wstring(::GetCurrentProcessId());
        p += L".log";
        return p;
    }

    // Raw COM read of the WinRT restricted-error-info for the current thread.
    // Best-effort; GetRestrictedErrorInfo consumes (clears) the slot on success.
    std::wstring captureRestrictedError()
    {
        std::wstring out;
        winrt::com_ptr<IRestrictedErrorInfo> info;
        if (SUCCEEDED(::GetRestrictedErrorInfo(info.put())) && info)
        {
            HRESULT errorHr{};
            BSTR description{};
            BSTR restricted{};
            BSTR capabilitySid{};
            if (SUCCEEDED(info->GetErrorDetails(&description, &errorHr, &restricted, &capabilitySid)))
            {
                out += L"errorCode=0x";
                out += hex32(static_cast<uint32_t>(errorHr));
                out += L"\r\n";
                if (description && ::SysStringLen(description))
                {
                    out += L"description=";
                    out += description;
                    out += L"\r\n";
                }
                if (restricted && ::SysStringLen(restricted))
                {
                    out += L"restrictedDescription=";
                    out += restricted;
                    out += L"\r\n";
                }
                if (capabilitySid && ::SysStringLen(capabilitySid))
                {
                    out += L"capabilitySid=";
                    out += capabilitySid;
                    out += L"\r\n";
                }
                ::SysFreeString(description);
                ::SysFreeString(restricted);
                ::SysFreeString(capabilitySid);
            }
            BSTR reference{};
            if (SUCCEEDED(info->GetReference(&reference)) && reference && ::SysStringLen(reference))
            {
                out += L"reference=";
                out += reference;
                out += L"\r\n";
                ::SysFreeString(reference);
            }
        }
        return out;
    }

    // Writes a minidump; returns its path (empty on failure). ep may be null
    // (terminate/abort paths have no EXCEPTION_POINTERS).
    std::wstring writeMiniDump(EXCEPTION_POINTERS* ep)
    {
        if (g_artifactDir.empty())
        {
            return {};
        }
        std::wstring path = g_artifactDir;
        path += L"\\crash-";
        path += std::to_wstring(::GetCurrentProcessId());
        path += L"-";
        path += nowStamp();
        path += L".dmp";

        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = ::GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        const auto type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo |
            MiniDumpWithFullMemoryInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpWithIndirectlyReferencedMemory);

        const BOOL ok = ::MiniDumpWriteDump(
            ::GetCurrentProcess(),
            ::GetCurrentProcessId(),
            file,
            type,
            ep ? &mei : nullptr,
            nullptr,
            nullptr);

        ::CloseHandle(file);
        return ok ? path : std::wstring{};
    }

    // LLM-readable post-mortem: headed sections, most-important first.
    void writeCrashLog(std::wstring_view kind, std::wstring_view detail, std::wstring_view restricted, std::wstring_view dumpPath)
    {
        if (g_artifactDir.empty())
        {
            return;
        }

        std::wstring text;
        text += L"## SUMMARY\r\n";
        text += L"Kind: ";
        text.append(kind);
        text += L"\r\n";
        text += L"PID: ";
        text += std::to_wstring(::GetCurrentProcessId());
        text += L"  TID: ";
        text += std::to_wstring(::GetCurrentThreadId());
        text += L"  Time: ";
        text += nowStamp();
        text += L"\r\n";
        if (!detail.empty())
        {
            text += L"Detail: ";
            text.append(detail);
            text += L"\r\n";
        }

        text += L"\r\n## RESTRICTED ERROR INFO\r\n";
        if (restricted.empty())
        {
            text += L"(none)\r\n";
        }
        else
        {
            text.append(restricted);
            if (restricted.back() != L'\n')
            {
                text += L"\r\n";
            }
        }

        text += L"\r\n## RECENT UI EVENTS\r\n";
        {
            // try_lock: if a crash happened while g_mutex was held, do NOT block.
            std::unique_lock<std::mutex> lk(g_mutex, std::try_to_lock);
            if (lk.owns_lock())
            {
                if (g_ring.empty())
                {
                    text += L"(no breadcrumbs recorded)\r\n";
                }
                else
                {
                    for (const auto& e : g_ring)
                    {
                        text.append(e); // each line already ends with \r\n
                    }
                }
            }
            else
            {
                text += L"(breadcrumb ring unavailable: lock held at crash)\r\n";
            }
        }

        text += L"\r\n## ARTIFACTS\r\n";
        text += L"Dump: ";
        if (dumpPath.empty())
        {
            text += L"(none)";
        }
        else
        {
            text.append(dumpPath);
        }
        text += L"\r\n";
        text += L"Breadcrumbs: ";
        text += breadcrumbPath();
        text += L"\r\n";

        std::wstring path = g_artifactDir;
        path += L"\\crash-";
        path += std::to_wstring(::GetCurrentProcessId());
        path += L".log";

        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            writeUtf8(h, text);
            ::FlushFileBuffers(h);
            ::CloseHandle(h);
        }
    }

    // Unified capture: a pre-crash breadcrumb (lock-free), then dump + crash.log.
    // Guarded against reentrancy per-thread so a fault INSIDE the handler can't
    // recurse forever. Best-effort throughout.
    void captureToArtifacts(std::wstring_view kind, std::wstring_view detail, std::wstring_view restricted, EXCEPTION_POINTERS* ep)
    {
        static thread_local bool inHandler = false;
        if (inHandler)
        {
            return;
        }
        inHandler = true;

        appendBreadcrumbLine(makeLine(kind, detail));
        const std::wstring dump = writeMiniDump(ep);
        writeCrashLog(kind, detail, restricted, dump);

        inHandler = false;
    }

    // --------------------------------------------------------------------- //
    // Group 3 -- process-level exception nets.
    // --------------------------------------------------------------------- //
    LONG CALLBACK vectoredHandler(PEXCEPTION_POINTERS ep)
    {
        // VEH fires for EVERY first-chance exception, so stay cheap: only react
        // to the fail-fast stack-cookie code, and document that __fastfail
        // usually bypasses even this.
        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == STATUS_STACK_BUFFER_OVERRUN)
        {
            captureToArtifacts(L"VEH/STACK_BUFFER_OVERRUN(0xC0000409)",
                               L"best-effort; a true __fastfail usually bypasses the vectored handler",
                               {},
                               ep);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI unhandledSehFilter(PEXCEPTION_POINTERS ep)
    {
        const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
        std::wstring detail = L"code=0x";
        detail += hex32(static_cast<uint32_t>(code));
        captureToArtifacts(L"UnhandledSEH", detail, {}, ep);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void terminateHandler()
    {
        std::wstring detail;
        try
        {
            const auto e = std::current_exception();
            if (e)
            {
                std::rethrow_exception(e);
            }
            else
            {
                detail = L"(std::terminate with no current exception)";
            }
        }
        catch (const winrt::hresult_error& hr)
        {
            detail = L"winrt::hresult_error 0x";
            detail += hex32(static_cast<uint32_t>(hr.code()));
            detail += L": ";
            detail += hr.message().c_str();
        }
        catch (const std::exception& ex)
        {
            detail = L"std::exception: ";
            detail += widenUtf8(ex.what());
        }
        catch (...)
        {
            detail = L"(non-standard exception)";
        }

        captureToArtifacts(L"std::terminate", detail, captureRestrictedError(), nullptr);
        // Falls through to the default terminate behaviour (abort). Our SIGABRT
        // handler is reentrancy-guarded so it won't double-capture on the same
        // thread.
    }

    void sigabrtHandler(int)
    {
        captureToArtifacts(L"SIGABRT", L"abort() raised", captureRestrictedError(), nullptr);
    }

    void __cdecl invalidParameterHandler(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned int line, uintptr_t)
    {
        std::wstring detail;
        detail += L"expr=";
        detail += (expr ? expr : L"(unknown)");
        detail += L" func=";
        detail += (func ? func : L"(unknown)");
        detail += L" file=";
        detail += (file ? file : L"(unknown)");
        detail += L" line=";
        detail += std::to_wstring(line);
        captureToArtifacts(L"InvalidParameter", detail, {}, nullptr);
    }

#ifdef _DEBUG
    // Intercepts the /RTC run-time checks (incl. /RTCs "Stack around variable
    // corrupted") that otherwise pop a BLOCKING dialog. We log and return 0
    // (continue without a debugger), which suppresses the modal box.
    int __cdecl rtcErrorHandler(int errType, const wchar_t* file, int line, const wchar_t* module, const wchar_t* format, ...)
    {
        wchar_t msg[1024]{};
        if (format)
        {
            va_list ap;
            va_start(ap, format);
            ::_vsnwprintf_s(msg, _countof(msg), _TRUNCATE, format, ap);
            va_end(ap);
        }

        std::wstring detail;
        detail += L"errType=";
        detail += std::to_wstring(errType);
        if (file)
        {
            detail += L" file=";
            detail += file;
        }
        detail += L" line=";
        detail += std::to_wstring(line);
        if (module)
        {
            detail += L" module=";
            detail += module;
        }
        detail += L" msg=";
        detail += msg;

        captureToArtifacts(L"RTC_RuntimeCheck", detail, {}, nullptr);
        return 0; // continue without breaking into a debugger -> no modal box
    }
#endif
}

namespace TerminalApp::Diagnostics
{
    void InstallCrashCapture(std::wstring_view artifactDir)
    {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true))
        {
            return; // idempotent
        }

        g_artifactDir.assign(artifactDir);
        ::CreateDirectoryW(g_artifactDir.c_str(), nullptr);

        const std::wstring bp = breadcrumbPath();
        g_breadcrumbFile = ::CreateFileW(bp.c_str(),
                                         FILE_APPEND_DATA,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr,
                                         OPEN_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr);

        // ---- Group 1: kill modal dialogs FIRST (a frozen box is worse than a
        // crash for an unattended run). ----
        ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
        for (const int rt : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
        {
            ::_CrtSetReportMode(rt, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            ::_CrtSetReportFile(rt, _CRTDBG_FILE_STDERR);
        }
        ::_RTC_SetErrorFuncW(&rtcErrorHandler);
#endif
        ::_set_invalid_parameter_handler(&invalidParameterHandler);

        // ---- Group 3: process-level exception nets. ----
        ::AddVectoredExceptionHandler(1, &vectoredHandler);
        ::SetUnhandledExceptionFilter(&unhandledSehFilter);
        std::set_terminate(&terminateHandler);
        ::signal(SIGABRT, &sigabrtHandler);

        Breadcrumb(L"CrashCapture.Installed", g_artifactDir);
    }

    void AttachXamlDiagnostics(winrt::Windows::Foundation::IInspectable const& appInsp)
    {
        namespace WUX = winrt::Windows::UI::Xaml;

        const auto app = appInsp.try_as<WUX::Application>();
        if (!app)
        {
            Breadcrumb(L"AttachXamlDiagnostics.Skipped", L"argument is not a Windows.UI.Xaml.Application");
            return;
        }

        // ---- Group 4: the ONLY prevention path. Routed XAML/WinRT exceptions
        // are caught here; Handled(true) lets the iteration survive. ----
        app.UnhandledException([](winrt::Windows::Foundation::IInspectable const&, WUX::UnhandledExceptionEventArgs const& e) {
            std::wstring detail = L"hresult=0x";
            detail += hex32(static_cast<uint32_t>(e.Exception()));
            detail += L" message=";
            detail += e.Message().c_str();

            const std::wstring restricted = captureRestrictedError();
            captureToArtifacts(L"XAML.UnhandledException", detail, restricted, nullptr);

            // Survive: keep the iteration alive so we can keep poking at drag.
            e.Handled(true);
        });

        auto dbg = app.DebugSettings();
        dbg.IsBindingTracingEnabled(true);
        dbg.FailFastOnErrors(false);
        // Note: this WUX (Windows.UI.Xaml) DebugSettings projection has no
        // IsXamlResourceReferenceTracingEnabled, and BindingFailedEventArgs lives
        // in the core Windows.UI.Xaml namespace (not ...::Data).
        dbg.BindingFailed([](winrt::Windows::Foundation::IInspectable const&, WUX::BindingFailedEventArgs const& e) {
            Breadcrumb(L"XAML.BindingFailed", e.Message().c_str());
        });

        Breadcrumb(L"AttachXamlDiagnostics.Attached", {});
    }

    void Breadcrumb(std::wstring_view event, std::wstring_view detail)
    {
        const std::wstring line = makeLine(event, detail);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_ring.push_back(line);
            if (g_ring.size() > kRingCap)
            {
                g_ring.pop_front();
            }
        }
        appendBreadcrumbLine(line);
    }

    std::wstring ArtifactDir()
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_artifactDir;
    }
}
