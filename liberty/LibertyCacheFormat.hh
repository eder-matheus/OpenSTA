// OpenSTA, Static Timing Analyzer
// Copyright (c) 2026, Parallax Software, Inc.
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
//
// The origin of this software must not be misrepresented; you must not
// claim that you wrote the original software.
//
// Altered source versions must be plainly marked as such, and must not be
// misrepresented as being the original software.
//
// This notice may not be removed or altered from any source distribution.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "LibertyCache.hh"  // for LibertyCacheFormatError

namespace sta {

namespace cache {

// 'S','T','A','C' as little-endian u32. Identifies a Liberty cache file.
inline constexpr uint32_t kMagic = 0x43415453;

// Bumped whenever the on-disk layout changes in a way that would
// confuse an older reader. Mismatches are fatal — readers do not
// attempt to read older versions; users must regenerate the cache.
inline constexpr uint32_t kFormatVersion = 1;

// Endian sentinel: writer always stores 0x12345678 in native order;
// the reader checks bit-for-bit equality. A mismatch indicates the
// cache was written on a host with the opposite endianness, which
// the format does not support.
inline constexpr uint32_t kEndianSentinel = 0x12345678;

// Section identifiers. Stored as the first u32 of each section to
// allow forward-compatible skipping if we ever introduce optional
// sections. Today, every reader expects every writer-emitted section
// in order.
enum class SectionId : uint32_t {
  LibraryHeader      = 0x01,
  LibraryScalars     = 0x02,
  BusDcls            = 0x10,
  OperatingConditions = 0x11,
  ScaleFactors       = 0x12,
  SupplyVoltages     = 0x13,
  TableTemplates     = 0x14,
  OcvDerates         = 0x15,
  DriverWaveforms    = 0x16,
  Cells              = 0x20,
  EndMarker          = 0xFF,
};

// Header flags. Reserved for future use (e.g., "compressed body",
// "contains hash of source"). Always 0 in v1.
inline constexpr uint32_t kFlagsNone = 0;

// === Binary I/O primitives ============================================
// All primitives:
//   * write fixed-width little-endian fields directly via fread/fwrite
//     (we error on big-endian via the endian sentinel rather than
//     supporting byte swap).
//   * throw on short reads/writes via the cache::error helper below.
// Strings are length-prefixed by a u32 byte count.

[[noreturn]] void error(const std::string &msg);

void writeU32(FILE *f, uint32_t v);
uint32_t readU32(FILE *f);

void writeU64(FILE *f, uint64_t v);
uint64_t readU64(FILE *f);

void writeI64(FILE *f, int64_t v);
int64_t readI64(FILE *f);

void writeFloat(FILE *f, float v);
float readFloat(FILE *f);

void writeBool(FILE *f, bool v);
bool readBool(FILE *f);

void writeString(FILE *f, std::string_view s);
std::string readString(FILE *f);

// Length-prefixed float array. The raw bytes are streamed as a single
// fwrite/fread for cache-friendly throughput on large tables.
void writeFloatArray(FILE *f, const float *data, size_t n);
std::vector<float> readFloatArray(FILE *f);

// Section helpers: read/verify or write the section ID at the head
// of a section. Throw LibertyCacheFormatError on mismatch.
void writeSectionId(FILE *f, SectionId id);
void expectSectionId(FILE *f, SectionId expected);

} // namespace cache
} // namespace sta
