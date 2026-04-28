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

#include "LibertyCache.hh"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>

#include "Error.hh"
#include "Format.hh"
#include "Liberty.hh"
#include "LibertyCacheFormat.hh"
#include "Network.hh"
#include "StaConfig.hh"
#include "StaState.hh"
#include "Transition.hh"

namespace sta {

using cache::SectionId;

namespace {

void
readHeader(FILE *f,
           bool ignore_source_check,
           // Out: header fields the reader may need later.
           std::string &source_filename)
{
  uint32_t magic = cache::readU32(f);
  if (magic != cache::kMagic)
    cache::error(sta::format("liberty cache: bad magic 0x{:x} (expected 0x{:x})",
                             magic, cache::kMagic));
  uint32_t version = cache::readU32(f);
  if (version != cache::kFormatVersion)
    cache::error(sta::format(
        "liberty cache: format version {} not supported (this build expects {}); "
        "regenerate the cache with this version of STA.",
        version, cache::kFormatVersion));
  uint32_t endian = cache::readU32(f);
  if (endian != cache::kEndianSentinel)
    cache::error("liberty cache: endian mismatch (cache was written on a "
                 "host with the opposite byte order; not supported)");
  cache::readU32(f); // flags (reserved, ignored in v1)

  std::string sta_version = cache::readString(f);
  if (sta_version != STA_VERSION)
    cache::error(sta::format(
        "liberty cache: STA version mismatch (cache='{}', this build='{}'); "
        "regenerate the cache.",
        sta_version, STA_VERSION));

  source_filename = cache::readString(f);
  uint64_t source_size = cache::readU64(f);
  int64_t source_mtime = cache::readI64(f);

  if (ignore_source_check || source_filename.empty())
    return;

  // Compare the recorded (size, mtime) against the source file as it
  // exists now. Any discrepancy means the source has changed since
  // the cache was written; bail rather than serve potentially stale
  // timing data.
  std::error_code ec;
  std::filesystem::path path{source_filename};
  if (!std::filesystem::exists(path, ec))
    // Source missing is allowed when ignore_source_check is set;
    // here we treat the missing source as a hard error to surface
    // moved/renamed .lib mistakes.
    cache::error(sta::format(
        "liberty cache: source file '{}' is missing; pass "
        "-ignore_source_check to load anyway.", source_filename));

  uint64_t cur_size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  auto ftime = std::filesystem::last_write_time(path, ec);
  int64_t cur_mtime = std::chrono::duration_cast<std::chrono::seconds>(
                        ftime.time_since_epoch()).count();
  if (cur_size != source_size || cur_mtime != source_mtime)
    cache::error(sta::format(
        "liberty cache: source file '{}' has changed since the cache was "
        "written (size {}->{}, mtime {}->{}); regenerate the cache or pass "
        "-ignore_source_check.",
        source_filename, source_size, cur_size, source_mtime, cur_mtime));
}

void
readLibraryHeader(FILE *f,
                  // Out: extracted from the section.
                  std::string &name,
                  std::string &filename,
                  DelayModelType &delay_model)
{
  cache::expectSectionId(f, SectionId::LibraryHeader);
  name = cache::readString(f);
  filename = cache::readString(f);
  delay_model = static_cast<DelayModelType>(cache::readU32(f));
}

void
readLibraryScalars(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::LibraryScalars);

  lib->setNominalProcess(cache::readFloat(f));
  lib->setNominalVoltage(cache::readFloat(f));
  lib->setNominalTemperature(cache::readFloat(f));
  lib->setOcvArcDepth(cache::readFloat(f));

  lib->setDefaultInputPinCap(cache::readFloat(f));
  lib->setDefaultOutputPinCap(cache::readFloat(f));
  lib->setDefaultBidirectPinCap(cache::readFloat(f));
  lib->setSlewDerateFromLibrary(cache::readFloat(f));

  for (auto rf : RiseFall::range())
    lib->setInputThreshold(rf, cache::readFloat(f));
  for (auto rf : RiseFall::range())
    lib->setOutputThreshold(rf, cache::readFloat(f));
  for (auto rf : RiseFall::range())
    lib->setSlewLowerThreshold(rf, cache::readFloat(f));
  for (auto rf : RiseFall::range())
    lib->setSlewUpperThreshold(rf, cache::readFloat(f));

  // (value, exists) pairs are only restored when exists is true so a
  // round-trip from a library where the field was never set leaves the
  // loaded library's "exists" bit false (matching the source).
  for (auto rf : RiseFall::range()) {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists)
      lib->setDefaultIntrinsic(rf, value);
  }
  for (auto rf : RiseFall::range()) {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists)
      lib->setDefaultBidirectPinRes(rf, value);
  }
  for (auto rf : RiseFall::range()) {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists)
      lib->setDefaultOutputPinRes(rf, value);
  }

  {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists) lib->setDefaultFanoutLoad(value);
  }
  {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists) lib->setDefaultMaxCapacitance(value);
  }
  {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists) lib->setDefaultMaxFanout(value);
  }
  {
    float value = cache::readFloat(f);
    bool exists = cache::readBool(f);
    if (exists) lib->setDefaultMaxSlew(value);
  }
}

} // namespace

LibertyLibrary *
readLibertyCache(const char *filename,
                 bool ignore_source_check,
                 StaState *sta)
{
  std::unique_ptr<FILE, int(*)(FILE*)> in(fopen(filename, "rb"), &fclose);
  if (!in)
    throw FileNotReadable(filename);

  FILE *f = in.get();
  std::string source_filename;
  readHeader(f, ignore_source_check, source_filename);

  std::string name;
  std::string lib_filename;
  DelayModelType delay_model;
  readLibraryHeader(f, name, lib_filename, delay_model);

  Network *network = sta->networkReader();
  LibertyLibrary *lib = network
      ? network->makeLibertyLibrary(name, lib_filename)
      : new LibertyLibrary(name, lib_filename);
  lib->setDelayModelType(delay_model);

  readLibraryScalars(f, lib);

  cache::expectSectionId(f, SectionId::EndMarker);
  return lib;
}

} // namespace sta
