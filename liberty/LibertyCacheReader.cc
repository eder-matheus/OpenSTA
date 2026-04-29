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
#include "FuncExpr.hh"
#include "InternalPower.hh"
#include "LeakagePower.hh"
#include "LibertyBuilder.hh"
#include "NetworkClass.hh"
#include "Network.hh"
#include "PortDirection.hh"
#include "RiseFallMinMax.hh"
#include "Sequential.hh"
#include "StaConfig.hh"
#include "TableModel.hh"
#include "TimingArc.hh"
#include "TimingModel.hh"
#include "TimingRole.hh"
#include "StaState.hh"
#include "Transition.hh"
#include "Units.hh"

namespace sta {

using cache::SectionId;

namespace {

// Forward declaration: readCellPortDetails calls readReceiverModel
// for the per-port receiver block, but readReceiverModel lives in
// the timing-arc helper block further down the file.
ReceiverModelPtr readReceiverModel(FILE *f, LibertyLibrary *lib);

void
readHeader(FILE *f,
           bool ignore_source_check,
           // Out: header fields the reader may need later.
           std::string &source_filename)
{
  uint32_t magic = cache::readU32(f);
  if (magic != cache::kMagic)
    cache::error(sta::format("ldb: bad magic 0x{:x} (expected 0x{:x})",
                             magic, cache::kMagic));
  uint32_t version = cache::readU32(f);
  if (version != cache::kFormatVersion)
    cache::error(sta::format(
        "ldb: format version {} not supported (this build expects {}); "
        "regenerate the cache with this version of STA.",
        version, cache::kFormatVersion));
  uint32_t endian = cache::readU32(f);
  if (endian != cache::kEndianSentinel)
    cache::error("ldb: endian mismatch (cache was written on a "
                 "host with the opposite byte order; not supported)");
  cache::readU32(f); // flags (reserved, ignored in v1)

  std::string sta_version = cache::readString(f);
  if (sta_version != STA_VERSION)
    cache::error(sta::format(
        "ldb: STA version mismatch (cache='{}', this build='{}'); "
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
        "ldb: source file '{}' is missing; pass "
        "-ignore_source_check to load anyway.", source_filename));

  uint64_t cur_size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  auto ftime = std::filesystem::last_write_time(path, ec);
  int64_t cur_mtime = std::chrono::duration_cast<std::chrono::seconds>(
                        ftime.time_since_epoch()).count();
  if (cur_size != source_size || cur_mtime != source_mtime)
    cache::error(sta::format(
        "ldb: source file '{}' has changed since the cache was "
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

void
readUnits(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::Units);
  Units *units = lib->units();
  auto read_unit = [&](Unit *u) {
    float scale = cache::readFloat(f);
    std::string suffix = cache::readString(f);
    uint32_t digits = cache::readU32(f);
    u->setScale(scale);
    u->setSuffix(suffix.c_str());
    u->setDigits(static_cast<int>(digits));
  };
  read_unit(units->timeUnit());
  read_unit(units->resistanceUnit());
  read_unit(units->capacitanceUnit());
  read_unit(units->voltageUnit());
  read_unit(units->currentUnit());
  read_unit(units->powerUnit());
  read_unit(units->distanceUnit());
  read_unit(units->scalarUnit());
}

void
readBusDcls(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::BusDcls);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    int from = static_cast<int>(cache::readI64(f));
    int to   = static_cast<int>(cache::readI64(f));
    lib->makeBusDcl(name, from, to);
  }
}

void
readOperatingConditions(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::OperatingConditions);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    OperatingConditions *op = lib->makeOperatingConditions(name);
    op->setProcess(cache::readFloat(f));
    op->setVoltage(cache::readFloat(f));
    op->setTemperature(cache::readFloat(f));
    op->setWireloadTree(static_cast<WireloadTree>(cache::readU32(f)));
  }
  std::string default_name = cache::readString(f);
  if (!default_name.empty())
    lib->setDefaultOperatingConditions(lib->findOperatingConditions(default_name));
}

void
readScaleFactors(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::ScaleFactors);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    ScaleFactors *sf = lib->makeScaleFactors(name);
    for (int t = 0; t < scale_factor_type_count; ++t) {
      for (int p = 0; p < scale_factor_pvt_count; ++p) {
        for (auto rf : RiseFall::range())
          sf->setScale(static_cast<ScaleFactorType>(t),
                       static_cast<ScaleFactorPvt>(p), rf,
                       cache::readFloat(f));
      }
    }
  }
  std::string default_name = cache::readString(f);
  if (!default_name.empty())
    lib->setScaleFactors(lib->findScaleFactors(default_name));
}

