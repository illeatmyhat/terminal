// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Precompiled header for the WorkspaceModel static library.
//
// Intentionally narrow: this header pulls in only standard C++ facilities.
// The WorkspaceModel lib must remain free of winrt::*, Windows.h, and any
// other Windows-specific dependency so its tests (fuzzer, DSL-driven
// behavioural tests) can build with no WinRT apartment.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <json/json.h>
