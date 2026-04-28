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
#include <system_error>

#include "Error.hh"
#include "FuncExpr.hh"
#include "Liberty.hh"
#include "LibertyCacheFormat.hh"
#include "MinMaxValues.hh"
#include "PortDirection.hh"
#include "RiseFallMinMax.hh"
#include "StaConfig.hh"
#include "TableModel.hh"
#include "Transition.hh"

namespace sta {

using cache::SectionId;

namespace {

// Capture (size, mtime) of the source .lib path stamped in the cache
// header. If the file is unreachable at write time, we record zero
// metadata; the reader will be unable to validate staleness in that
// case, which is consistent with users intentionally dropping the
// source after caching.
struct SourceMetadata
{
  uint64_t size = 0;
  int64_t mtime = 0;
};

SourceMetadata
sourceMetadata(const std::string &filename)
{
  SourceMetadata md;
  if (filename.empty())
    return md;
  std::error_code ec;
  std::filesystem::path path{filename};
  auto sz = std::filesystem::file_size(path, ec);
  if (!ec)
    md.size = static_cast<uint64_t>(sz);
  auto ftime = std::filesystem::last_write_time(path, ec);
  if (!ec)
    md.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                 ftime.time_since_epoch()).count();
  return md;
}

void
writeHeader(FILE *f, const LibertyLibrary *lib)
{
  cache::writeU32(f, cache::kMagic);
  cache::writeU32(f, cache::kFormatVersion);
  cache::writeU32(f, cache::kEndianSentinel);
  cache::writeU32(f, cache::kFlagsNone);
  cache::writeString(f, STA_VERSION);
  cache::writeString(f, lib->filename());
  SourceMetadata md = sourceMetadata(lib->filename());
  cache::writeU64(f, md.size);
  cache::writeI64(f, md.mtime);
}

void
writeLibraryHeader(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::LibraryHeader);
  cache::writeString(f, lib->name());
  cache::writeString(f, lib->filename());
  cache::writeU32(f, static_cast<uint32_t>(lib->delayModelType()));
}

void
writeLibraryScalars(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::LibraryScalars);

  cache::writeFloat(f, lib->nominalProcess());
  cache::writeFloat(f, lib->nominalVoltage());
  cache::writeFloat(f, lib->nominalTemperature());
  cache::writeFloat(f, lib->ocvArcDepth());

  cache::writeFloat(f, lib->defaultInputPinCap());
  cache::writeFloat(f, lib->defaultOutputPinCap());
  cache::writeFloat(f, lib->defaultBidirectPinCap());
  cache::writeFloat(f, lib->slewDerateFromLibrary());

  for (auto rf : RiseFall::range())
    cache::writeFloat(f, lib->inputThreshold(rf));
  for (auto rf : RiseFall::range())
    cache::writeFloat(f, lib->outputThreshold(rf));
  for (auto rf : RiseFall::range())
    cache::writeFloat(f, lib->slewLowerThreshold(rf));
  for (auto rf : RiseFall::range())
    cache::writeFloat(f, lib->slewUpperThreshold(rf));

  // Default intrinsic and pin resistances are RiseFallValues so that
  // each rise/fall slot tracks both a value and a "present" flag.
  for (auto rf : RiseFall::range()) {
    float value;
    bool exists;
    lib->defaultIntrinsic(rf, value, exists);
    cache::writeFloat(f, value);
    cache::writeBool(f, exists);
  }
  for (auto rf : RiseFall::range()) {
    float value;
    bool exists;
    lib->defaultBidirectPinRes(rf, value, exists);
    cache::writeFloat(f, value);
    cache::writeBool(f, exists);
  }
  for (auto rf : RiseFall::range()) {
    float value;
    bool exists;
    lib->defaultOutputPinRes(rf, value, exists);
    cache::writeFloat(f, value);
    cache::writeBool(f, exists);
  }

  // Library-wide caps/limits: the four "default_max_*" + fanout_load.
  // Each is a (float, bool) pair tracking whether the .lib actually
  // specified the field.
  float fval;
  bool exists;
  lib->defaultFanoutLoad(fval, exists);
  cache::writeFloat(f, fval);
  cache::writeBool(f, exists);

  lib->defaultMaxCapacitance(fval, exists);
  cache::writeFloat(f, fval);
  cache::writeBool(f, exists);

  lib->defaultMaxFanout(fval, exists);
  cache::writeFloat(f, fval);
  cache::writeBool(f, exists);

  lib->defaultMaxSlew(fval, exists);
  cache::writeFloat(f, fval);
  cache::writeBool(f, exists);
}