void
readSupplyVoltages(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::SupplyVoltages);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    float voltage = cache::readFloat(f);
    lib->addSupplyVoltage(name, voltage);
  }
}

// Returns the deserialized TableAxis, or nullptr if the on-disk
// "present" flag was false.
TableAxisPtr
readTableAxis(FILE *f)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  TableAxisVariable variable = static_cast<TableAxisVariable>(cache::readU32(f));
  std::vector<float> values = cache::readFloatArray(f);
  FloatSeq fs(values.begin(), values.end());
  return std::make_shared<TableAxis>(variable, std::move(fs));
}

// === Table read =====================================================
//
// Mirror of writeTable in LibertyCacheWriter.cc; see the comment block
// there for the on-disk shape.

TablePtr
readTablePtr(FILE *f)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  uint32_t order = cache::readU32(f);
  switch (order) {
  case 0: {
    float v = cache::readFloat(f);
    return std::make_shared<Table>(v);
  }
  case 1: {
    TableAxisPtr axis1 = readTableAxis(f);
    auto raw = cache::readFloatArray(f);
    FloatSeq fs(raw.begin(), raw.end());
    return std::make_shared<Table>(std::move(fs), axis1);
  }
  case 2:
  case 3: {
    TableAxisPtr axis1 = readTableAxis(f);
    TableAxisPtr axis2 = readTableAxis(f);
    TableAxisPtr axis3 = (order == 3) ? readTableAxis(f) : nullptr;
    uint32_t row_count = cache::readU32(f);
    FloatTable rows;
    rows.reserve(row_count);
    for (uint32_t r = 0; r < row_count; ++r) {
      auto raw = cache::readFloatArray(f);
      rows.emplace_back(raw.begin(), raw.end());
    }
    if (order == 2)
      return std::make_shared<Table>(std::move(rows), axis1, axis2);
    return std::make_shared<Table>(std::move(rows), axis1, axis2, axis3);
  }
  default:
    cache::error(sta::format("ldb: unsupported table order {}", order));
  }
  return nullptr;
}

void
readTableTemplates(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::TableTemplates);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    TableTemplateType type = static_cast<TableTemplateType>(cache::readU32(f));
    std::string name = cache::readString(f);
    TableAxisPtr axis1 = readTableAxis(f);
    TableAxisPtr axis2 = readTableAxis(f);
    TableAxisPtr axis3 = readTableAxis(f);
    // The library owns all template instances. makeTableTemplate
    // inserts a default-constructed template under (name, type) and
    // returns a pointer that we then fill in.
    TableTemplate *tt = lib->makeTableTemplate(name, type);
    tt->setAxis1(axis1);
    tt->setAxis2(axis2);
    tt->setAxis3(axis3);
  }
}

// === Port + FuncExpr helpers =========================================
//
// Mirror of the writer-side helpers; see comment block in
// LibertyCacheWriter.cc.

FuncExpr *
readFuncExpr(FILE *f, const std::vector<LibertyPort*> &ports)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  FuncExpr::Op op = static_cast<FuncExpr::Op>(cache::readU32(f));
  switch (op) {
  case FuncExpr::Op::port: {
    uint32_t idx = cache::readU32(f);
    LibertyPort *port = (idx < ports.size()) ? ports[idx] : nullptr;
    return FuncExpr::makePort(port);
  }
  case FuncExpr::Op::not_:
    return FuncExpr::makeNot(readFuncExpr(f, ports));
  case FuncExpr::Op::or_: {
    FuncExpr *l = readFuncExpr(f, ports);
    FuncExpr *r = readFuncExpr(f, ports);
    return FuncExpr::makeOr(l, r);
  }
  case FuncExpr::Op::and_: {
    FuncExpr *l = readFuncExpr(f, ports);
    FuncExpr *r = readFuncExpr(f, ports);
    return FuncExpr::makeAnd(l, r);
  }
  case FuncExpr::Op::xor_: {
    FuncExpr *l = readFuncExpr(f, ports);
    FuncExpr *r = readFuncExpr(f, ports);
    return FuncExpr::makeXor(l, r);
  }
  case FuncExpr::Op::one:
    return FuncExpr::makeOne();
  case FuncExpr::Op::zero:
    return FuncExpr::makeZero();
  }
  return nullptr;
}

