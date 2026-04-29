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
#include "InternalPower.hh"
#include "LeakagePower.hh"
#include "Liberty.hh"
#include "LibertyCacheFormat.hh"
#include "MinMaxValues.hh"
#include "NetworkClass.hh"
#include "PortDirection.hh"
#include "RiseFallMinMax.hh"
#include "Sequential.hh"
#include "StaConfig.hh"
#include "TableModel.hh"
#include "TimingArc.hh"
#include "TimingModel.hh"
#include "TimingRole.hh"
#include "Transition.hh"
#include "Units.hh"

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

// Forward declarations for the CCS helpers in the timing-arc block;
// the per-port writers below reference them too (port-level
// ReceiverModel / DriverWaveform refs were added in commit 4c).
void writeTableModel(FILE *f, const TableModel *model);
void writeReceiverModel(FILE *f, const ReceiverModel *rm);

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

// Liberty's `time_unit`, `capacitive_load_unit`, etc. are user-facing
// scale + suffix metadata — STA stores values internally in SI base
// units, so the unit struct only affects formatting (write_liberty,
// report tables). Without this section, a cache-loaded library renders
// every quantity in SI: `1ns` becomes `1s`, `1pF` becomes `1F`, and
// every value formatted with N digits truncates to zero.
void
writeUnits(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::Units);
  const Units *units = lib->units();
  auto write_unit = [&](const Unit *u) {
    cache::writeFloat(f, u->scale());
    cache::writeString(f, u->suffix());
    cache::writeU32(f, static_cast<uint32_t>(u->digits()));
  };
  write_unit(units->timeUnit());
  write_unit(units->resistanceUnit());
  write_unit(units->capacitanceUnit());
  write_unit(units->voltageUnit());
  write_unit(units->currentUnit());
  write_unit(units->powerUnit());
  write_unit(units->distanceUnit());
  write_unit(units->scalarUnit());
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
  cache::writeString(f, def ? def->name() : std::string_view{});
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
  cache::writeString(f, def ? def->name() : std::string_view{});
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

// === Table serialization (commit 4a) =================================
//
// Liberty Tables are 0/1/2/3-D float arrays attached to up to three
// TableAxis values (slew, capacitance, etc.). Larger tables (CCS
// timing) dominate the on-disk size of a real .lib; each row is
// streamed as a single length-prefixed float array via writeFloatArray
// so the parser can later reload it with one fread per row.
//
// The "present" bit lets call sites pass a nullable TablePtr / Table*
// without an extra wrapper.

