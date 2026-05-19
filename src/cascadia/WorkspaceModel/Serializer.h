// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Structural snapshot serializer for the WorkspaceModel.
//
// The snapshot format is a single JSON object that mirrors the
// WorkspaceModelData shape directly. It carries a schemaVersion at the top
// level so future shape changes can be detected on load and rejected with
// a clear error.
//
// Round-trip property: for any well-formed WorkspaceModelData s,
//   fromJson(toJson(s)) == s
// EXCEPT that every TabRecord.mount is reset to std::nullopt on the
// deserialized side -- ContentId values are runtime-only handles and never
// participate in persistence. Callers that need to remount must do so via
// the (future) ContentRegistry; the on-disk shape never carries a mount.
//
// Pure C++: no winrt::*, no Windows.h.

#pragma once

#include <json/json.h>

#include <stdexcept>
#include <string>

#include "WorkspaceState.h"

namespace WorkspaceModel
{
    // The on-disk schema version. Bump when the persisted shape changes in a
    // way that's not backwards-compatible. Slice 4 introduces version 1.
    inline constexpr int kSchemaVersion = 1;

    // Thrown by fromJson() when the input JSON does not match the expected
    // schema (wrong version, missing required field, malformed structure).
    // Wraps the JsonCpp parse-error message when the underlying issue is a
    // parse failure.
    struct SerializerError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // Convert an in-memory WorkspaceModelData to its on-disk JSON form.
    // Always succeeds; the result always carries a schemaVersion field
    // equal to kSchemaVersion.
    [[nodiscard]] Json::Value toJson(const WorkspaceModelData& s);

    // Convert a JSON snapshot back to an in-memory WorkspaceModelData.
    // Throws SerializerError on any schema mismatch or missing required
    // field. The returned model satisfies the same invariants as the
    // original (the validator can be run against the result; this function
    // does not call the validator itself -- it is the caller's job to do
    // so if they want post-load validation).
    [[nodiscard]] WorkspaceModelData fromJson(const Json::Value& j);

    // Parse a JSON-text blob, then deserialize it. Convenience for the
    // common "read state.json from disk" path. Throws SerializerError on
    // both parse and schema failures.
    [[nodiscard]] WorkspaceModelData parseFromString(const std::string& jsonText);

    // Serialize to a compact (one-line) JSON string. Useful for the
    // actions.log writer where each entry is one line of JSON.
    [[nodiscard]] std::string writeCompact(const Json::Value& v);

    // Serialize to a pretty-printed JSON string. Useful for the human-
    // readable state.json snapshot.
    [[nodiscard]] std::string writePretty(const Json::Value& v);

    // Parse a JSON-text blob into a Json::Value. Throws SerializerError on
    // parse failure (wrapping the underlying JsonCpp message).
    [[nodiscard]] Json::Value parseJson(const std::string& text);

    // -------------------- Shared field helpers --------------------
    //
    // Exposed for use by ActionLog (op-record JSON encoding) so the WAL and
    // the snapshot share one canonical form for every primitive field.
    // None of these should appear in user-facing callers; they're an
    // implementation detail of the persistence layer.
    namespace detail
    {
        [[nodiscard]] Json::Value optColorToJson(const std::optional<Color>& c);
        [[nodiscard]] std::optional<Color> optColorFromJson(const Json::Value& v);

        [[nodiscard]] Json::Value tabContentToJson(const TabContent& tc);
        [[nodiscard]] TabContent tabContentFromJson(const Json::Value& v);

        [[nodiscard]] const Json::Value& require(const Json::Value& obj, const char* key);
        [[nodiscard]] std::string requireString(const Json::Value& obj, const char* key);
        [[nodiscard]] bool requireBool(const Json::Value& obj, const char* key);
        [[nodiscard]] double requireDouble(const Json::Value& obj, const char* key);
        [[nodiscard]] std::uint64_t requireUInt64(const Json::Value& obj, const char* key);

        [[noreturn]] void fail(const std::string& msg);
    }
}