const RiseFall *
readRfPtr(FILE *f)
{
  uint32_t code = cache::readU32(f);
  if (code == 1) return RiseFall::rise();
  if (code == 2) return RiseFall::fall();
  return nullptr;
}

// Per-port structural kind (mirror of the writer's PortKind enum).
enum PortKind : uint32_t {
  kPortScalar       = 0,
  kPortBusParent    = 1,
  kPortBusBit       = 2,
  kPortBundleParent = 3,
};

// Resolve a BusDcl by name -- per-cell map first (cell-local types),
// then library map (already populated by readBusDcls).
BusDcl *
findBusDclAnywhere(LibertyCell *cell, const std::string &name)
{
  if (name.empty())
    return nullptr;
  if (BusDcl *bd = cell->findBusDcl(name))
    return bd;
  return cell->libertyLibrary()->findBusDcl(name);
}

// Pass 1: re-create the cell's ports in the same order the writer
// emitted them, including bus parent / bit-member structure and
// bundle wrapping. Returns the per-cell flat port vector that pass 2
// uses for FuncExpr port-reference resolution.
std::vector<LibertyPort*>
readCellPortHeaders(FILE *f, LibertyCell *cell, LibertyBuilder &builder)
{
  uint32_t n = cache::readU32(f);
  std::vector<LibertyPort*> ports;
  ports.reserve(n);

  // When kPortBusParent runs, makeBusPort auto-creates the bit
  // members in declared order -- subsequent kPortBusBit entries pop
  // from this stash instead of doing a per-bit findLibertyPort
  // hashmap lookup.
  std::vector<LibertyPort*> pending_bus_bits;
  size_t pending_idx = 0;

  for (uint32_t i = 0; i < n; ++i) {
    PortKind kind = static_cast<PortKind>(cache::readU32(f));
    std::string name = cache::readString(f);
    std::string dir_name = cache::readString(f);

    LibertyPort *port = nullptr;
    switch (kind) {
    case kPortScalar:
      port = builder.makePort(cell, name);
      break;
    case kPortBusParent: {
      int from_index = static_cast<int>(cache::readI64(f));
      int to_index   = static_cast<int>(cache::readI64(f));
      std::string bus_dcl_name = cache::readString(f);
      BusDcl *bus_dcl = findBusDclAnywhere(cell, bus_dcl_name);
      port = builder.makeBusPort(cell, name, from_index, to_index, bus_dcl);
      pending_bus_bits.clear();
      LibertyPortMemberIterator m(port);
      while (m.hasNext())
        pending_bus_bits.push_back(m.next());
      pending_idx = 0;
      break;
    }
    case kPortBusBit:
      port = (pending_idx < pending_bus_bits.size())
          ? pending_bus_bits[pending_idx++]
          : cell->findLibertyPort(name);
      break;
    case kPortBundleParent: {
      uint32_t mc = cache::readU32(f);
      ConcretePortSeq *members = new ConcretePortSeq;
      members->reserve(mc);
      for (uint32_t mi = 0; mi < mc; ++mi) {
        uint32_t midx = cache::readU32(f);
        members->push_back((midx < ports.size()) ? ports[midx] : nullptr);
      }
      port = builder.makeBundlePort(cell, name, members);
      break;
    }
    }

    if (port && !dir_name.empty()) {
      PortDirection *dir = PortDirection::find(dir_name.c_str());
      if (dir) port->setDirection(dir);
    }
    ports.push_back(port);
  }
  return ports;
}

