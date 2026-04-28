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
#include "TableModel.hh"
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

// Populate the library-level shared resources serialized in commit 2.
// Each user-created entry pairs with an explicit assertion in
// expectSharedResourcesRoundTripped below.
static void
populateSharedResources(LibertyLibrary *lib)
{
  // Bus declarations.
  lib->makeBusDcl("BUS8", 7, 0);
  lib->makeBusDcl("BUS16", 0, 15);

  // Operating conditions.
  OperatingConditions *slow = lib->makeOperatingConditions("slow");
  slow->setProcess(1.2F);
  slow->setVoltage(0.95F);
  slow->setTemperature(125.0F);
  slow->setWireloadTree(WireloadTree::worst_case);

  OperatingConditions *fast = lib->makeOperatingConditions("fast");
  fast->setProcess(0.8F);
  fast->setVoltage(1.10F);
  fast->setTemperature(-40.0F);
  fast->setWireloadTree(WireloadTree::best_case);
  lib->setDefaultOperatingConditions(slow);

  // Scale factors. Set a couple of distinguishable scale slots so the
  // round-trip is checked at a representative pair of indices, not
  // just at the all-zeros default.
  ScaleFactors *sf = lib->makeScaleFactors("typical_scales");
  sf->setScale(ScaleFactorType::cell,
               ScaleFactorPvt::process,
               RiseFall::rise(), 1.10F);
  sf->setScale(ScaleFactorType::cell,
               ScaleFactorPvt::volt,
               RiseFall::fall(), 0.95F);
  lib->setScaleFactors(sf);

  // Supply voltages.
  lib->addSupplyVoltage("VDD", 0.9F);
  lib->addSupplyVoltage("VDDH", 1.8F);

  // Table templates with axes. Constructing axes with explicit values
  // also exercises writeFloatArray / readFloatArray.
  TableTemplate *delay_tt = lib->makeTableTemplate("delay_2x3",
                                                   TableTemplateType::delay);
  FloatSeq slew_pts{ 0.01F, 0.05F, 0.20F };
  FloatSeq cap_pts { 0.005F, 0.050F };
  delay_tt->setAxis1(std::make_shared<TableAxis>(
      TableAxisVariable::input_net_transition, std::move(slew_pts)));
  delay_tt->setAxis2(std::make_shared<TableAxis>(
      TableAxisVariable::total_output_net_capacitance, std::move(cap_pts)));

  TableTemplate *power_tt = lib->makeTableTemplate("power_1d",
                                                   TableTemplateType::power);
  FloatSeq pwr_pts{ 0.0F, 0.5F, 1.0F, 1.5F };
  power_tt->setAxis1(std::make_shared<TableAxis>(
      TableAxisVariable::input_transition_time, std::move(pwr_pts)));
}

static void
expectSharedResourcesRoundTripped(LibertyLibrary *lib)
{
  // Bus declarations: confirmed via findBusDcl by name.
  ASSERT_NE(lib->findBusDcl("BUS8"), nullptr);
  EXPECT_EQ(lib->findBusDcl("BUS8")->from(), 7);
  EXPECT_EQ(lib->findBusDcl("BUS8")->to(),   0);
  ASSERT_NE(lib->findBusDcl("BUS16"), nullptr);
  EXPECT_EQ(lib->findBusDcl("BUS16")->from(), 0);
  EXPECT_EQ(lib->findBusDcl("BUS16")->to(),   15);

  // Operating conditions.
  OperatingConditions *slow = lib->findOperatingConditions("slow");
  ASSERT_NE(slow, nullptr);
  EXPECT_FLOAT_EQ(slow->process(),     1.2F);
  EXPECT_FLOAT_EQ(slow->voltage(),     0.95F);
  EXPECT_FLOAT_EQ(slow->temperature(), 125.0F);
  EXPECT_EQ(slow->wireloadTree(), WireloadTree::worst_case);

  OperatingConditions *fast = lib->findOperatingConditions("fast");
  ASSERT_NE(fast, nullptr);
  EXPECT_FLOAT_EQ(fast->voltage(), 1.10F);
  EXPECT_EQ(fast->wireloadTree(), WireloadTree::best_case);

  EXPECT_EQ(lib->defaultOperatingConditions(), slow);

  // Scale factors.
  ASSERT_NE(lib->scaleFactors(), nullptr);
  EXPECT_EQ(lib->scaleFactors()->name(), "typical_scales");
  EXPECT_FLOAT_EQ(lib->scaleFactors()->scale(ScaleFactorType::cell,
                                             ScaleFactorPvt::process,
                                             RiseFall::rise()),
                  1.10F);
  EXPECT_FLOAT_EQ(lib->scaleFactors()->scale(ScaleFactorType::cell,
                                             ScaleFactorPvt::volt,
                                             RiseFall::fall()),
                  0.95F);

  // Supply voltages.
  EXPECT_TRUE(lib->supplyExists("VDD"));
  EXPECT_TRUE(lib->supplyExists("VDDH"));
  float volt = 0;
  bool exists = false;
  lib->supplyVoltage("VDD", volt, exists);
  EXPECT_TRUE(exists);
  EXPECT_FLOAT_EQ(volt, 0.9F);
  lib->supplyVoltage("VDDH", volt, exists);
  EXPECT_TRUE(exists);
  EXPECT_FLOAT_EQ(volt, 1.8F);

  // Table templates: axes (variable + values) are the only field that
  // requires write/read float arrays, so this also validates that path.
  TableTemplate *delay_tt = lib->findTableTemplate("delay_2x3",
                                                   TableTemplateType::delay);
  ASSERT_NE(delay_tt, nullptr);
  ASSERT_NE(delay_tt->axis1(), nullptr);
  EXPECT_EQ(delay_tt->axis1()->variable(),
            TableAxisVariable::input_net_transition);
  ASSERT_EQ(delay_tt->axis1()->values().size(), 3u);
  EXPECT_FLOAT_EQ(delay_tt->axis1()->values()[2], 0.20F);
  ASSERT_NE(delay_tt->axis2(), nullptr);
  EXPECT_EQ(delay_tt->axis2()->variable(),
            TableAxisVariable::total_output_net_capacitance);
  ASSERT_EQ(delay_tt->axis2()->values().size(), 2u);
  EXPECT_FLOAT_EQ(delay_tt->axis2()->values()[1], 0.050F);
  EXPECT_EQ(delay_tt->axis3(), nullptr);

  TableTemplate *power_tt = lib->findTableTemplate("power_1d",
                                                   TableTemplateType::power);
  ASSERT_NE(power_tt, nullptr);
  ASSERT_NE(power_tt->axis1(), nullptr);
  ASSERT_EQ(power_tt->axis1()->values().size(), 4u);
  EXPECT_FLOAT_EQ(power_tt->axis1()->values()[3], 1.5F);
  EXPECT_EQ(power_tt->axis2(), nullptr);
}

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

