// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Module Name:
// - CrashCapture.h
//
// Abstract:
// - A self-contained crash-diagnostic surface for iterating on MUX TabView
//   drag/drop without losing context to a silent process abort.
//
//   Drag/drop edits in this codebase can trip XAML invariant violations and CRT
//   run-time checks that fail-fast (__fastfail / RoFailFastWithErrorContext),
//   bypassing normal C++ exception handling. When that happens mid-iteration the
//   process dies and all context is lost. This component does three things, in
//   priority order for an *unattended* (agent-driven) run:
//
//     1. Suppresses the modal CRT / WER dialogs that otherwise HANG the run
//        forever. This is the single most important thing: a frozen modal box
//        is worse than a crash, because nothing can proceed past it.
//     2. Captures maximum context the instant something fails, to text-first
//        artifacts: crash.log (LLM-readable, headed sections, most-important
//        first) + a continuously-flushed breadcrumb log (the ONLY thing that
//        survives a true fail-fast, because no code runs after it) + a minidump
//        (binary fallback).
//     3. Catches what is actually catchable -- XAML/WinRT exceptions surfaced
//        through Application::UnhandledException -- and lets the iteration
//        survive (Handled = true).
//
//   What it CANNOT do: prevent a true fail-fast (0xC0000409 __fastfail). Those
//   bypass SetUnhandledExceptionFilter and generally the vectored handler too.
//   For them this component maximises *pre-crash* context (breadcrumbs are
//   already on disk) and relies on the suite's per-class process isolation to
//   keep the rest of the run alive.
//
//   Standalone by design: depends only on Win32 + CRT + C++/WinRT, no project
//   headers. It currently lives next to the drag rig; promoting it to a shared
//   location so the packaged dev app can install the same surface for live
//   smoke runs is a pure file-move.

#pragma once

#include <string>
#include <string_view>
#include <winrt/Windows.Foundation.h>

namespace TerminalApp::Diagnostics
{
    // Install the process-level diagnostic surface ONCE, as early as possible
    // (before any XAML or drag code runs). Idempotent; safe from any thread.
    // - artifactDir: directory for crash.log / breadcrumbs.log / *.dmp. Created
    //   if missing. Pass an absolute, writable path (e.g. under %TEMP%).
    void InstallCrashCapture(std::wstring_view artifactDir);

    // Attach the XAML-level surface: Application::UnhandledException (Handled =
    // true so the iteration survives) + DebugSettings binding/resource tracing.
    // Call AFTER the Application object exists, on the UI thread. `app` must be a
    // Windows.UI.Xaml.Application (passed as IInspectable to keep this header
    // free of XAML includes). A no-op if `app` is not an Application.
    void AttachXamlDiagnostics(winrt::Windows::Foundation::IInspectable const& app);

    // Append one breadcrumb line, flushed to disk immediately so it survives a
    // fail-fast. Cheap and thread-safe. Call around each drag/drop UI event
    // (DragStarting, DragOver accept/reject, Drop hit-test, DragCompleted, ...).
    void Breadcrumb(std::wstring_view event, std::wstring_view detail = {});

    // The artifact directory passed to InstallCrashCapture (empty until then).
    // Lets a test locate crash.log / breadcrumbs without re-deriving the path.
    std::wstring ArtifactDir();
}