void
readCellPortDetails(FILE *f, LibertyLibrary *lib,
                    const std::vector<LibertyPort*> &ports)
{
  for (LibertyPort *p : ports) {
    p->setPwrGndType(static_cast<PwrGndType>(cache::readU32(f)));
    p->setVoltageName(cache::readString(f));
    p->setScanSignalType(static_cast<ScanSignalType>(cache::readU32(f)));

    for (auto rf : RiseFall::range()) {
      for (auto mm : MinMax::range()) {
        float val = cache::readFloat(f);
        bool exists = cache::readBool(f);
        if (exists) p->setCapacitance(rf, mm, val);
      }
    }

    {
      // Six (slew/cap/fanout) × (min/max) limits.
      float val; bool exists;
      val = cache::readFloat(f); exists = cache::readBool(f);
      if (exists) p->setSlewLimit(val, MinMax::min());
      val = cache::readFloat(f); exists = cache::readBool(f);
      if (exists) p->setSlewLimit(val, MinMax::max());
      val = cache::readFloat(f); exists = cache::readBool(f);
      if (exists) p->setCapacitanceLimit(val, MinMax::min());
      val = cache::readFloat(f); exists = cache::readBool(f);
      if (exists) p->setCapacitanceLimit(val, MinMax::max());
      val = cache::readFloat(f); exists = cache::readBool(f);
      if (exists) p->setFanoutLimit(val, MinMax::min());
      val = cache::readFloat(f); exists = cache::readBool(f);
      if (exists) p->setFanoutLimit(val, MinMax::max());
    }
    {
      float fanout_load = cache::readFloat(f);
      bool fanout_load_exists = cache::readBool(f);
      if (fanout_load_exists) p->setFanoutLoad(fanout_load);
    }
    {
      float val = cache::readFloat(f);
      bool exists = cache::readBool(f);
      if (exists) p->setMinPeriod(val);
    }
    for (auto rf : RiseFall::range()) {
      float val = cache::readFloat(f);
      bool exists = cache::readBool(f);
      if (exists) p->setMinPulseWidth(rf, val);
    }

    const RiseFall *trig = readRfPtr(f);
    const RiseFall *sense = readRfPtr(f);
    if (trig != nullptr || sense != nullptr)
      p->setPulseClk(trig, sense);

    // Intra-cell related-port refs.
    auto resolve_port = [&](uint32_t idx) -> LibertyPort* {
      if (idx == 0xFFFFFFFFu || idx >= ports.size()) return nullptr;
      return ports[idx];
    };
    LibertyPort *gnd_ref = resolve_port(cache::readU32(f));
    LibertyPort *pwr_ref = resolve_port(cache::readU32(f));
    if (gnd_ref) p->setRelatedGroundPort(gnd_ref);
    if (pwr_ref) p->setRelatedPowerPort(pwr_ref);

    uint32_t flags = cache::readU32(f);
    if (flags & 0x0001) p->setIsClock(true);
    if (flags & 0x0002) p->setIsRegClk(true);
    if (flags & 0x0004) p->setIsRegOutput(true);
    if (flags & 0x0008) p->setIsLatchData(true);
    if (flags & 0x0010) p->setIsCheckClk(true);
    if (flags & 0x0020) p->setIsClockGateClock(true);
    if (flags & 0x0040) p->setIsClockGateEnable(true);
    if (flags & 0x0080) p->setIsClockGateOut(true);
    if (flags & 0x0100) p->setIsPllFeedback(true);
    if (flags & 0x0200) p->setIsolationCellData(true);
    if (flags & 0x0400) p->setIsolationCellEnable(true);
    if (flags & 0x0800) p->setLevelShifterData(true);
    if (flags & 0x1000) p->setIsSwitch(true);
    if (flags & 0x2000) p->setIsPad(true);

    FuncExpr *func = readFuncExpr(f, ports);
    if (func) p->setFunction(func);
    FuncExpr *tri = readFuncExpr(f, ports);
    if (tri) p->setTristateEnable(tri);

    // Per-port ReceiverModel + DriverWaveform refs. The writer
    // interleaves these inside the per-port loop, so the reader has
    // to do the same -- otherwise the file position drifts by
    // (sizeof receiver_model + 2 strings) per port.
    ReceiverModelPtr rm = readReceiverModel(f, lib);
    if (rm) p->setReceiverModel(std::move(rm));
    for (auto rf : RiseFall::range()) {
      std::string dw_name = cache::readString(f);
      if (!dw_name.empty()) {
        if (DriverWaveform *dw = lib->findDriverWaveform(dw_name))
          p->setDriverWaveform(dw, rf);
      }
    }
  }
}

// === Per-arc TableModel / TableModels read ===========================

TableModel *
readTableModel(FILE *f, LibertyLibrary *lib)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  std::string template_name = cache::readString(f);
  TableTemplateType template_type =
      static_cast<TableTemplateType>(cache::readU32(f));
  ScaleFactorType sft = static_cast<ScaleFactorType>(cache::readU32(f));
  uint32_t rf_index = cache::readU32(f);
  TablePtr table = readTablePtr(f);

  TableTemplate *tt = template_name.empty()
      ? nullptr
      : lib->findTableTemplate(template_name, template_type);
  const RiseFall *rf = (rf_index == RiseFall::riseIndex())
      ? RiseFall::rise()
      : RiseFall::fall();
  return new TableModel(std::move(table), tt, sft, rf);
}