// Populate two cells with distinct metadata. Functions, ports, and
// timing arcs are deliberately NOT touched here — those land in
// commits 3b/4. This commit's invariant is just "cell list survives".
static void
populateCellMetadata(LibertyLibrary *lib)
{
  // Plain combinational cell. The library expects each LibertyCell to
  // be registered via addCell (see LibertyBuilder::makeCell).
  LibertyCell *inv = new LibertyCell(lib, "INV", "inv.lib");
  lib->addCell(inv);
  inv->setArea(1.5F);
  inv->setDontUse(false);
  inv->setIsMacro(false);
  inv->setIsPad(false);
  inv->setLeakagePower(0.123F);
  inv->setFootprint("inv_footprint");
  inv->setUserFunctionClass("inverter_class");
  inv->setOcvArcDepth(0.0F);

  // Macro cell with several flags toggled and most string fields set.
  LibertyCell *macro = new LibertyCell(lib, "MEM_BIG", "mem.lib");
  lib->addCell(macro);
  macro->setArea(120.0F);
  macro->setDontUse(true);
  macro->setIsMacro(true);
  macro->setIsMemory(true);
  macro->setIsClockCell(false);
  macro->setIsLevelShifter(true);
  macro->setLevelShifterType(LevelShifterType::HL);
  macro->setIsIsolationCell(true);
  macro->setAlwaysOn(true);
  macro->setSwitchCellType(SwitchCellType::coarse_grain);
  macro->setInterfaceTiming(true);
  macro->setClockGateType(ClockGateType::other);
  macro->setHasInferedRegTimingArcs(true);
  macro->setOcvArcDepth(3.5F);
  macro->setFootprint("mem_footprint");
}

static void
expectCellMetadataRoundTripped(LibertyLibrary *lib)
{
  LibertyCell *inv = lib->findLibertyCell("INV");
  ASSERT_NE(inv, nullptr);
  EXPECT_FLOAT_EQ(inv->area(), 1.5F);
  EXPECT_FALSE(inv->dontUse());
  EXPECT_FALSE(inv->isMacro());
  EXPECT_TRUE(inv->leakagePowerExists());
  float leak = 0;
  bool exists = false;
  inv->leakagePower(leak, exists);
  EXPECT_TRUE(exists);
  EXPECT_FLOAT_EQ(leak, 0.123F);
  EXPECT_EQ(inv->footprint(), "inv_footprint");
  EXPECT_EQ(inv->userFunctionClass(), "inverter_class");
  EXPECT_EQ(inv->levelShifterType(), LevelShifterType::HL_LH);  // default
  EXPECT_FALSE(inv->isClockGate());

  LibertyCell *macro = lib->findLibertyCell("MEM_BIG");
  ASSERT_NE(macro, nullptr);
  EXPECT_FLOAT_EQ(macro->area(), 120.0F);
  EXPECT_TRUE(macro->dontUse());
  EXPECT_TRUE(macro->isMacro());
  EXPECT_TRUE(macro->isMemory());
  EXPECT_TRUE(macro->isLevelShifter());
  EXPECT_EQ(macro->levelShifterType(), LevelShifterType::HL);
  EXPECT_TRUE(macro->isIsolationCell());
  EXPECT_TRUE(macro->alwaysOn());
  EXPECT_EQ(macro->switchCellType(), SwitchCellType::coarse_grain);
  EXPECT_TRUE(macro->interfaceTiming());
  EXPECT_TRUE(macro->isClockGateOther());
  EXPECT_TRUE(macro->isClockGate());
  EXPECT_TRUE(macro->hasInferedRegTimingArcs());
  EXPECT_FLOAT_EQ(macro->ocvArcDepth(), 3.5F);
  EXPECT_EQ(macro->footprint(), "mem_footprint");
  // leakage was never set on this cell.
  EXPECT_FALSE(macro->leakagePowerExists());
}

TEST(LibertyCache, CellMetadataRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("cell_lib", ""));
  populateLibraryScalars(src.get());
  populateCellMetadata(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_cells.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectScalarsRoundTripped(dst.get());
  expectCellMetadataRoundTripped(dst.get());
}

TEST(LibertyCache, SharedResourcesRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("shared_lib", ""));
  populateLibraryScalars(src.get());
  populateSharedResources(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_shared.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectScalarsRoundTripped(dst.get());
  expectSharedResourcesRoundTripped(dst.get());
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
