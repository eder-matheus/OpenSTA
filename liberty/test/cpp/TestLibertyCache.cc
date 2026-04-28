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

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Liberty.hh"
#include "Transition.hh"

namespace sta {

// Lifetime guard for a temporary cache file: deletes the file in the
// destructor so a single failing test doesn't pollute /tmp.
class TempCachePath
{
public:
  explicit TempCachePath(const std::string &name) : path_(name) {}
  ~TempCachePath() { std::filesystem::remove(path_); }
  const char *c_str() const { return path_.c_str(); }
  const std::string &str() const { return path_; }

private:
  std::string path_;
};

// Populate every scalar the v1 cache writer/reader handles. Round-trip
// tests then verify each one survived intact.
static void
populateLibraryScalars(LibertyLibrary *lib)
{
  lib->setDelayModelType(DelayModelType::table);
  lib->setNominalProcess(1.5F);
  lib->setNominalVoltage(1.05F);
  lib->setNominalTemperature(85.0F);
  lib->setOcvArcDepth(2.0F);

  lib->setDefaultInputPinCap(0.001F);
  lib->setDefaultOutputPinCap(0.002F);
  lib->setDefaultBidirectPinCap(0.003F);
  lib->setSlewDerateFromLibrary(0.5F);

  lib->setInputThreshold(RiseFall::rise(),  0.4F);
  lib->setInputThreshold(RiseFall::fall(),  0.6F);
  lib->setOutputThreshold(RiseFall::rise(), 0.45F);
  lib->setOutputThreshold(RiseFall::fall(), 0.55F);
  lib->setSlewLowerThreshold(RiseFall::rise(), 0.1F);
  lib->setSlewLowerThreshold(RiseFall::fall(), 0.15F);
  lib->setSlewUpperThreshold(RiseFall::rise(), 0.85F);
  lib->setSlewUpperThreshold(RiseFall::fall(), 0.9F);

  lib->setDefaultIntrinsic(RiseFall::rise(), 0.012F);
  lib->setDefaultIntrinsic(RiseFall::fall(), 0.014F);
  lib->setDefaultBidirectPinRes(RiseFall::rise(), 100.0F);
  lib->setDefaultBidirectPinRes(RiseFall::fall(), 110.0F);
  lib->setDefaultOutputPinRes(RiseFall::rise(), 200.0F);
  lib->setDefaultOutputPinRes(RiseFall::fall(), 220.0F);

  lib->setDefaultFanoutLoad(7.0F);
  lib->setDefaultMaxCapacitance(1.0F);
  lib->setDefaultMaxFanout(32.0F);
  lib->setDefaultMaxSlew(0.5F);
}

#define EXPECT_RFV_EQ(getter, value_rise, value_fall)                  \
  do {                                                                  \
    float v;                                                            \
    bool e;                                                             \
    getter(RiseFall::rise(), v, e);                                     \
    EXPECT_TRUE(e);                                                     \
    EXPECT_FLOAT_EQ(v, value_rise);                                     \
    getter(RiseFall::fall(), v, e);                                     \
    EXPECT_TRUE(e);                                                     \
    EXPECT_FLOAT_EQ(v, value_fall);                                     \
  } while (0)

#define EXPECT_PAIR_EQ(getter, expected)                                \
  do {                                                                  \
    float v;                                                            \
    bool e;                                                             \
    lib->getter(v, e);                                                  \
    EXPECT_TRUE(e);                                                     \
    EXPECT_FLOAT_EQ(v, expected);                                       \
  } while (0)

static void
expectScalarsRoundTripped(LibertyLibrary *lib)
{
  EXPECT_EQ(lib->delayModelType(), DelayModelType::table);
  EXPECT_FLOAT_EQ(lib->nominalProcess(),     1.5F);
  EXPECT_FLOAT_EQ(lib->nominalVoltage(),     1.05F);
  EXPECT_FLOAT_EQ(lib->nominalTemperature(), 85.0F);
  EXPECT_FLOAT_EQ(lib->ocvArcDepth(),        2.0F);

  EXPECT_FLOAT_EQ(lib->defaultInputPinCap(),     0.001F);
  EXPECT_FLOAT_EQ(lib->defaultOutputPinCap(),    0.002F);
  EXPECT_FLOAT_EQ(lib->defaultBidirectPinCap(),  0.003F);
  EXPECT_FLOAT_EQ(lib->slewDerateFromLibrary(),  0.5F);

  EXPECT_FLOAT_EQ(lib->inputThreshold(RiseFall::rise()),     0.4F);
  EXPECT_FLOAT_EQ(lib->inputThreshold(RiseFall::fall()),     0.6F);
  EXPECT_FLOAT_EQ(lib->outputThreshold(RiseFall::rise()),    0.45F);
  EXPECT_FLOAT_EQ(lib->outputThreshold(RiseFall::fall()),    0.55F);
  EXPECT_FLOAT_EQ(lib->slewLowerThreshold(RiseFall::rise()), 0.1F);
  EXPECT_FLOAT_EQ(lib->slewLowerThreshold(RiseFall::fall()), 0.15F);
  EXPECT_FLOAT_EQ(lib->slewUpperThreshold(RiseFall::rise()), 0.85F);
  EXPECT_FLOAT_EQ(lib->slewUpperThreshold(RiseFall::fall()), 0.9F);

  auto member_intrinsic = [&](const RiseFall *rf, float &v, bool &e) {
    lib->defaultIntrinsic(rf, v, e);
  };
  auto member_bidirect = [&](const RiseFall *rf, float &v, bool &e) {
    lib->defaultBidirectPinRes(rf, v, e);
  };
  auto member_output = [&](const RiseFall *rf, float &v, bool &e) {
    lib->defaultOutputPinRes(rf, v, e);
  };
  EXPECT_RFV_EQ(member_intrinsic, 0.012F, 0.014F);
  EXPECT_RFV_EQ(member_bidirect, 100.0F, 110.0F);
  EXPECT_RFV_EQ(member_output,   200.0F, 220.0F);

  EXPECT_PAIR_EQ(defaultFanoutLoad,    7.0F);
  EXPECT_PAIR_EQ(defaultMaxCapacitance, 1.0F);
  EXPECT_PAIR_EQ(defaultMaxFanout,     32.0F);
  EXPECT_PAIR_EQ(defaultMaxSlew,        0.5F);
}

#undef EXPECT_RFV_EQ
#undef EXPECT_PAIR_EQ

TEST(LibertyCache, ScalarsRoundTrip)
{
  // Empty filename keeps the writer from stamping a real source path,
  // so the source-staleness check on read is naturally a no-op.
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("test_lib", ""));
  populateLibraryScalars(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_scalars.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), /*sta=*/nullptr);

  // Read into a fresh library. ignore_source_check is irrelevant when
  // the source filename is empty, but pass true to document intent.
  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(),
                       /*ignore_source_check=*/true,
                       /*sta=*/nullptr));
  ASSERT_NE(dst.get(), nullptr);

  EXPECT_EQ(dst->name(), "test_lib");
  expectScalarsRoundTripped(dst.get());
}

TEST(LibertyCache, RejectsBadMagic)
{
  TempCachePath path("/tmp/sta_libcache_bad_magic.cache");
  // Write 4 bytes of nonsense as the magic field.
  std::unique_ptr<FILE, int(*)(FILE*)> f(fopen(path.c_str(), "wb"), &fclose);
  ASSERT_NE(f.get(), nullptr);
  uint32_t bad_magic = 0xDEADBEEFu;
  fwrite(&bad_magic, sizeof bad_magic, 1, f.get());
  f.reset();

  EXPECT_THROW(readLibertyCache(path.c_str(), true, nullptr),
               LibertyCacheFormatError);
}

} // namespace sta