TableModels *
readTableModels(FILE *f, LibertyLibrary *lib)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  TableModel *m = readTableModel(f, lib);
  TableModels *models = new TableModels(m);
  TableModel *sigma_early = readTableModel(f, lib);
  TableModel *sigma_late  = readTableModel(f, lib);
  if (sigma_early) models->setSigma(sigma_early, EarlyLate::early());
  if (sigma_late)  models->setSigma(sigma_late,  EarlyLate::late());
  TableModel *std_dev    = readTableModel(f, lib);
  TableModel *mean_shift = readTableModel(f, lib);
  TableModel *skewness   = readTableModel(f, lib);
  if (std_dev)    models->setStdDev(std_dev);
  if (mean_shift) models->setMeanShift(mean_shift);
  if (skewness)   models->setSkewness(skewness);
  return models;
}

// === ReceiverModel + OutputWaveforms read ============================

ReceiverModelPtr
readReceiverModel(FILE *f, LibertyLibrary *lib)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  uint32_t n = cache::readU32(f);
  auto rm = std::make_shared<ReceiverModel>();
  for (uint32_t i = 0; i < n; ++i) {
    bool tm_present = cache::readBool(f);
    if (!tm_present)
      continue;  // empty slot in the source vector
    // Read a TableModel inline.
    std::string template_name = cache::readString(f);
    TableTemplateType template_type =
        static_cast<TableTemplateType>(cache::readU32(f));
    ScaleFactorType sft = static_cast<ScaleFactorType>(cache::readU32(f));
    cache::readU32(f);  // rf_index — discarded; we use position below
    TablePtr table = readTablePtr(f);
    TableTemplate *tt = template_name.empty()
        ? nullptr
        : lib->findTableTemplate(template_name, template_type);
    // Derive (segment, rf) from the slot position so the reader
    // re-creates the source layout exactly. Using the deserialized
    // rf_index here would mis-place models for slots whose source-side
    // value is opaque to the writer (default-constructed empties have
    // rf_index=0 regardless of slot).
    size_t segment = i / RiseFall::index_count;
    const RiseFall *pos_rf = (i % RiseFall::index_count
                                == RiseFall::riseIndex())
        ? RiseFall::rise() : RiseFall::fall();
    TableModel tm(std::move(table), tt, sft, pos_rf);
    rm->setCapacitanceModel(std::move(tm), segment, pos_rf);
  }
  return rm;
}

OutputWaveforms *
readOutputWaveforms(FILE *f)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  uint32_t rf_index = cache::readU32(f);
  const RiseFall *rf = (rf_index == RiseFall::riseIndex())
      ? RiseFall::rise() : RiseFall::fall();
  TableAxisPtr slew_axis = readTableAxis(f);
  TableAxisPtr cap_axis  = readTableAxis(f);
  uint32_t wf_count = cache::readU32(f);
  Table1Seq waveforms;
  waveforms.reserve(wf_count);
  for (uint32_t i = 0; i < wf_count; ++i) {
    TablePtr t = readTablePtr(f);
    // OutputWaveforms takes raw Table* and assumes ownership; we lift
    // out of the shared_ptr (release-style). The cache reader is the
    // sole owner of these freshly-allocated tables.
    waveforms.push_back(t ? new Table(std::move(*t)) : new Table());
  }
  TablePtr ref_table_ptr = readTablePtr(f);
  Table ref_times = ref_table_ptr ? std::move(*ref_table_ptr) : Table();
  return new OutputWaveforms(slew_axis, cap_axis, rf,
                             waveforms, std::move(ref_times));
}

TimingModel *
readArcModel(FILE *f, LibertyLibrary *lib, LibertyCell *cell, bool is_check)
{
  bool present = cache::readBool(f);
  if (!present)
    return nullptr;
  if (is_check) {
    TableModels *check_models = readTableModels(f, lib);
    return new CheckTableModel(cell, check_models);
  }
  TableModels *delay_models = readTableModels(f, lib);
  TableModels *slew_models  = readTableModels(f, lib);
  ReceiverModelPtr receiver_model = readReceiverModel(f, lib);
  OutputWaveforms *output_waveforms = readOutputWaveforms(f);
  if (receiver_model || output_waveforms)
    return new GateTableModel(cell, delay_models, slew_models,
                              receiver_model, output_waveforms);
  return new GateTableModel(cell, delay_models, slew_models);
}

