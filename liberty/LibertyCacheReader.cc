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
#include "LibertyBuilder.hh"
#include "Network.hh"
#include "PortDirection.hh"
#include "RiseFallMinMax.hh"
#include "StaConfig.hh"
#include "TableModel.hh"
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

// === Port + FuncExpr helpers (commit 3b) =============================
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

// Pass 1: re-create the cell's ports in the same order the writer
// emitted them. Returns the per-cell port vector that pass 2 uses for
// FuncExpr port-reference resolution.
std::vector<LibertyPort*>
readCellPortHeaders(FILE *f, LibertyCell *cell, LibertyBuilder &builder)
{
  uint32_t n = cache::readU32(f);
  std::vector<LibertyPort*> ports;
  ports.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    std::string name = cache::readString(f);
    std::string dir_name = cache::readString(f);
    LibertyPort *port = builder.makePort(cell, name);
    if (!dir_name.empty()) {
      PortDirection *dir = PortDirection::find(dir_name.c_str());
      if (dir) port->setDirection(dir);
    }
    ports.push_back(port);
  }
  return ports;
}

void
readCellPortDetails(FILE *f, const std::vector<LibertyPort*> &ports)
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
  }
}

void
readCells(FILE *f, LibertyLibrary *lib)
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
    readCellPortDetails(f, ports);
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
  readBusDcls(f, lib);
  readOperatingConditions(f, lib);
  readScaleFactors(f, lib);
  readSupplyVoltages(f, lib);
  readTableTemplates(f, lib);
  readCells(f, lib);

  cache::expectSectionId(f, SectionId::EndMarker);
  return lib;
}

} // namespace sta
