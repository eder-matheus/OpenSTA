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

#include "LibertyCacheFormat.hh"

#include <cstring>

#include "Format.hh"

namespace sta::cache {

[[noreturn]] void
error(const std::string &msg)
{
  throw LibertyCacheFormatError(msg);
}

static void
writeRaw(FILE *f, const void *data, size_t n)
{
  if (fwrite(data, 1, n, f) != n)
    error("liberty cache: write failed");
}

static void
readRaw(FILE *f, void *data, size_t n)
{
  if (fread(data, 1, n, f) != n)
    error("liberty cache: short read or truncated file");
}

void writeU32(FILE *f, uint32_t v) { writeRaw(f, &v, sizeof v); }

uint32_t
readU32(FILE *f)
{
  uint32_t v;
  readRaw(f, &v, sizeof v);
  return v;
}

void writeU64(FILE *f, uint64_t v) { writeRaw(f, &v, sizeof v); }

uint64_t
readU64(FILE *f)
{
  uint64_t v;
  readRaw(f, &v, sizeof v);
  return v;
}

void writeI64(FILE *f, int64_t v) { writeRaw(f, &v, sizeof v); }

int64_t
readI64(FILE *f)
{
  int64_t v;
  readRaw(f, &v, sizeof v);
  return v;
}

void writeFloat(FILE *f, float v) { writeRaw(f, &v, sizeof v); }

float
readFloat(FILE *f)
{
  float v;
  readRaw(f, &v, sizeof v);
  return v;
}

void
writeBool(FILE *f, bool v)
{
  // Stored as u8 to keep the on-disk layout deterministic regardless
  // of sizeof(bool) variation across ABIs.
  uint8_t b = v ? 1 : 0;
  writeRaw(f, &b, 1);
}

bool
readBool(FILE *f)
{
  uint8_t b;
  readRaw(f, &b, 1);
  return b != 0;
}

void
writeString(FILE *f, std::string_view s)
{
  writeU32(f, static_cast<uint32_t>(s.size()));
  if (!s.empty())
    writeRaw(f, s.data(), s.size());
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

void
writeSectionId(FILE *f, SectionId id)
{
  writeU32(f, static_cast<uint32_t>(id));
}

void
expectSectionId(FILE *f, SectionId expected)
{
  uint32_t got = readU32(f);
  if (got != static_cast<uint32_t>(expected))
    error(sta::format("liberty cache: expected section {} but got {}",
                      static_cast<uint32_t>(expected), got));
}

} // namespace sta::cache