void
readTimingArcSets(FILE *f, LibertyLibrary *lib, LibertyCell *cell,
                  const std::vector<LibertyPort*> &ports)
{
  uint32_t set_count = cache::readU32(f);
  auto resolve_port = [&](uint32_t idx) -> LibertyPort* {
    if (idx == 0xFFFFFFFFu || idx >= ports.size()) return nullptr;
    return ports[idx];
  };
  for (uint32_t s = 0; s < set_count; ++s) {
    LibertyPort *from        = resolve_port(cache::readU32(f));
    LibertyPort *to          = resolve_port(cache::readU32(f));
    LibertyPort *related_out = resolve_port(cache::readU32(f));
    std::string role_name = cache::readString(f);
    bool is_cond_default  = cache::readBool(f);

    const TimingRole *role = TimingRole::find(role_name.c_str());
    bool is_check = role ? role->isTimingCheck() : false;

    TimingArcAttrsPtr attrs = std::make_shared<TimingArcAttrs>();
    attrs->setTimingType(static_cast<TimingType>(cache::readU32(f)));
    attrs->setTimingSense(static_cast<TimingSense>(cache::readU32(f)));
    if (FuncExpr *cond = readFuncExpr(f, ports))
      attrs->setCond(cond);
    std::string sdf_cond_start = cache::readString(f);
    std::string sdf_cond_end   = cache::readString(f);
    if (!sdf_cond_start.empty())
      attrs->setSdfCondStart(sdf_cond_start);
    if (!sdf_cond_end.empty())
      attrs->setSdfCondEnd(sdf_cond_end);
    std::string mode_name  = cache::readString(f);
    std::string mode_value = cache::readString(f);
    if (!mode_name.empty())  attrs->setModeName(mode_name);
    if (!mode_value.empty()) attrs->setModeValue(mode_value);
    attrs->setOcvArcDepth(cache::readFloat(f));

    // Per-RF model. attrs takes ownership of the TimingModel pointers.
    for (auto rf : RiseFall::range()) {
      TimingModel *m = readArcModel(f, lib, cell, is_check);
      if (m) attrs->setModel(rf, m);
    }

    TimingArcSet *set = cell->makeTimingArcSet(from, to, related_out,
                                               role, attrs);
    set->setIsCondDefault(is_cond_default);

    // Arcs. NB: TimingArc's ctor calls set->addTimingArc(this), so
    // constructing the arc is sufficient -- a second addTimingArc()
    // call here would double-register the pointer and trigger a
    // double-free when the arc set is later destructed.
    uint32_t arc_count = cache::readU32(f);
    for (uint32_t a = 0; a < arc_count; ++a) {
      std::string from_rf_name = cache::readString(f);
      std::string to_rf_name   = cache::readString(f);
      const Transition *from_t = Transition::find(from_rf_name);
      const Transition *to_t   = Transition::find(to_rf_name);
      // Explicit attrs-slot index: writer recorded which rf slot the
      // original arc's model pointer aliased. Inferring from from_t
      // would mis-assign negative-unate and tristate arcs.
      uint32_t slot = cache::readU32(f);
      TimingModel *model = nullptr;
      if (slot == 0)      model = attrs->model(RiseFall::rise());
      else if (slot == 1) model = attrs->model(RiseFall::fall());
      new TimingArc(set, from_t, to_t, model);
    }
  }
}

void
readOcvDerates(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::OcvDerates);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    OcvDerate *derate = lib->makeOcvDerate(name);
    for (auto rf : RiseFall::range()) {
      for (auto el : EarlyLate::range()) {
        for (size_t pt = 0; pt < path_type_count; ++pt) {
          TablePtr table = readTablePtr(f);
          if (table)
            derate->setDerateTable(rf, el, static_cast<PathType>(pt),
                                   std::move(table));
        }
      }
    }
  }
  std::string default_name = cache::readString(f);
  if (!default_name.empty())
    lib->setDefaultOcvDerate(lib->findOcvDerate(default_name));
}

void
readDriverWaveforms(FILE *f, LibertyLibrary *lib)
{
  cache::expectSectionId(f, SectionId::DriverWaveforms);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    TablePtr waveforms = readTablePtr(f);
    lib->makeDriverWaveform(name, waveforms);
  }
}

