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

#include "LibertyParseCacheFormat.hh"

#include <cstring>
#include <stdexcept>

#include "LibertyParseCache.hh"

namespace sta {

namespace lpc {

void
error(const std::string &msg)
{
  throw LpcFormatError(msg);
}

static void
writeRaw(FILE *f, const void *data, size_t n)
{
  if (fwrite(data, 1, n, f) != n)
    error("ldb-parse: write failed");
}

static void
readRaw(FILE *f, void *data, size_t n)
{
  if (fread(data, 1, n, f) != n)
    error("ldb-parse: short read or truncated file");
}

void writeBytes(FILE *f, const void *data, size_t n) { writeRaw(f, data, n); }
void readBytes(FILE *f, void *data, size_t n) { readRaw(f, data, n); }

void writeU8(FILE *f, uint8_t v) { writeRaw(f, &v, 1); }
uint8_t readU8(FILE *f) { uint8_t v; readRaw(f, &v, 1); return v; }

void writeU32(FILE *f, uint32_t v) { writeRaw(f, &v, 4); }
uint32_t readU32(FILE *f) { uint32_t v; readRaw(f, &v, 4); return v; }

void writeI32(FILE *f, int32_t v) { writeRaw(f, &v, 4); }
int32_t readI32(FILE *f) { int32_t v; readRaw(f, &v, 4); return v; }

void writeU64(FILE *f, uint64_t v) { writeRaw(f, &v, 8); }
uint64_t readU64(FILE *f) { uint64_t v; readRaw(f, &v, 8); return v; }

void writeI64(FILE *f, int64_t v) { writeRaw(f, &v, 8); }
int64_t readI64(FILE *f) { int64_t v; readRaw(f, &v, 8); return v; }

void writeFloat(FILE *f, float v) { writeRaw(f, &v, 4); }
float readFloat(FILE *f) { float v; readRaw(f, &v, 4); return v; }

void
writeString(FILE *f, std::string_view s)
{
  uint32_t n = static_cast<uint32_t>(s.size());
  writeU32(f, n);
  if (n > 0)
    writeRaw(f, s.data(), n);
}

std::string
readString(FILE *f)
{
  uint32_t n = readU32(f);
  std::string s(n, '\0');
  if (n > 0)
    readRaw(f, s.data(), n);
  return s;
}

} // namespace lpc

} // namespace sta
