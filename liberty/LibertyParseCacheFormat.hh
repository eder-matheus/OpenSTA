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

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace sta {

namespace lpc {

// 'O','S','L','P' as little-endian u32. Identifies an LPC (Liberty
// Parse Cache) file -- a serialized stream of LibertyGroupVisitor
// events captured during a normal read_liberty parse.
inline constexpr uint32_t kMagic = 0x504C534F;

// Bumped whenever the on-disk layout changes. Mismatches are fatal.
inline constexpr uint32_t kFormatVersion = 1;

// Endian sentinel: 0x12345678 stored in native byte order. Reader
// checks bit-for-bit equality; mismatch means the file was written
// on a host with the opposite endianness.
inline constexpr uint32_t kEndianSentinel = 0x12345678;

// Header flags. Reserved for future use.
inline constexpr uint32_t kFlagsNone = 0;

// Event tags. Stored as a u8 prefix on every event record.
enum class EventTag : uint8_t {
  kEnd            = 0x00,  // end of current group (no payload)
  kBeginGroup     = 0x01,  // (type:str_id, params:AttrValueSeq, line:i32)
  kSimpleAttr     = 0x02,  // (name:str_id, value:AttrValue, line:i32)
  kComplexAttr    = 0x03,  // (name:str_id, values:AttrValueSeq, line:i32)
  kVariable       = 0x04,  // (name:str_id, value:float, line:i32)
  kStringPoolNew  = 0x05,  // (string:bytes) new string interned; assigned next id
  kEof            = 0xFF,  // end of stream marker
};

// AttrValue is a tagged union (string-interned-id or float).
enum class AttrValueTag : uint8_t {
  kFloat  = 0x00,
  kString = 0x01,  // string id into the inline pool
};

// === Binary I/O primitives =========================================
// All primitives:
//   * write fixed-width little-endian fields directly via fread/fwrite
//     (we error on big-endian via the endian sentinel).
//   * throw on short reads/writes via the lpc::error helper below.
// Strings are length-prefixed by a u32 byte count.

[[noreturn]] void error(const std::string &msg);

void writeU8(FILE *f, uint8_t v);
uint8_t readU8(FILE *f);

void writeU32(FILE *f, uint32_t v);
uint32_t readU32(FILE *f);

void writeI32(FILE *f, int32_t v);
int32_t readI32(FILE *f);

void writeU64(FILE *f, uint64_t v);
uint64_t readU64(FILE *f);

void writeI64(FILE *f, int64_t v);
int64_t readI64(FILE *f);

void writeFloat(FILE *f, float v);
float readFloat(FILE *f);

void writeBytes(FILE *f, const void *data, size_t n);
void readBytes(FILE *f, void *data, size_t n);

void writeString(FILE *f, std::string_view s);
std::string readString(FILE *f);

// Tag helpers.
inline void writeTag(FILE *f, EventTag t) { writeU8(f, static_cast<uint8_t>(t)); }
inline EventTag readTag(FILE *f) { return static_cast<EventTag>(readU8(f)); }
inline void writeAttrValueTag(FILE *f, AttrValueTag t) { writeU8(f, static_cast<uint8_t>(t)); }
inline AttrValueTag readAttrValueTag(FILE *f) { return static_cast<AttrValueTag>(readU8(f)); }

} // namespace lpc

} // namespace sta