void
writeTable(FILE *f, const Table *table)
{
  if (table == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  uint32_t order = static_cast<uint32_t>(table->order());
  cache::writeU32(f, order);
  switch (order) {
  case 0:
    // Order-0 stores a single float in value_; the public accessor is
    // value(index1=0, index2=0, index3=0).
    cache::writeFloat(f, table->value(0, 0, 0));
    break;
  case 1: {
    writeTableAxis(f, table->axis1());
    const FloatSeq *values = table->values();
    if (values)
      cache::writeFloatArray(f, values->data(), values->size());
    else
      cache::writeFloatArray(f, nullptr, 0);
    break;
  }
  case 2:
  case 3: {
    writeTableAxis(f, table->axis1());
    writeTableAxis(f, table->axis2());
    if (order == 3)
      writeTableAxis(f, table->axis3());
    // values_table_ is shaped as either axis1.size rows × axis2.size
    // cols (order 2), or axis1*axis2 rows × axis3 cols (order 3).
    // Storing the row count + each row's float[] preserves both
    // layouts without the reader needing to know the shape.
    const FloatTable *t = const_cast<Table*>(table)->values3();
    cache::writeU32(f, t ? static_cast<uint32_t>(t->size()) : 0u);
    if (t) {
      for (const FloatSeq &row : *t)
        cache::writeFloatArray(f, row.data(), row.size());
    }
    break;
  }
  default:
    cache::error(sta::format("liberty cache: unsupported table order {}", order));
  }
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

// Per-port structural kind. Stored as a u8-in-u32 so the format can
// add new kinds (e.g., for arrayed bundles) without bumping version.
enum PortKind : uint32_t {
  kPortScalar       = 0,  // standalone or bundle-member scalar
  kPortBusParent    = 1,  // bus envelope; followed by N BUS_BIT entries
  kPortBusBit       = 2,  // a bit member of the most recently emitted bus parent
  kPortBundleParent = 3,  // bundle envelope; references already-listed members by flat index
};

// Build the canonical flat port list together with a parallel kinds
// vector. Walk the cell's top-level port iter: scalar ports map 1:1;
// bus parents are emitted with their bit members inlined immediately
// after; bundle parents come last in `cell->ports_` order (their
// members were already emitted earlier as scalars). FuncExpr /
// related-port indices use this exact ordering.
struct PortEntry { const LibertyPort *port; PortKind kind; };

std::vector<PortEntry>
collectPorts(const LibertyCell *cell)
{
  std::vector<PortEntry> ports;
  LibertyCellPortIterator iter(cell);
  while (iter.hasNext()) {
    const LibertyPort *p = iter.next();
    PortKind k = kPortScalar;
    if (p->isBus())         k = kPortBusParent;
    else if (p->isBundle()) k = kPortBundleParent;
    ports.push_back({p, k});
    if (k == kPortBusParent) {
      LibertyPortMemberIterator m(p);
      while (m.hasNext())
        ports.push_back({m.next(), kPortBusBit});
    }
  }
  return ports;
}

// Pass 1: write each port's identity + structural shape (just enough
// so the reader can construct the LibertyPorts in the same order, and
// rebuild bus parent / bit member relationships and bundle wrapping).
// Returns the within-cell index map that pass 2 uses for FuncExpr
// port references.
std::pair<PortIndexMap, std::vector<PortEntry>>
writeCellPortHeaders(FILE *f, const LibertyCell *cell)
{
  PortIndexMap idx_map;
  std::vector<PortEntry> ports = collectPorts(cell);
  for (uint32_t i = 0; i < ports.size(); ++i)
    idx_map[ports[i].port] = i;

  cache::writeU32(f, static_cast<uint32_t>(ports.size()));
  for (const PortEntry &e : ports) {
    const LibertyPort *p = e.port;
    cache::writeU32(f, static_cast<uint32_t>(e.kind));
    cache::writeString(f, p->name());
    cache::writeString(f, p->direction() ? p->direction()->name()
                                         : std::string_view{});

    if (e.kind == kPortBusParent) {
      cache::writeI64(f, p->fromIndex());
      cache::writeI64(f, p->toIndex());
      const BusDcl *bd = p->busDcl();
      cache::writeString(f, bd ? bd->name() : std::string_view{});
    }
    else if (e.kind == kPortBundleParent) {
      // Members were already emitted as scalar entries earlier in
      // this loop (cell->ports_ order). Store their flat-list indices
      // so the reader can rebuild the ConcretePortSeq for makeBundlePort.
      LibertyPortMemberIterator m(p);
      std::vector<uint32_t> member_indices;
      while (m.hasNext()) {
        const LibertyPort *mp = m.next();
        auto it = idx_map.find(mp);
        member_indices.push_back(it == idx_map.end() ? 0xFFFFFFFFu : it->second);
      }
      cache::writeU32(f, static_cast<uint32_t>(member_indices.size()));
      for (uint32_t mi : member_indices)
        cache::writeU32(f, mi);
    }
  }
  return {std::move(idx_map), std::move(ports)};
}

void
writeCellPortDetails(FILE *f,
                     const std::vector<PortEntry> &ports,
                     const PortIndexMap &port_idx)
{
  for (const PortEntry &e : ports) {
    const LibertyPort *p = e.port;
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

    // Per-port ReceiverModel (CCS receiver capacitance for this port,
    // independent of the per-arc receiver model on GateTableModel).
    writeReceiverModel(f, p->receiverModel());

    // DriverWaveform per RiseFall: stored as the library-level entry's
    // name; reader rebinds via lib->findDriverWaveform(name). Empty
    // name means "no DriverWaveform set on this RF slot".
    for (auto rf : RiseFall::range()) {
      DriverWaveform *dw = p->driverWaveform(rf);
      cache::writeString(f, dw ? dw->name() : std::string_view{});
    }
  }
}

// === Per-arc TableModel / TableModels (commit 4b) ====================
//
// TableModel wraps a TablePtr together with a TableTemplate pointer
// (looked up in the library by name on read), a ScaleFactorType, the
// RiseFall index, and an is_scaled flag. The is_scaled flag is a
// runtime PVT-scaling marker that's always false for a freshly-read
// .lib, so it isn't serialized -- the reader leaves it at default.

void
writeTableModel(FILE *f, const TableModel *model)
{
  if (model == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  // Template (looked up by name + type on read). Templates are written
  // in section TableTemplates (commit 2) so are guaranteed to be
  // available before any cell-section deserialization.
  TableTemplate *tt = model->tblTemplate();
  cache::writeString(f, tt ? tt->name() : std::string_view{});
  cache::writeU32(f, tt ? static_cast<uint32_t>(tt->type())
                        : static_cast<uint32_t>(TableTemplateType::delay));
  cache::writeU32(f, static_cast<uint32_t>(model->scaleFactorType()));
  cache::writeU32(f, static_cast<uint32_t>(model->rfIndex()));
  // The wrapped Table itself.
  writeTable(f, model->table().get());
}

// TableModels groups the main TableModel with optional OCV variants.
// Each slot is independently nullable; we write a presence-bit per
// slot rather than a header bitmask so missing-from-cache combinations
// stay forward-compatible.
void
writeTableModels(FILE *f, const TableModels *models)
{
  if (models == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  writeTableModel(f, models->model());
  writeTableModel(f, const_cast<TableModels*>(models)->sigma(EarlyLate::early()));
  writeTableModel(f, const_cast<TableModels*>(models)->sigma(EarlyLate::late()));
  writeTableModel(f, models->stdDev());
  writeTableModel(f, models->meanShift());
  writeTableModel(f, models->skewness());
}

// === ReceiverModel + OutputWaveforms (commit 4c) =====================

void
writeReceiverModel(FILE *f, const ReceiverModel *rm)
{
  if (rm == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  // capacitance_models_ is laid out as [segment * 2 + rf_index]. We
  // serialize the flat vector; the reader recovers segment/rf from
  // the index. Empty (default-constructed) slots are emitted as
  // present=false so the reader doesn't treat them as real entries
  // and re-bind them to a wrong (segment, rf) on the read side.
  const std::vector<TableModel> &models = rm->capacitanceModels();
  cache::writeU32(f, static_cast<uint32_t>(models.size()));
  for (const TableModel &m : models) {
    if (m.table().get() == nullptr) {
      cache::writeBool(f, false);  // empty slot
      continue;
    }
    writeTableModel(f, &m);
  }
}

void
writeOutputWaveforms(FILE *f, const OutputWaveforms *ow)
{
  if (ow == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  cache::writeU32(f, static_cast<uint32_t>(ow->rf()->index()));
  writeTableAxis(f, ow->slewAxis());
  writeTableAxis(f, ow->capAxis());

  // current_waveforms_ is a vector of 1D Tables (Table*). Length is
  // typically slewAxis.size * capAxis.size; we serialize the raw
  // count and each Table independently so reordering or sparse
  // populations would still round-trip.
  const Table1Seq &waveforms = ow->currentWaveforms();
  cache::writeU32(f, static_cast<uint32_t>(waveforms.size()));
  for (const Table *t : waveforms)
    writeTable(f, t);

  // ref_times_ is a (slew × cap) grid of reference times.
  writeTable(f, &ow->referenceTimes());
}

// Write a TimingModel*. The dispatch to GateTableModel / CheckTableModel
// is determined by the TimingArcSet's role (set on the read side).
void
writeArcModel(FILE *f, const TimingModel *model, bool is_check)
{
  if (model == nullptr) {
    cache::writeBool(f, false);
    return;
  }
  cache::writeBool(f, true);
  if (is_check) {
    const CheckTableModel *cm = static_cast<const CheckTableModel*>(model);
    writeTableModels(f, cm->checkModels());
  }
  else {
    const GateTableModel *gm = static_cast<const GateTableModel*>(model);
    writeTableModels(f, gm->delayModels());
    writeTableModels(f, gm->slewModels());
    // CCS-specific fields. Either may be null on cells without CCS
    // characterization.
    writeReceiverModel(f, gm->receiverModel());
    writeOutputWaveforms(f, gm->outputWaveforms());
  }
}

// === TimingArcSet section (commit 4b) ================================

void
writeTimingArcSets(FILE *f, const LibertyCell *cell, const PortIndexMap &port_idx)
{
  const TimingArcSetSeq &sets = cell->timingArcSets();
  cache::writeU32(f, static_cast<uint32_t>(sets.size()));
  auto write_port_ref = [&](const LibertyPort *p) {
    if (p == nullptr) {
      cache::writeU32(f, 0xFFFFFFFFu);
      return;
    }
    auto it = port_idx.find(p);
    cache::writeU32(f, it == port_idx.end() ? 0xFFFFFFFFu : it->second);
  };
  for (const TimingArcSet *set : sets) {
    write_port_ref(set->from());
    write_port_ref(set->to());
    write_port_ref(set->relatedOut());
    cache::writeString(f, set->role()->to_string());
    cache::writeBool(f, set->isCondDefault());

    // Attrs.
    cache::writeU32(f, static_cast<uint32_t>(set->timingType()));
    cache::writeU32(f, static_cast<uint32_t>(set->sense()));
    writeFuncExpr(f, set->cond(), port_idx);
    cache::writeString(f, set->sdfCondStart());  // also serves as sdfCond
    cache::writeString(f, set->sdfCondEnd());
    cache::writeString(f, set->modeName());
    cache::writeString(f, set->modeValue());
    cache::writeFloat(f, set->ocvArcDepth());

    // Per-RF model in attrs (rise/fall slots). Concrete type is
    // determined by the role.
    bool is_check = set->role()->isTimingCheck();
    for (auto rf : RiseFall::range())
      writeArcModel(f, set->model(rf), is_check);

    // Per-arc rf slot (0=rise, 1=fall, 0xFF=none) of the attrs model
    // that the arc's TimingModel aliases. Stored explicitly because
    // LibertyBuilder keys arcs by the *output* edge for negative-unate
    // combinational and by trZ1/trZ0 for tristate -- not inferable
    // from from_t alone.
    const TimingArcSeq &arcs = set->arcs();
    cache::writeU32(f, static_cast<uint32_t>(arcs.size()));
    const TimingModel *m_rise = set->model(RiseFall::rise());
    const TimingModel *m_fall = set->model(RiseFall::fall());
    for (const TimingArc *arc : arcs) {
      cache::writeString(f, arc->fromEdge()->to_string());
      cache::writeString(f, arc->toEdge()->to_string());
      const TimingModel *am = arc->model();
      uint32_t slot = 0xFFu;
      if      (am != nullptr && am == m_rise) slot = 0;
      else if (am != nullptr && am == m_fall) slot = 1;
      cache::writeU32(f, slot);
    }
  }
}

void
writeOcvDerates(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::OcvDerates);
  const OcvDerateMap &derate_map = lib->ocvDerateMap();
  cache::writeU32(f, static_cast<uint32_t>(derate_map.size()));
  for (const auto &[name, derate] : derate_map) {
    cache::writeString(f, name);
    // OcvDerate's derate_ array is keyed by [rise/fall][early/late]
    // [path_type=clk|data]. Iterate in the same order on read.
    OcvDerate &mut_derate = const_cast<OcvDerate&>(derate);
    for (auto rf : RiseFall::range()) {
      for (auto el : EarlyLate::range()) {
        for (size_t pt = 0; pt < path_type_count; ++pt) {
          const Table *t = mut_derate.derateTable(rf, el,
                                                  static_cast<PathType>(pt));
          writeTable(f, t);
        }
      }
    }
  }
  // Default by name (empty if none).
  const OcvDerate *def = lib->defaultOcvDerate();
  cache::writeString(f, def ? def->name() : std::string_view{});
}

void
writeDriverWaveforms(FILE *f, const LibertyLibrary *lib)
{
  cache::writeSectionId(f, SectionId::DriverWaveforms);
  const DriverWaveformMap &dw_map = lib->driverWaveformMap();
  cache::writeU32(f, static_cast<uint32_t>(dw_map.size()));
  for (const auto &[name, dw] : dw_map) {
    cache::writeString(f, name);
    writeTable(f, dw.waveformsTable());
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
    cache::writeString(f, cell_sf ? cell_sf->name() : std::string_view{});

    // Two-pass: port headers first so FuncExpr port refs in pass 2
    // can resolve to within-cell indices.
    auto [port_idx, port_entries] = writeCellPortHeaders(f, cell);
    writeCellPortDetails(f, port_entries, port_idx);

    // === Per-cell BusDcl ============================================
    const BusDclMap &cell_bus_dcls = cell->busDclMap();
    cache::writeU32(f, static_cast<uint32_t>(cell_bus_dcls.size()));
    for (const auto &[name, bd] : cell_bus_dcls) {
      cache::writeString(f, name);
      cache::writeI64(f, bd.from());
      cache::writeI64(f, bd.to());
    }

    // === Per-cell ModeDef ============================================
    // Each ModeDef holds a value map; each ModeValueDef has a
    // (string sdf_cond, FuncExpr cond). cond may reference cell ports.
    const ModeDefMap &cell_modes = cell->modeDefMap();
    cache::writeU32(f, static_cast<uint32_t>(cell_modes.size()));
    for (const auto &[mode_name, mode] : cell_modes) {
      cache::writeString(f, mode_name);
      const ModeValueMap &values = mode.values();
      cache::writeU32(f, static_cast<uint32_t>(values.size()));
      for (const auto &[value_name, value_def] : values) {
        cache::writeString(f, value_name);
        cache::writeString(f, value_def.sdfCond());
        writeFuncExpr(f, value_def.cond(), port_idx);
      }
    }

    // === Sequentials =================================================
    // Each Sequential has up to 4 FuncExprs (clock/data/clear/preset),
    // 2 LogicValue enums, and 2 LibertyPort refs (output, output_inv).
    const SequentialSeq &seqs = cell->sequentials();
    cache::writeU32(f, static_cast<uint32_t>(seqs.size()));
    auto write_port_ref = [&](const LibertyPort *p) {
      if (p == nullptr) {
        cache::writeU32(f, 0xFFFFFFFFu);
        return;
      }
      auto it = port_idx.find(p);
      cache::writeU32(f, it == port_idx.end() ? 0xFFFFFFFFu : it->second);
    };
    for (const Sequential &s : seqs) {
      cache::writeBool(f, s.isRegister());
      writeFuncExpr(f, s.clock(),  port_idx);
      writeFuncExpr(f, s.data(),   port_idx);
      writeFuncExpr(f, s.clear(),  port_idx);
      writeFuncExpr(f, s.preset(), port_idx);
      cache::writeU32(f, static_cast<uint32_t>(s.clearPresetOutput()));
      cache::writeU32(f, static_cast<uint32_t>(s.clearPresetOutputInv()));
      write_port_ref(s.output());
      write_port_ref(s.outputInv());
    }

    // === Statetable ==================================================
    const Statetable *st = cell->statetable();
    cache::writeBool(f, st != nullptr);
    if (st) {
      const LibertyPortSeq &in_ports = st->inputPorts();
      cache::writeU32(f, static_cast<uint32_t>(in_ports.size()));
      for (const LibertyPort *p : in_ports)
        write_port_ref(p);
      const LibertyPortSeq &int_ports = st->internalPorts();
      cache::writeU32(f, static_cast<uint32_t>(int_ports.size()));
      for (const LibertyPort *p : int_ports)
        write_port_ref(p);
      const StatetableRows &rows = st->table();
      cache::writeU32(f, static_cast<uint32_t>(rows.size()));
      for (const StatetableRow &row : rows) {
        const auto &iv = row.inputValues();
        cache::writeU32(f, static_cast<uint32_t>(iv.size()));
        for (StateInputValue v : iv)
          cache::writeU32(f, static_cast<uint32_t>(v));
        const auto &cv = row.currentValues();
        cache::writeU32(f, static_cast<uint32_t>(cv.size()));
        for (StateInternalValue v : cv)
          cache::writeU32(f, static_cast<uint32_t>(v));
        const auto &nv = row.nextValues();
        cache::writeU32(f, static_cast<uint32_t>(nv.size()));
        for (StateInternalValue v : nv)
          cache::writeU32(f, static_cast<uint32_t>(v));
      }
    }

    // === Timing arc sets (commit 4b) ================================
    writeTimingArcSets(f, cell, port_idx);

    // === Internal power (commit 5) ==================================
    auto write_port_ref_5 = [&](const LibertyPort *p) {
      if (p == nullptr) {
        cache::writeU32(f, 0xFFFFFFFFu);
        return;
      }
      auto it = port_idx.find(p);
      cache::writeU32(f, it == port_idx.end() ? 0xFFFFFFFFu : it->second);
    };

    const InternalPowerSeq &ips = cell->internalPowers();
    cache::writeU32(f, static_cast<uint32_t>(ips.size()));
    for (const InternalPower &ip : ips) {
      write_port_ref_5(ip.port());
      write_port_ref_5(ip.relatedPort());
      write_port_ref_5(ip.relatedPgPin());
      writeFuncExpr(f, ip.when(), port_idx);
      // Per-RF InternalPowerModel: just the wrapped TableModel.
      for (auto rf : RiseFall::range())
        writeTableModel(f, ip.model(rf).model());
    }

    // === Leakage power (commit 5) ===================================
    const LeakagePowerSeq &lps = cell->leakagePowers();
    cache::writeU32(f, static_cast<uint32_t>(lps.size()));
    for (const LeakagePower &lp : lps) {
      write_port_ref_5(lp.relatedPgPort());
      writeFuncExpr(f, lp.when(), port_idx);
      cache::writeFloat(f, lp.power());
    }
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
  writeUnits(f, lib);
  writeBusDcls(f, lib);
  writeOperatingConditions(f, lib);
  writeScaleFactors(f, lib);
  writeSupplyVoltages(f, lib);
  writeTableTemplates(f, lib);
  writeOcvDerates(f, lib);
  writeDriverWaveforms(f, lib);
  writeCells(f, lib);
  cache::writeSectionId(f, SectionId::EndMarker);
}

} // namespace sta
