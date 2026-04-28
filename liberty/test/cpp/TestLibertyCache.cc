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

#include "FuncExpr.hh"
#include "InternalPower.hh"
#include "LeakagePower.hh"
#include "Liberty.hh"
#include "liberty/LibertyBuilder.hh"
#include "NetworkClass.hh"
#include "PortDirection.hh"
#include "Sequential.hh"
#include "TableModel.hh"
#include "TimingArc.hh"
#include "TimingModel.hh"
#include "TimingRole.hh"
#include "Transition.hh"

namespace sta {

// PortDirection::find() depends on the static singleton table being
// populated; initSta() does this in normal flows but the gtest binary
// runs each test in isolation via --gtest_filter, so we initialize on
// first use here.
class PortDirectionInit
{
public:
  PortDirectionInit()
  {
    static bool done = false;
    if (!done) {
      PortDirection::init();
      done = true;
    }
  }
};

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

// Build a 3-port AND cell with assorted port-level state. Used to
// exercise the new (commit 3b) per-cell port format including the
// FuncExpr serializer.
static void
populateCellWithPorts(LibertyLibrary *lib)
{
  PortDirectionInit pd_init;
  LibertyBuilder builder(/*debug=*/nullptr, /*report=*/nullptr);
  LibertyCell *cell = builder.makeCell(lib, "AND2", "and.lib");
  cell->setArea(2.0F);

  LibertyPort *a = builder.makePort(cell, "A");
  LibertyPort *b = builder.makePort(cell, "B");
  LibertyPort *y = builder.makePort(cell, "Y");

  PortDirection *in_dir = PortDirection::find("input");
  PortDirection *out_dir = PortDirection::find("output");
  ASSERT_NE(in_dir, nullptr);
  ASSERT_NE(out_dir, nullptr);
  a->setDirection(in_dir);
  b->setDirection(in_dir);
  y->setDirection(out_dir);

  // Capacitance with distinct rise/fall × min/max slots.
  a->setCapacitance(RiseFall::rise(), MinMax::min(), 0.0010F);
  a->setCapacitance(RiseFall::rise(), MinMax::max(), 0.0020F);
  a->setCapacitance(RiseFall::fall(), MinMax::min(), 0.0015F);
  a->setCapacitance(RiseFall::fall(), MinMax::max(), 0.0025F);
  b->setCapacitance(0.005F);  // single-value: all four slots = 0.005
  a->setFanoutLoad(1.5F);

  // Limits.
  y->setSlewLimit(0.5F, MinMax::max());
  y->setCapacitanceLimit(2.0F, MinMax::max());
  y->setFanoutLimit(16.0F, MinMax::max());

  // Sequential-ish flags on B + a min_period for variety.
  b->setIsClock(true);
  b->setIsRegClk(true);
  b->setMinPeriod(1.0F);
  // Only set min_pulse_width on rise. Setting both rise and fall trips
  // a pre-existing LibertyPort bug: min_pulse_width_exists_ is declared
  // `bool : 2` but the code uses it as a 2-bit bitmask, which C++
  // bool bit-fields don't support (writes collapse to 0/1, then the
  // read-side `& (1<<index)` mis-reports the second slot). Out of
  // scope to fix here; round-trip works for either edge alone.
  b->setMinPulseWidth(RiseFall::rise(), 0.4F);
  b->setPulseClk(RiseFall::rise(), RiseFall::fall());
  b->setVoltageName("VDD");
  b->setScanSignalType(ScanSignalType::clock);

  // Function: Y = A AND B. A,B ports need to exist before we wire
  // the FuncExpr — they do, via the makePort calls above.
  y->setFunction(FuncExpr::makeAnd(FuncExpr::makePort(a),
                                   FuncExpr::makePort(b)));
  // Tristate enable: simple A.
  y->setTristateEnable(FuncExpr::makePort(a));
}

static void
expectCellWithPortsRoundTripped(LibertyLibrary *lib)
{
  LibertyCell *cell = lib->findLibertyCell("AND2");
  ASSERT_NE(cell, nullptr);
  EXPECT_FLOAT_EQ(cell->area(), 2.0F);

  LibertyPort *a = cell->findLibertyPort("A");
  LibertyPort *b = cell->findLibertyPort("B");
  LibertyPort *y = cell->findLibertyPort("Y");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(y, nullptr);

  // Direction.
  ASSERT_NE(a->direction(), nullptr);
  EXPECT_EQ(a->direction()->name(), "input");
  ASSERT_NE(y->direction(), nullptr);
  EXPECT_EQ(y->direction()->name(), "output");

  // Capacitance.
  float val;
  bool exists;
  a->capacitance(RiseFall::rise(), MinMax::min(), val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 0.0010F);
  a->capacitance(RiseFall::fall(), MinMax::max(), val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 0.0025F);

  // Limits.
  y->slewLimit(MinMax::max(), val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 0.5F);
  y->capacitanceLimit(MinMax::max(), val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 2.0F);
  y->fanoutLimit(MinMax::max(), val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 16.0F);

  a->fanoutLoad(val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 1.5F);

  // Flags + per-port scalars on B.
  EXPECT_TRUE(b->isClock());
  EXPECT_TRUE(b->isRegClk());
  b->minPeriod(val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 1.0F);
  b->minPulseWidth(RiseFall::rise(), val, exists);
  EXPECT_TRUE(exists); EXPECT_FLOAT_EQ(val, 0.4F);
  // (fall slot intentionally not asserted — see populateCellWithPorts)
  EXPECT_EQ(b->pulseClkTrigger(), RiseFall::rise());
  EXPECT_EQ(b->pulseClkSense(),   RiseFall::fall());
  EXPECT_EQ(b->voltageName(), "VDD");
  EXPECT_EQ(b->scanSignalType(), ScanSignalType::clock);

  // FuncExpr: Y = A AND B. Verify structure rather than re-comparing
  // pointers (the loaded ports are different LibertyPort instances).
  FuncExpr *func = y->function();
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(func->op(), FuncExpr::Op::and_);
  ASSERT_NE(func->left(), nullptr);
  ASSERT_NE(func->right(), nullptr);
  EXPECT_EQ(func->left()->op(),  FuncExpr::Op::port);
  EXPECT_EQ(func->right()->op(), FuncExpr::Op::port);
  EXPECT_EQ(func->left()->port(),  a);
  EXPECT_EQ(func->right()->port(), b);

  FuncExpr *tri = y->tristateEnable();
  ASSERT_NE(tri, nullptr);
  EXPECT_EQ(tri->op(), FuncExpr::Op::port);
  EXPECT_EQ(tri->port(), a);
}

// Exercise the structural pieces added in commit 3c: per-cell
// BusDcl, ModeDef (with cond + sdf_cond), and Sequential.
static void
populateSequentialCell(LibertyLibrary *lib)
{
  PortDirectionInit pd_init;
  LibertyBuilder builder(/*debug=*/nullptr, /*report=*/nullptr);
  LibertyCell *cell = builder.makeCell(lib, "DFF", "dff.lib");

  PortDirection *in_dir = PortDirection::find("input");
  PortDirection *out_dir = PortDirection::find("output");
  LibertyPort *d   = builder.makePort(cell, "D");
  LibertyPort *clk = builder.makePort(cell, "CLK");
  LibertyPort *clr = builder.makePort(cell, "CLR");
  LibertyPort *q   = builder.makePort(cell, "Q");
  d->setDirection(in_dir);
  clk->setDirection(in_dir);
  clr->setDirection(in_dir);
  q->setDirection(out_dir);

  // Per-cell bus_dcl.
  cell->makeBusDcl("LOCAL_BUS", 0, 3);

  // Per-cell mode_def: one mode "M" with one value "vA"
  // (cond = D, sdf_cond = "1").
  ModeDef *mode = cell->makeModeDef("M");
  ModeValueDef *va = mode->defineValue("vA");
  ASSERT_NE(va, nullptr);
  va->setSdfCond("D == 1'b1");
  va->setCond(FuncExpr::makePort(d));

  // Sequential: register, clocked on CLK, data = D, clear = CLR,
  // output = Q.
  cell->makeSequential(/*size=*/1,
                       /*is_register=*/true,
                       FuncExpr::makePort(clk),
                       FuncExpr::makePort(d),
                       FuncExpr::makePort(clr),
                       /*preset=*/nullptr,
                       LogicValue::zero,
                       LogicValue::one,
                       q,
                       /*output_inv=*/nullptr);
}

static void
expectSequentialCellRoundTripped(LibertyLibrary *lib)
{
  LibertyCell *cell = lib->findLibertyCell("DFF");
  ASSERT_NE(cell, nullptr);

  // Per-cell bus_dcl.
  BusDcl *bd = cell->findBusDcl("LOCAL_BUS");
  ASSERT_NE(bd, nullptr);
  EXPECT_EQ(bd->from(), 0);
  EXPECT_EQ(bd->to(),   3);

  // Mode definition + value.
  const ModeDef *mode = cell->findModeDef("M");
  ASSERT_NE(mode, nullptr);
  const ModeValueDef *va = mode->findValueDef("vA");
  ASSERT_NE(va, nullptr);
  EXPECT_EQ(va->sdfCond(), "D == 1'b1");
  ASSERT_NE(va->cond(), nullptr);
  EXPECT_EQ(va->cond()->op(), FuncExpr::Op::port);
  // Cond port should be the D port from the round-tripped cell.
  LibertyPort *d_round = cell->findLibertyPort("D");
  EXPECT_EQ(va->cond()->port(), d_round);

  // Sequential.
  ASSERT_TRUE(cell->hasSequentials());
  const SequentialSeq &seqs = cell->sequentials();
  ASSERT_EQ(seqs.size(), 1u);
  const Sequential &s = seqs[0];
  EXPECT_TRUE(s.isRegister());

  LibertyPort *clk_round = cell->findLibertyPort("CLK");
  LibertyPort *clr_round = cell->findLibertyPort("CLR");
  LibertyPort *q_round   = cell->findLibertyPort("Q");

  ASSERT_NE(s.clock(), nullptr);
  EXPECT_EQ(s.clock()->op(), FuncExpr::Op::port);
  EXPECT_EQ(s.clock()->port(), clk_round);

  ASSERT_NE(s.data(), nullptr);
  EXPECT_EQ(s.data()->port(), d_round);

  ASSERT_NE(s.clear(), nullptr);
  EXPECT_EQ(s.clear()->port(), clr_round);

  EXPECT_EQ(s.preset(), nullptr);
  EXPECT_EQ(s.clearPresetOutput(),    LogicValue::zero);
  EXPECT_EQ(s.clearPresetOutputInv(), LogicValue::one);
  EXPECT_EQ(s.output(),    q_round);
  EXPECT_EQ(s.outputInv(), nullptr);
}

// Helper: build a small 1D Table (size N) for tests.
static TablePtr
make1DTable(TableAxisVariable var, std::vector<float> axis_pts,
            std::vector<float> values)
{
  FloatSeq axis_fs(axis_pts.begin(), axis_pts.end());
  TableAxisPtr axis = std::make_shared<TableAxis>(var, std::move(axis_fs));
  FloatSeq vals(values.begin(), values.end());
  return std::make_shared<Table>(std::move(vals), axis);
}

// Helper: build a 2D Table.
static TablePtr
make2DTable(TableAxisVariable var1, std::vector<float> axis1_pts,
            TableAxisVariable var2, std::vector<float> axis2_pts,
            std::vector<std::vector<float>> values)
{
  FloatSeq a1(axis1_pts.begin(), axis1_pts.end());
  FloatSeq a2(axis2_pts.begin(), axis2_pts.end());
  TableAxisPtr axis1 = std::make_shared<TableAxis>(var1, std::move(a1));
  TableAxisPtr axis2 = std::make_shared<TableAxis>(var2, std::move(a2));
  FloatTable t;
  t.reserve(values.size());
  for (auto &row : values)
    t.emplace_back(row.begin(), row.end());
  return std::make_shared<Table>(std::move(t), axis1, axis2);
}

static void
populateOcvAndDriverWaveforms(LibertyLibrary *lib)
{
  // OcvDerate "early_drv": one 1D table (rise/early/clk) + one 2D
  // table (fall/late/data). All other slots remain null.
  OcvDerate *derate = lib->makeOcvDerate("early_drv");
  derate->setDerateTable(RiseFall::rise(), EarlyLate::early(), PathType::clk,
                         make1DTable(TableAxisVariable::input_net_transition,
                                     {0.01F, 0.05F, 0.20F},
                                     {0.95F, 1.00F, 1.05F}));
  derate->setDerateTable(RiseFall::fall(), EarlyLate::late(), PathType::data,
                         make2DTable(TableAxisVariable::input_net_transition,
                                     {0.01F, 0.05F},
                                     TableAxisVariable::total_output_net_capacitance,
                                     {0.005F, 0.050F, 0.500F},
                                     {{1.10F, 1.05F, 1.00F},
                                      {1.20F, 1.10F, 1.05F}}));
  lib->setDefaultOcvDerate(derate);

  // DriverWaveform: a 2D table (slew × time → voltage).
  TablePtr wf = make2DTable(TableAxisVariable::input_net_transition,
                            {0.01F, 0.05F},
                            TableAxisVariable::time,
                            {0.0F, 0.5F, 1.0F},
                            {{0.0F, 0.5F, 0.9F},
                             {0.0F, 0.4F, 0.8F}});
  lib->makeDriverWaveform("rising_wave", wf);
}

static void
expectOcvAndDriverWaveformsRoundTripped(LibertyLibrary *lib)
{
  // OCV derate.
  OcvDerate *derate = lib->findOcvDerate("early_drv");
  ASSERT_NE(derate, nullptr);
  EXPECT_EQ(lib->defaultOcvDerate(), derate);

  // Rise/early/clk: 1D, 3 entries.
  const Table *t1 = derate->derateTable(RiseFall::rise(), EarlyLate::early(),
                                        PathType::clk);
  ASSERT_NE(t1, nullptr);
  EXPECT_EQ(t1->order(), 1);
  ASSERT_NE(t1->axis1(), nullptr);
  ASSERT_EQ(t1->axis1()->values().size(), 3u);
  EXPECT_FLOAT_EQ(t1->axis1()->values()[2], 0.20F);
  EXPECT_FLOAT_EQ(t1->value(static_cast<size_t>(0)), 0.95F);
  EXPECT_FLOAT_EQ(t1->value(static_cast<size_t>(2)), 1.05F);

  // Fall/late/data: 2D, 2 rows × 3 cols.
  const Table *t2 = derate->derateTable(RiseFall::fall(), EarlyLate::late(),
                                        PathType::data);
  ASSERT_NE(t2, nullptr);
  EXPECT_EQ(t2->order(), 2);
  ASSERT_NE(t2->axis1(), nullptr);
  ASSERT_NE(t2->axis2(), nullptr);
  EXPECT_EQ(t2->axis1()->values().size(), 2u);
  EXPECT_EQ(t2->axis2()->values().size(), 3u);
  EXPECT_FLOAT_EQ(t2->value(0, 0), 1.10F);
  EXPECT_FLOAT_EQ(t2->value(1, 2), 1.05F);

  // Other slots are null.
  EXPECT_EQ(derate->derateTable(RiseFall::rise(), EarlyLate::late(),
                                PathType::clk),
            nullptr);

  // DriverWaveform.
  DriverWaveform *wf = lib->findDriverWaveform("rising_wave");
  ASSERT_NE(wf, nullptr);
  const Table *wf_table = wf->waveformsTable();
  ASSERT_NE(wf_table, nullptr);
  EXPECT_EQ(wf_table->order(), 2);
  EXPECT_EQ(wf_table->axis1()->values().size(), 2u);
  EXPECT_EQ(wf_table->axis2()->values().size(), 3u);
  EXPECT_FLOAT_EQ(wf_table->value(0, 2), 0.9F);
  EXPECT_FLOAT_EQ(wf_table->value(1, 1), 0.4F);
}

// Build a 2D NLDM-style TableModel: input slew × output cap → delay.
static TableModel *
makeDelayTableModel(LibertyLibrary *lib, const RiseFall *rf,
                    std::vector<float> slew_pts,
                    std::vector<float> cap_pts,
                    std::vector<std::vector<float>> values)
{
  // Reuse / find the delay template if it exists; create otherwise.
  TableTemplate *tt = lib->findTableTemplate("delay_2d", TableTemplateType::delay);
  if (tt == nullptr) {
    tt = lib->makeTableTemplate("delay_2d", TableTemplateType::delay);
    FloatSeq slew_axis(slew_pts.begin(), slew_pts.end());
    FloatSeq cap_axis (cap_pts.begin(),  cap_pts.end());
    tt->setAxis1(std::make_shared<TableAxis>(
        TableAxisVariable::input_net_transition, std::move(slew_axis)));
    tt->setAxis2(std::make_shared<TableAxis>(
        TableAxisVariable::total_output_net_capacitance, std::move(cap_axis)));
  }
  TablePtr table = make2DTable(TableAxisVariable::input_net_transition,
                               slew_pts,
                               TableAxisVariable::total_output_net_capacitance,
                               cap_pts, values);
  return new TableModel(std::move(table), tt, ScaleFactorType::cell, rf);
}

static void
populateCellWithTimingArc(LibertyLibrary *lib)
{
  PortDirectionInit pd_init;
  LibertyBuilder builder(/*debug=*/nullptr, /*report=*/nullptr);
  LibertyCell *cell = builder.makeCell(lib, "BUF", "buf.lib");
  PortDirection *in_dir = PortDirection::find("input");
  PortDirection *out_dir = PortDirection::find("output");
  LibertyPort *a = builder.makePort(cell, "A");
  LibertyPort *y = builder.makePort(cell, "Y");
  a->setDirection(in_dir);
  y->setDirection(out_dir);

  // Two-row × two-col delay tables for rise and fall edges.
  TableModel *rise_delay = makeDelayTableModel(lib, RiseFall::rise(),
      {0.01F, 0.05F},
      {0.005F, 0.050F},
      {{0.10F, 0.20F},
       {0.15F, 0.30F}});
  TableModel *fall_delay = makeDelayTableModel(lib, RiseFall::fall(),
      {0.01F, 0.05F},
      {0.005F, 0.050F},
      {{0.12F, 0.22F},
       {0.18F, 0.34F}});
  // Slew tables (just reuse the same shape with different values).
  TableModel *rise_slew = makeDelayTableModel(lib, RiseFall::rise(),
      {0.01F, 0.05F},
      {0.005F, 0.050F},
      {{0.05F, 0.10F},
       {0.08F, 0.15F}});
  TableModel *fall_slew = makeDelayTableModel(lib, RiseFall::fall(),
      {0.01F, 0.05F},
      {0.005F, 0.050F},
      {{0.06F, 0.11F},
       {0.09F, 0.16F}});

  TimingArcAttrsPtr attrs = std::make_shared<TimingArcAttrs>();
  attrs->setTimingType(TimingType::combinational);
  attrs->setTimingSense(TimingSense::positive_unate);
  attrs->setModel(RiseFall::rise(),
                  new GateTableModel(cell,
                                     new TableModels(rise_delay),
                                     new TableModels(rise_slew)));
  attrs->setModel(RiseFall::fall(),
                  new GateTableModel(cell,
                                     new TableModels(fall_delay),
                                     new TableModels(fall_slew)));

  TimingArcSet *set = cell->makeTimingArcSet(a, y, /*related_out=*/nullptr,
                                             TimingRole::combinational(),
                                             attrs);
  // TimingArc's ctor calls set->addTimingArc(this); constructing is
  // sufficient. A second addTimingArc() would double-register the
  // pointer and trip a double-free at teardown.
  new TimingArc(set, Transition::rise(), Transition::rise(),
                attrs->model(RiseFall::rise()));
  new TimingArc(set, Transition::fall(), Transition::fall(),
                attrs->model(RiseFall::fall()));
}

static void
expectCellWithTimingArcRoundTripped(LibertyLibrary *lib)
{
  LibertyCell *cell = lib->findLibertyCell("BUF");
  ASSERT_NE(cell, nullptr);
  const TimingArcSetSeq &sets = cell->timingArcSets();
  ASSERT_EQ(sets.size(), 1u);
  const TimingArcSet *set = sets[0];

  ASSERT_NE(set->from(), nullptr);
  ASSERT_NE(set->to(), nullptr);
  EXPECT_EQ(set->from()->name(), "A");
  EXPECT_EQ(set->to()->name(),   "Y");
  EXPECT_EQ(set->relatedOut(), nullptr);
  EXPECT_EQ(set->role(), TimingRole::combinational());
  EXPECT_EQ(set->timingType(), TimingType::combinational);

  // Two arcs, rise→rise and fall→fall.
  const TimingArcSeq &arcs = set->arcs();
  ASSERT_EQ(arcs.size(), 2u);
  EXPECT_EQ(arcs[0]->fromEdge(), Transition::rise());
  EXPECT_EQ(arcs[0]->toEdge(),   Transition::rise());
  EXPECT_EQ(arcs[1]->fromEdge(), Transition::fall());
  EXPECT_EQ(arcs[1]->toEdge(),   Transition::fall());

  // Each arc points to the appropriate attrs model.
  ASSERT_NE(arcs[0]->model(), nullptr);
  ASSERT_NE(arcs[1]->model(), nullptr);

  const GateTableModel *rise_gm =
      static_cast<const GateTableModel*>(arcs[0]->model());
  ASSERT_NE(rise_gm->delayModels(), nullptr);
  const TableModel *rise_delay = rise_gm->delayModels()->model();
  ASSERT_NE(rise_delay, nullptr);
  ASSERT_NE(rise_delay->table().get(), nullptr);
  EXPECT_EQ(rise_delay->order(), 2);
  // Spot-check a value.
  EXPECT_FLOAT_EQ(rise_delay->table()->value(0u, 0u), 0.10F);
  EXPECT_FLOAT_EQ(rise_delay->table()->value(1u, 1u), 0.30F);

  const GateTableModel *fall_gm =
      static_cast<const GateTableModel*>(arcs[1]->model());
  ASSERT_NE(fall_gm->slewModels(), nullptr);
  const TableModel *fall_slew = fall_gm->slewModels()->model();
  ASSERT_NE(fall_slew, nullptr);
  EXPECT_FLOAT_EQ(fall_slew->table()->value(1u, 0u), 0.09F);
}

// Build an OutputWaveforms with a 2-by-2 grid of trivial 1D
// time/current waveforms + a 0D ref_times scalar.
static OutputWaveforms *
makeOutputWaveforms(const RiseFall *rf,
                    std::vector<float> slew_pts,
                    std::vector<float> cap_pts)
{
  FloatSeq sf(slew_pts.begin(), slew_pts.end());
  FloatSeq cf(cap_pts.begin(),  cap_pts.end());
  TableAxisPtr slew_axis = std::make_shared<TableAxis>(
      TableAxisVariable::input_net_transition, std::move(sf));
  TableAxisPtr cap_axis = std::make_shared<TableAxis>(
      TableAxisVariable::total_output_net_capacitance, std::move(cf));

  // Each waveform is a 1D time→current table. We synthesize trivial
  // 3-point waveforms for the test; values matter for round-trip.
  Table1Seq waveforms;
  for (size_t s = 0; s < slew_pts.size(); ++s) {
    for (size_t c = 0; c < cap_pts.size(); ++c) {
      FloatSeq time_axis_pts{0.0F, 0.5F, 1.0F};
      TableAxisPtr time_axis = std::make_shared<TableAxis>(
          TableAxisVariable::time, std::move(time_axis_pts));
      FloatSeq currents{
          0.0F + 0.1F * s,
          1.0F + 0.1F * c,
          2.0F + 0.1F * (s + c)};
      waveforms.push_back(new Table(std::move(currents), time_axis));
    }
  }
  Table ref_times{0.42F};
  return new OutputWaveforms(slew_axis, cap_axis, rf, waveforms,
                             std::move(ref_times));
}

static void
populateCellWithCcsArc(LibertyLibrary *lib)
{
  PortDirectionInit pd_init;
  LibertyBuilder builder(/*debug=*/nullptr, /*report=*/nullptr);
  LibertyCell *cell = builder.makeCell(lib, "BUF_CCS", "buf_ccs.lib");
  PortDirection *in_dir  = PortDirection::find("input");
  PortDirection *out_dir = PortDirection::find("output");
  LibertyPort *a = builder.makePort(cell, "A");
  LibertyPort *y = builder.makePort(cell, "Y");
  a->setDirection(in_dir);
  y->setDirection(out_dir);

  // Library-level DriverWaveform "rising_edge".
  TablePtr dw_table = make2DTable(TableAxisVariable::input_net_transition,
                                  {0.01F, 0.05F},
                                  TableAxisVariable::time,
                                  {0.0F, 0.5F, 1.0F},
                                  {{0.0F, 0.5F, 0.9F},
                                   {0.0F, 0.4F, 0.8F}});
  lib->makeDriverWaveform("rising_edge", dw_table);

  // Per-port DriverWaveform on a (rise edge).
  a->setDriverWaveform(lib->findDriverWaveform("rising_edge"),
                       RiseFall::rise());

  // Per-port ReceiverModel on a: one capacitance model per (segment,
  // rf). Use segment 0 with rise + fall, and segment 1 with rise.
  auto rm = std::make_shared<ReceiverModel>();
  TableTemplate *cap_tt = lib->makeTableTemplate("cap_1d",
                                                 TableTemplateType::capacitance);
  FloatSeq cap_axis_pts{0.005F, 0.050F};
  cap_tt->setAxis1(std::make_shared<TableAxis>(
      TableAxisVariable::input_net_transition, std::move(cap_axis_pts)));
  auto make_cap_model = [&](const RiseFall *rf, std::vector<float> values) {
    FloatSeq fs(values.begin(), values.end());
    FloatSeq axis_pts{0.005F, 0.050F};
    TableAxisPtr axis = std::make_shared<TableAxis>(
        TableAxisVariable::input_net_transition, std::move(axis_pts));
    TablePtr table = std::make_shared<Table>(std::move(fs), axis);
    return TableModel(std::move(table), cap_tt, ScaleFactorType::cell, rf);
  };
  rm->setCapacitanceModel(make_cap_model(RiseFall::rise(), {0.001F, 0.0015F}),
                          /*segment=*/0, RiseFall::rise());
  rm->setCapacitanceModel(make_cap_model(RiseFall::fall(), {0.0011F, 0.0016F}),
                          /*segment=*/0, RiseFall::fall());
  rm->setCapacitanceModel(make_cap_model(RiseFall::rise(), {0.002F, 0.003F}),
                          /*segment=*/1, RiseFall::rise());
  a->setReceiverModel(rm);

  // Build a combinational arc with a CCS-equipped GateTableModel
  // (delay/slew tables + per-arc receiver model + output waveforms).
  TableModel *rise_delay = makeDelayTableModel(lib, RiseFall::rise(),
      {0.01F, 0.05F}, {0.005F, 0.050F},
      {{0.10F, 0.20F}, {0.15F, 0.30F}});
  TableModel *rise_slew  = makeDelayTableModel(lib, RiseFall::rise(),
      {0.01F, 0.05F}, {0.005F, 0.050F},
      {{0.05F, 0.10F}, {0.08F, 0.15F}});
  TableModels *rise_dm = new TableModels(rise_delay);
  TableModels *rise_sm = new TableModels(rise_slew);

  // Arc-level receiver model (independent from per-port one).
  auto arc_rm = std::make_shared<ReceiverModel>();
  arc_rm->setCapacitanceModel(make_cap_model(RiseFall::rise(),
                                             {0.0009F, 0.0012F}),
                              /*segment=*/0, RiseFall::rise());

  OutputWaveforms *ow = makeOutputWaveforms(RiseFall::rise(),
                                            {0.01F, 0.05F},
                                            {0.005F, 0.050F});

  GateTableModel *rise_model =
      new GateTableModel(cell, rise_dm, rise_sm, arc_rm, ow);

  TimingArcAttrsPtr attrs = std::make_shared<TimingArcAttrs>();
  attrs->setTimingType(TimingType::combinational);
  attrs->setTimingSense(TimingSense::positive_unate);
  attrs->setModel(RiseFall::rise(), rise_model);

  TimingArcSet *set = cell->makeTimingArcSet(a, y, /*related_out=*/nullptr,
                                             TimingRole::combinational(),
                                             attrs);
  // TimingArc's ctor self-registers via set->addTimingArc(this).
  new TimingArc(set, Transition::rise(), Transition::rise(),
                attrs->model(RiseFall::rise()));
}

static void
expectCellWithCcsArcRoundTripped(LibertyLibrary *lib)
{
  LibertyCell *cell = lib->findLibertyCell("BUF_CCS");
  ASSERT_NE(cell, nullptr);
  LibertyPort *a = cell->findLibertyPort("A");
  ASSERT_NE(a, nullptr);

  // Per-port ReceiverModel.
  const ReceiverModel *port_rm = a->receiverModel();
  ASSERT_NE(port_rm, nullptr);
  // Layout: [seg0_rise, seg0_fall, seg1_rise, seg1_fall]. Index 3
  // (seg1_fall) was never populated, so its TableModel has no table.
  const auto &models = port_rm->capacitanceModels();
  ASSERT_GE(models.size(), 3u);
  ASSERT_NE(models[0].table().get(), nullptr);
  ASSERT_NE(models[1].table().get(), nullptr);
  ASSERT_NE(models[2].table().get(), nullptr);
  EXPECT_FLOAT_EQ(models[0].table()->value(static_cast<size_t>(0)), 0.001F);
  EXPECT_FLOAT_EQ(models[1].table()->value(static_cast<size_t>(1)), 0.0016F);
  EXPECT_FLOAT_EQ(models[2].table()->value(static_cast<size_t>(0)), 0.002F);

  // Per-port DriverWaveform refs.
  DriverWaveform *dw_rise = a->driverWaveform(RiseFall::rise());
  ASSERT_NE(dw_rise, nullptr);
  EXPECT_EQ(dw_rise->name(), "rising_edge");
  EXPECT_EQ(a->driverWaveform(RiseFall::fall()), nullptr);

  // Per-arc CCS model.
  ASSERT_EQ(cell->timingArcSets().size(), 1u);
  const TimingArc *arc = cell->timingArcSets()[0]->arcs()[0];
  const GateTableModel *gm =
      static_cast<const GateTableModel*>(arc->model());
  ASSERT_NE(gm, nullptr);
  ASSERT_NE(gm->receiverModel(), nullptr);
  ASSERT_GE(gm->receiverModel()->capacitanceModels().size(), 1u);

  OutputWaveforms *ow = gm->outputWaveforms();
  ASSERT_NE(ow, nullptr);
  EXPECT_EQ(ow->rf(), RiseFall::rise());
  ASSERT_NE(ow->slewAxis(), nullptr);
  ASSERT_NE(ow->capAxis(),  nullptr);
  EXPECT_EQ(ow->slewAxis()->values().size(), 2u);
  EXPECT_EQ(ow->capAxis()->values().size(),  2u);
  // 2x2 grid → 4 waveforms.
  EXPECT_EQ(ow->currentWaveforms().size(), 4u);
  // ref_times was a 0D scalar = 0.42.
  EXPECT_FLOAT_EQ(ow->referenceTimes().value(0u, 0u, 0u), 0.42F);
}

// Build a cell with internal/leakage power records (commit 5).
static void
populateCellWithPower(LibertyLibrary *lib)
{
  PortDirectionInit pd_init;
  LibertyBuilder builder(/*debug=*/nullptr, /*report=*/nullptr);
  LibertyCell *cell = builder.makeCell(lib, "AND_PWR", "and_pwr.lib");
  PortDirection *in_dir  = PortDirection::find("input");
  PortDirection *out_dir = PortDirection::find("output");
  PortDirection *pg_dir  = PortDirection::find("internal");
  LibertyPort *a = builder.makePort(cell, "A");
  LibertyPort *b = builder.makePort(cell, "B");
  LibertyPort *y = builder.makePort(cell, "Y");
  LibertyPort *vdd = builder.makePort(cell, "VDD");
  a->setDirection(in_dir);
  b->setDirection(in_dir);
  y->setDirection(out_dir);
  vdd->setDirection(pg_dir ? pg_dir : in_dir);
  vdd->setPwrGndType(PwrGndType::primary_power);

  // Build a small power TableModel (just for the test — values are
  // arbitrary).
  TableTemplate *tt = lib->makeTableTemplate("pwr_2d", TableTemplateType::power);
  FloatSeq slew_pts{0.01F, 0.05F};
  FloatSeq cap_pts {0.005F, 0.050F};
  tt->setAxis1(std::make_shared<TableAxis>(
      TableAxisVariable::input_net_transition, std::move(slew_pts)));
  tt->setAxis2(std::make_shared<TableAxis>(
      TableAxisVariable::total_output_net_capacitance, std::move(cap_pts)));

  auto make_pwr_model = [&](const RiseFall *rf, std::vector<std::vector<float>> values) {
    TablePtr table = make2DTable(TableAxisVariable::input_net_transition,
                                 {0.01F, 0.05F},
                                 TableAxisVariable::total_output_net_capacitance,
                                 {0.005F, 0.050F},
                                 std::move(values));
    return new TableModel(std::move(table), tt, ScaleFactorType::cell, rf);
  };

  InternalPowerModels ip_models{
      InternalPowerModel(std::shared_ptr<TableModel>(
          make_pwr_model(RiseFall::rise(), {{0.001F, 0.002F},
                                             {0.0015F, 0.0025F}}))),
      InternalPowerModel(std::shared_ptr<TableModel>(
          make_pwr_model(RiseFall::fall(), {{0.0011F, 0.0022F},
                                             {0.0016F, 0.0026F}})))};

  // Internal power on Y, related to A (from the AND function).
  cell->makeInternalPower(y, a, vdd, /*when=*/nullptr, ip_models);

  // Leakage power: when = (A AND B), pg port = vdd.
  cell->makeLeakagePower(vdd,
                         FuncExpr::makeAnd(FuncExpr::makePort(a),
                                           FuncExpr::makePort(b)),
                         /*power=*/0.05F);
  // And a second leakage entry without a when expression.
  cell->makeLeakagePower(vdd, /*when=*/nullptr, 0.01F);
}

static void
expectCellWithPowerRoundTripped(LibertyLibrary *lib)
{
  LibertyCell *cell = lib->findLibertyCell("AND_PWR");
  ASSERT_NE(cell, nullptr);
  LibertyPort *a   = cell->findLibertyPort("A");
  LibertyPort *y   = cell->findLibertyPort("Y");
  LibertyPort *vdd = cell->findLibertyPort("VDD");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(y, nullptr);
  ASSERT_NE(vdd, nullptr);
  EXPECT_EQ(vdd->pwrGndType(), PwrGndType::primary_power);

  const InternalPowerSeq &ips = cell->internalPowers();
  ASSERT_EQ(ips.size(), 1u);
  const InternalPower &ip = ips[0];
  EXPECT_EQ(ip.port(),         y);
  EXPECT_EQ(ip.relatedPort(),  a);
  EXPECT_EQ(ip.relatedPgPin(), vdd);
  EXPECT_EQ(ip.when(), nullptr);
  // Per-RF model survives.
  ASSERT_NE(ip.model(RiseFall::rise()).model(), nullptr);
  ASSERT_NE(ip.model(RiseFall::fall()).model(), nullptr);
  const Table *rise_table = ip.model(RiseFall::rise()).model()->table().get();
  ASSERT_NE(rise_table, nullptr);
  EXPECT_EQ(rise_table->order(), 2);
  EXPECT_FLOAT_EQ(rise_table->value(0u, 0u), 0.001F);
  EXPECT_FLOAT_EQ(rise_table->value(1u, 1u), 0.0025F);

  const LeakagePowerSeq &lps = cell->leakagePowers();
  ASSERT_EQ(lps.size(), 2u);

  // First entry: when = (A AND B), power = 0.05.
  EXPECT_EQ(lps[0].relatedPgPort(), vdd);
  EXPECT_FLOAT_EQ(lps[0].power(), 0.05F);
  ASSERT_NE(lps[0].when(), nullptr);
  EXPECT_EQ(lps[0].when()->op(), FuncExpr::Op::and_);

  // Second entry: no when.
  EXPECT_EQ(lps[1].relatedPgPort(), vdd);
  EXPECT_FLOAT_EQ(lps[1].power(), 0.01F);
  EXPECT_EQ(lps[1].when(), nullptr);
}

TEST(LibertyCache, InternalAndLeakagePowerRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("pwr_lib", ""));
  populateLibraryScalars(src.get());
  populateCellWithPower(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_pwr.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectCellWithPowerRoundTripped(dst.get());
}

TEST(LibertyCache, CcsRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("ccs_lib", ""));
  populateLibraryScalars(src.get());
  populateCellWithCcsArc(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_ccs.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectCellWithCcsArcRoundTripped(dst.get());
}

TEST(LibertyCache, TimingArcSetRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("arc_lib", ""));
  populateLibraryScalars(src.get());
  populateCellWithTimingArc(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_arc.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectCellWithTimingArcRoundTripped(dst.get());
}

TEST(LibertyCache, OcvAndDriverWaveformsRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("ocv_lib", ""));
  populateLibraryScalars(src.get());
  populateOcvAndDriverWaveforms(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_ocv.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectOcvAndDriverWaveformsRoundTripped(dst.get());
}

TEST(LibertyCache, SequentialAndModeDefRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("seq_lib", ""));
  populateLibraryScalars(src.get());
  populateSequentialCell(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_seq.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectSequentialCellRoundTripped(dst.get());
}

TEST(LibertyCache, CellPortsAndFuncExprRoundTrip)
{
  std::unique_ptr<LibertyLibrary> src(new LibertyLibrary("ports_lib", ""));
  populateLibraryScalars(src.get());
  populateCellWithPorts(src.get());

  TempCachePath cache_path("/tmp/sta_libcache_ports.cache");
  writeLibertyCache(src.get(), cache_path.c_str(), nullptr);

  std::unique_ptr<LibertyLibrary> dst(
      readLibertyCache(cache_path.c_str(), true, nullptr));
  ASSERT_NE(dst.get(), nullptr);

  expectCellWithPortsRoundTripped(dst.get());
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
