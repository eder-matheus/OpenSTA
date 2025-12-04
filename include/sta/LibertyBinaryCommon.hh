// OpenSTA, Static Timing Analyzer
// Copyright (c) 2025, Parallax Software, Inc.
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstdint>

namespace sta {

// Magic number for binary liberty files: "STALIB01"
static const char *LIBERTY_BINARY_MAGIC = "STALIB01";

enum class LibertyBinaryTag : uint8_t {
  GROUP_BEGIN = 1,
  GROUP_END = 2,
  ATTR_SIMPLE = 3,
  ATTR_COMPLEX = 4,
  DEFINE = 5, // Unlikely to be used if writing from semantic model, but good to have
  VARIABLE = 6,
  EOF_TAG = 0
};

enum class LibertyBinaryValueType : uint8_t {
  STRING = 1,
  FLOAT = 2,
  INT = 3,
  BOOLEAN = 4
};

} // namespace sta