void
writeBusDcls(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::BusDcls);
  const BusDclSeq dcls = lib->busDcls();
  cache::writeU32(f, static_cast<uint32_t>(dcls.size()));
  for (const BusDcl *bd : dcls) {
    cache::writeString(f, bd->name());
    cache::writeI64(f, bd->from());
    cache::writeI64(f, bd->to());
  }
}

void
writeOperatingConditions(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::OperatingConditions);
  const OperatingConditionsMap &op_map = lib->operatingConditionsMap();
  cache::writeU32(f, static_cast<uint32_t>(op_map.size()));
  for (const auto &[name, op] : op_map) {
    cache::writeString(f, name);
    cache::writeFloat(f, op.process());
    cache::writeFloat(f, op.voltage());
    cache::writeFloat(f, op.temperature());
    cache::writeU32(f, static_cast<uint32_t>(op.wireloadTree()));
  }
  // Default by name (empty if none).
  const OperatingConditions *def = lib->defaultOperatingConditions();
  cache::writeString(f, def ? def->name() : std::string{});
}

void
writeScaleFactors(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::ScaleFactors);
  const ScaleFactorsMap &sf_map = lib->scaleFactorsMap();
  cache::writeU32(f, static_cast<uint32_t>(sf_map.size()));
  for (const auto &[name, sf] : sf_map) {
    cache::writeString(f, name);
    // The 3D scales array is dense; writers cast away const because
    // the existing scale() accessor isn't const. We only read.
    auto &sf_mut = const_cast<ScaleFactors&>(sf);
    for (int t = 0; t < scale_factor_type_count; ++t) {
      for (int p = 0; p < scale_factor_pvt_count; ++p) {
        for (auto rf : RiseFall::range())
          cache::writeFloat(f, sf_mut.scale(static_cast<ScaleFactorType>(t),
                                            static_cast<ScaleFactorPvt>(p), rf));
      }
    }
  }
  const ScaleFactors *def = lib->scaleFactors();
  cache::writeString(f, def ? def->name() : std::string{});
}

void
writeSupplyVoltages(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::SupplyVoltages);
  const SupplyVoltageMap &sv_map = lib->supplyVoltageMap();
  cache::writeU32(f, static_cast<uint32_t>(sv_map.size()));
  for (const auto &[name, voltage] : sv_map) {
    cache::writeString(f, name);
    cache::writeFloat(f, voltage);
  }
}

// Serialize a TableAxis as (variable, values[]). The shared_ptr wrapper
// is irrelevant on disk — every cache-loaded axis is freshly allocated.
void
writeTableAxis(FILE *f, const TableAxis *axis)
{
  cache::writeBool(f, axis != nullptr);
  if (axis == nullptr)
    return;
  cache::writeU32(f, static_cast<uint32_t>(axis->variable()));
  const FloatSeq &values = axis->values();
  cache::writeFloatArray(f, values.data(), values.size());
}

void
writeTableTemplates(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::TableTemplates);
  TableTemplateSeq templates = lib->tableTemplates();
  cache::writeU32(f, static_cast<uint32_t>(templates.size()));
  for (const TableTemplate *tt : templates) {
    cache::writeU32(f, static_cast<uint32_t>(tt->type()));
    cache::writeString(f, tt->name());
    writeTableAxis(f, tt->axis1());
    writeTableAxis(f, tt->axis2());
    writeTableAxis(f, tt->axis3());
  }
}