void
readCells(FILE *f, LibertyLibrary *lib, StaState *sta)
{
  cache::expectSectionId(f, SectionId::Cells);
  uint32_t n = cache::readU32(f);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    std::string filename = cache::readString(f);
    // LibertyCell's ctor does not register with its library — the
    // builder pattern (see LibertyBuilder::makeCell) does that
    // explicitly. Match that flow so `findLibertyCell` works.
    LibertyCell *cell = new LibertyCell(lib, name, filename);
    lib->addCell(cell);

    cell->setArea(cache::readFloat(f));
    cell->setDontUse(cache::readBool(f));
    cell->setIsMacro(cache::readBool(f));
    cell->setIsMemory(cache::readBool(f));
    cell->setIsPad(cache::readBool(f));
    cell->setIsClockCell(cache::readBool(f));
    cell->setIsLevelShifter(cache::readBool(f));
    cell->setLevelShifterType(static_cast<LevelShifterType>(cache::readU32(f)));
    cell->setIsIsolationCell(cache::readBool(f));
    cell->setAlwaysOn(cache::readBool(f));
    cell->setSwitchCellType(static_cast<SwitchCellType>(cache::readU32(f)));
    cell->setInterfaceTiming(cache::readBool(f));
    cell->setClockGateType(static_cast<ClockGateType>(cache::readU32(f)));
    cell->setHasInferedRegTimingArcs(cache::readBool(f));

    float leakage = cache::readFloat(f);
    bool leakage_exists = cache::readBool(f);
    if (leakage_exists)
      cell->setLeakagePower(leakage);

    cell->setOcvArcDepth(cache::readFloat(f));
    cell->setFootprint(cache::readString(f));
    cell->setUserFunctionClass(cache::readString(f));

    std::string sf_name = cache::readString(f);
    if (!sf_name.empty())
      cell->setScaleFactors(lib->findScaleFactors(sf_name));

    // Pass 1: re-create ports in cache order (FuncExprs in pass 2 may
    // reference any port, including outputs defined "later" in the
    // cell, so all ports must exist before any FuncExpr is read).
    LibertyBuilder builder(/*debug=*/nullptr, /*report=*/nullptr);
    auto ports = readCellPortHeaders(f, cell, builder);
    readCellPortDetails(f, lib, ports);

    // Helpers for the structures that follow.
    auto resolve_port = [&](uint32_t idx) -> LibertyPort* {
      if (idx == 0xFFFFFFFFu || idx >= ports.size()) return nullptr;
      return ports[idx];
    };

    // Per-cell BusDcl.
    uint32_t bus_dcl_count = cache::readU32(f);
    for (uint32_t b = 0; b < bus_dcl_count; ++b) {
      std::string bd_name = cache::readString(f);
      int from = static_cast<int>(cache::readI64(f));
      int to   = static_cast<int>(cache::readI64(f));
      cell->makeBusDcl(bd_name, from, to);
    }

    // Per-cell ModeDef. cond expressions can reference cell ports.
    uint32_t mode_count = cache::readU32(f);
    for (uint32_t m = 0; m < mode_count; ++m) {
      std::string mode_name = cache::readString(f);
      ModeDef *mode = cell->makeModeDef(mode_name);
      uint32_t value_count = cache::readU32(f);
      for (uint32_t v = 0; v < value_count; ++v) {
        std::string value_name = cache::readString(f);
        std::string sdf_cond = cache::readString(f);
        FuncExpr *cond = readFuncExpr(f, ports);
        // ModeDef::defineValue uses try_emplace and always returns a
        // valid pointer (existing entries return the existing slot).
        ModeValueDef *vd = mode->defineValue(value_name);
        vd->setSdfCond(sdf_cond);
        if (cond)
          vd->setCond(cond);
      }
    }

    // Sequentials. makeSequential takes ownership of the FuncExprs
    // (deletes them after copying via bitSubExpr); call with size=1
    // since the cache already stores per-bit expressions.
    uint32_t seq_count = cache::readU32(f);
    for (uint32_t s = 0; s < seq_count; ++s) {
      bool is_register = cache::readBool(f);
      FuncExpr *clk    = readFuncExpr(f, ports);
      FuncExpr *data   = readFuncExpr(f, ports);
      FuncExpr *clear  = readFuncExpr(f, ports);
      FuncExpr *preset = readFuncExpr(f, ports);
      LogicValue cpo  = static_cast<LogicValue>(cache::readU32(f));
      LogicValue cpoi = static_cast<LogicValue>(cache::readU32(f));
      LibertyPort *out     = resolve_port(cache::readU32(f));
      LibertyPort *out_inv = resolve_port(cache::readU32(f));
      cell->makeSequential(/*size=*/1, is_register,
                           clk, data, clear, preset,
                           cpo, cpoi, out, out_inv);
    }

    // Statetable (optional).
    bool has_statetable = cache::readBool(f);
    if (has_statetable) {
      uint32_t in_n = cache::readU32(f);
      LibertyPortSeq in_ports;
      in_ports.reserve(in_n);
      for (uint32_t i = 0; i < in_n; ++i)
        in_ports.push_back(resolve_port(cache::readU32(f)));
      uint32_t int_n = cache::readU32(f);
      LibertyPortSeq int_ports;
      int_ports.reserve(int_n);
      for (uint32_t i = 0; i < int_n; ++i)
        int_ports.push_back(resolve_port(cache::readU32(f)));
      uint32_t row_n = cache::readU32(f);
      StatetableRows rows;
      rows.reserve(row_n);
      for (uint32_t r = 0; r < row_n; ++r) {
        StateInputValues iv;
        uint32_t iv_n = cache::readU32(f);
        iv.reserve(iv_n);
        for (uint32_t i = 0; i < iv_n; ++i)
          iv.push_back(static_cast<StateInputValue>(cache::readU32(f)));
        StateInternalValues cv;
        uint32_t cv_n = cache::readU32(f);
        cv.reserve(cv_n);
        for (uint32_t i = 0; i < cv_n; ++i)
          cv.push_back(static_cast<StateInternalValue>(cache::readU32(f)));
        StateInternalValues nv;
        uint32_t nv_n = cache::readU32(f);
        nv.reserve(nv_n);
        for (uint32_t i = 0; i < nv_n; ++i)
          nv.push_back(static_cast<StateInternalValue>(cache::readU32(f)));
        rows.emplace_back(iv, cv, nv);
      }
      cell->makeStatetable(in_ports, int_ports, rows);
    }

    // Timing arc sets.
    readTimingArcSets(f, lib, cell, ports);

    // === Internal power =============================================
    auto resolve_port_5 = [&](uint32_t idx) -> LibertyPort* {
      if (idx == 0xFFFFFFFFu || idx >= ports.size()) return nullptr;
      return ports[idx];
    };
    uint32_t ip_count = cache::readU32(f);
    for (uint32_t i = 0; i < ip_count; ++i) {
      LibertyPort *port           = resolve_port_5(cache::readU32(f));
      LibertyPort *related_port   = resolve_port_5(cache::readU32(f));
      LibertyPort *related_pg_pin = resolve_port_5(cache::readU32(f));
      FuncExpr *when_raw          = readFuncExpr(f, ports);
      std::shared_ptr<FuncExpr> when(when_raw);
      // InternalPowerModels is std::array<InternalPowerModel, 2>.
      InternalPowerModels models{
          InternalPowerModel(),
          InternalPowerModel()};
      for (auto rf : RiseFall::range()) {
        TableModel *tm = readTableModel(f, lib);
        if (tm)
          models[rf->index()] =
              InternalPowerModel(std::shared_ptr<TableModel>(tm));
      }
      cell->makeInternalPower(port, related_port, related_pg_pin,
                              when, models);
    }

    // === Leakage power ==============================================
    uint32_t lp_count = cache::readU32(f);
    for (uint32_t i = 0; i < lp_count; ++i) {
      LibertyPort *related_pg_port = resolve_port_5(cache::readU32(f));
      FuncExpr *when               = readFuncExpr(f, ports);
      float power                  = cache::readFloat(f);
      cell->makeLeakagePower(related_pg_port, when, power);
    }

    // Build the port→arc-set indices that timingArcSetsTo and
    // findTimingArcSet require. infer_latches=false because the
    // cached library was already finalized at write time.
    cell->finish(/*infer_latches=*/false,
                 sta ? sta->report() : nullptr,
                 sta ? sta->debug()  : nullptr);
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

  Network *network = sta ? sta->networkReader() : nullptr;
  LibertyLibrary *lib = network
      ? network->makeLibertyLibrary(name, lib_filename)
      : new LibertyLibrary(name, lib_filename);
  lib->setDelayModelType(delay_model);

  readLibraryScalars(f, lib);
  readUnits(f, lib);
  readBusDcls(f, lib);
  readOperatingConditions(f, lib);
  readScaleFactors(f, lib);
  readSupplyVoltages(f, lib);
  readTableTemplates(f, lib);
  readOcvDerates(f, lib);
  readDriverWaveforms(f, lib);
  readCells(f, lib, sta);

  cache::expectSectionId(f, SectionId::EndMarker);
  return lib;
}

} // namespace sta
