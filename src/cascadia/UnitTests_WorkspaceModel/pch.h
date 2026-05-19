// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Precompiled header for UnitTests_WorkspaceModel.
//
// This test project is intentionally winrt-free: the WorkspaceModel library
// under test is pure C++ and the tests must run without a WinRT apartment.
// We pull in only the C++ standard library and the TAEF test framework
// headers.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <WexTestClass.h>