// === Port + FuncExpr helpers (commit 3b) =============================
//
// FuncExpr trees can reference any LibertyPort of the owning cell. To
// keep the format position-independent, every cell writes its full port
// list first (just names, in iteration order) and we resolve port refs
// to within-cell indices. The reader rebuilds the same vector and looks
// up by index.

using PortIndexMap = std::unordered_map<const LibertyPort*, uint32_t>;

void
writeFuncExpr(FILE *f, const FuncExpr *expr, const PortIndexMap &port_idx)
{
  if (expr == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  cache::writeU32(f, static_cast<uint32_t>(expr->op()));
  switch (expr->op()) {
  case FuncExpr::Op::port: {
    auto it = port_idx.find(expr->port());
    cache::writeU32(f, it == port_idx.end() ? 0xFFFFFFFFu : it->second);
    break;
  }
  case FuncExpr::Op::not_:
    writeFuncExpr(f, expr->left(), port_idx);
    break;
  case FuncExpr::Op::or_:
  case FuncExpr::Op::and_:
  case FuncExpr::Op::xor_:
    writeFuncExpr(f, expr->left(), port_idx);
    writeFuncExpr(f, expr->right(), port_idx);
    break;
  case FuncExpr::Op::one:
  case FuncExpr::Op::zero:
    break;
  }
}

// const RiseFall* encoded as 0=null, 1=rise, 2=fall.
void
writeRfPtr(FILE *f, const RiseFall *rf)
{
  uint8_t code = 0;
  if (rf == RiseFall::rise()) code = 1;
  else if (rf == RiseFall::fall()) code = 2;
  uint32_t v = code;
  cache::writeU32(f, v);
}

// Bit-packed flag layout for LibertyPort. Bit positions are stable
// across format versions; future flags are appended at higher bits.
enum PortFlagBit : uint32_t {
  kIsClk           = 1u << 0,
  kIsRegClk        = 1u << 1,
  kIsRegOutput     = 1u << 2,
  kIsLatchData     = 1u << 3,
  kIsCheckClk      = 1u << 4,
  kIsClkGateClk    = 1u << 5,
  kIsClkGateEnable = 1u << 6,
  kIsClkGateOut    = 1u << 7,
  kIsPllFeedback   = 1u << 8,
  kIsolDataIn      = 1u << 9,
  kIsolEnable      = 1u << 10,
  kLvlShiftData    = 1u << 11,
  kIsSwitch        = 1u << 12,
  kIsPad           = 1u << 13,
};

uint32_t
collectPortFlags(const LibertyPort *p)
{
  uint32_t flags = 0;
  if (p->isClock())            flags |= kIsClk;
  if (p->isRegClk())           flags |= kIsRegClk;
  if (p->isRegOutput())        flags |= kIsRegOutput;
  if (p->isLatchData())        flags |= kIsLatchData;
  if (p->isCheckClk())         flags |= kIsCheckClk;
  if (p->isClockGateClock())   flags |= kIsClkGateClk;
  if (p->isClockGateEnable())  flags |= kIsClkGateEnable;
  if (p->isClockGateOut())     flags |= kIsClkGateOut;
  if (p->isPllFeedback())      flags |= kIsPllFeedback;
  if (p->isolationCellData())  flags |= kIsolDataIn;
  if (p->isolationCellEnable())flags |= kIsolEnable;
  if (p->levelShifterData())   flags |= kLvlShiftData;
  if (p->isSwitch())           flags |= kIsSwitch;
  if (p->isPad())              flags |= kIsPad;
  return flags;
}

// Pass 1: write each port's identity (just enough so the reader can
// construct the LibertyPorts in the same order). Returns the within-
// cell index map that pass 2 uses for FuncExpr port references.
PortIndexMap
writeCellPortHeaders(FILE *f, const LibertyCell *cell)
{
  PortIndexMap idx_map;
  std::vector<const LibertyPort*> ports;
  // Use the bit-iterator so bus members are visited as individual
  // ports — bus/bundle support lands in 3c, but iterating bits keeps
  // the index assignment consistent with what the reader recreates.
  LibertyCellPortBitIterator iter(cell);
  while (iter.hasNext())
    ports.push_back(iter.next());
  cache::writeU32(f, static_cast<uint32_t>(ports.size()));
  for (uint32_t i = 0; i < ports.size(); ++i) {
    cache::writeString(f, ports[i]->name());
    cache::writeString(f, ports[i]->direction()
                          ? std::string{ports[i]->direction()->name()}
                          : std::string{});
    idx_map[ports[i]] = i;
  }
  return idx_map;
}

void
writeCellPortDetails(FILE *f, const LibertyCell *cell,
                     const PortIndexMap &port_idx)
{
  std::vector<const LibertyPort*> ports;
  LibertyCellPortBitIterator iter(cell);
  while (iter.hasNext())
    ports.push_back(iter.next());

  for (const LibertyPort *p : ports) {
    cache::writeU32(f, static_cast<uint32_t>(p->pwrGndType()));
    cache::writeString(f, p->voltageName());
    cache::writeU32(f, static_cast<uint32_t>(p->scanSignalType()));

    // capacitance (RiseFallMinMax). LibertyPort caches it inline; we
    // pull each (rf, mm) slot through the public getter that returns
    // (value, exists).
    for (auto rf : RiseFall::range()) {
      for (auto mm : MinMax::range()) {
        float val = 0;
        bool exists = false;
        p->capacitance(rf, mm, val, exists);
        cache::writeFloat(f, val);
        cache::writeBool(f, exists);
      }
    }

    // Limits.
    {
      float val;
      bool exists;
      p->slewLimit(MinMax::min(), val, exists);
      cache::writeFloat(f, val); cache::writeBool(f, exists);
      p->slewLimit(MinMax::max(), val, exists);
      cache::writeFloat(f, val); cache::writeBool(f, exists);
      p->capacitanceLimit(MinMax::min(), val, exists);
      cache::writeFloat(f, val); cache::writeBool(f, exists);
      p->capacitanceLimit(MinMax::max(), val, exists);
      cache::writeFloat(f, val); cache::writeBool(f, exists);
      p->fanoutLimit(MinMax::min(), val, exists);
      cache::writeFloat(f, val); cache::writeBool(f, exists);
      p->fanoutLimit(MinMax::max(), val, exists);
      cache::writeFloat(f, val); cache::writeBool(f, exists);
    }
    {
      float fanout_load = 0;
      bool fanout_load_exists = false;
      p->fanoutLoad(fanout_load, fanout_load_exists);
      cache::writeFloat(f, fanout_load);
      cache::writeBool(f, fanout_load_exists);
    }

    // min_period.
    {
      float val = 0;
      bool exists = false;
      p->minPeriod(val, exists);
      cache::writeFloat(f, val);
      cache::writeBool(f, exists);
    }
    // min_pulse_width per RiseFall.
    for (auto rf : RiseFall::range()) {
      float val = 0;
      bool exists = false;
      p->minPulseWidth(rf, val, exists);
      cache::writeFloat(f, val);
      cache::writeBool(f, exists);
    }

    writeRfPtr(f, p->pulseClkTrigger());
    writeRfPtr(f, p->pulseClkSense());

    // Intra-cell related-port references by within-cell index.
    auto write_port_ref = [&](const LibertyPort *ref) {
      if (ref == nullptr) {
        cache::writeU32(f, 0xFFFFFFFFu);
        return;
      }
      auto it = port_idx.find(ref);
      cache::writeU32(f, it == port_idx.end() ? 0xFFFFFFFFu : it->second);
    };
    write_port_ref(p->relatedGroundPort());
    write_port_ref(p->relatedPowerPort());

    cache::writeU32(f, collectPortFlags(p));

    writeFuncExpr(f, p->function(), port_idx);
    writeFuncExpr(f, p->tristateEnable(), port_idx);
  }
}

void
writeCells(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::Cells);

  // Two-pass: count, then iterate. The iterator interface doesn't
  // expose a size, so the count is computed via a quick walk.
  uint32_t count = 0;
  {
    LibertyCellIterator iter(lib);
    while (iter.hasNext()) {
      iter.next();
      ++count;
    }
  }
  cache::writeU32(f, count);

  LibertyCellIterator iter(lib);
  while (iter.hasNext()) {
    const LibertyCell *cell = iter.next();
    cache::writeString(f, cell->name());
    cache::writeString(f, cell->filename());
    cache::writeFloat(f, cell->area());
    cache::writeBool(f, cell->dontUse());
    cache::writeBool(f, cell->isMacro());
    cache::writeBool(f, cell->isMemory());
    cache::writeBool(f, cell->isPad());
    cache::writeBool(f, cell->isClockCell());
    cache::writeBool(f, cell->isLevelShifter());
    cache::writeU32(f, static_cast<uint32_t>(cell->levelShifterType()));
    cache::writeBool(f, cell->isIsolationCell());
    cache::writeBool(f, cell->alwaysOn());
    cache::writeU32(f, static_cast<uint32_t>(cell->switchCellType()));
    cache::writeBool(f, cell->interfaceTiming());

    // ClockGateType is reachable only via the four predicate getters
    // (isClockGateLatchPosedge / Negedge / Other / isClockGate); collapse
    // them back into the underlying enum value for serialization.
    ClockGateType cgt = ClockGateType::none;
    if (cell->isClockGateLatchPosedge())      cgt = ClockGateType::latch_posedge;
    else if (cell->isClockGateLatchNegedge()) cgt = ClockGateType::latch_negedge;
    else if (cell->isClockGateOther())        cgt = ClockGateType::other;
    cache::writeU32(f, static_cast<uint32_t>(cgt));

    cache::writeBool(f, cell->hasInferedRegTimingArcs());

    float leakage = 0.0F;
    bool leakage_exists = false;
    cell->leakagePower(leakage, leakage_exists);
    cache::writeFloat(f, leakage);
    cache::writeBool(f, leakage_exists);

    cache::writeFloat(f, cell->ocvArcDepth());
    cache::writeString(f, cell->footprint());
    cache::writeString(f, cell->userFunctionClass());

    // Per-cell scale factor override (library-level entries already
    // serialized in section ScaleFactors); store by name so the reader
    // can rebind to the freshly-loaded library map.
    const ScaleFactors *cell_sf = cell->scaleFactors();
    cache::writeString(f, cell_sf ? cell_sf->name() : std::string{});

    // Two-pass: port headers first so FuncExpr port refs in pass 2
    // can resolve to within-cell indices.
    PortIndexMap port_idx = writeCellPortHeaders(f, cell);
    writeCellPortDetails(f, cell, port_idx);
  }
}

} // namespace

void
writeLibertyCache(LibertyLibrary *lib,
                  const char *filename,
                  StaState * /*sta*/)
{
  std::unique_ptr<FILE, int(*)(FILE*)> out(fopen(filename, "wb"), &fclose);
  if (!out)
    throw FileNotWritable(filename);

  FILE *f = out.get();
  writeHeader(f, lib);
  writeLibraryHeader(f, lib);
  writeLibraryScalars(f, lib);
  writeBusDcls(f, lib);
  writeOperatingConditions(f, lib);
  writeScaleFactors(f, lib);
  writeSupplyVoltages(f, lib);
  writeTableTemplates(f, lib);
  writeCells(f, lib);
  cache::writeSectionId(f, SectionId::EndMarker);
}

} // namespace sta
