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

#include "LibertyParseCache.hh"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "Error.hh"
#include "Format.hh"
#include "LibertyParseCacheFormat.hh"
#include "LibertyParser.hh"
#include "LibertyReaderPvt.hh"
#include "Network.hh"
#include "StaConfig.hh"

namespace sta {

namespace {

void
readHeader(FILE *f, bool ignore_source_check, std::string &source_filename)
{
  uint32_t magic = lpc::readU32(f);
  if (magic != lpc::kMagic)
    lpc::error(sta::format("ldb-parse: bad magic 0x{:x} (expected 0x{:x})",
                           magic, lpc::kMagic));
  uint32_t version = lpc::readU32(f);
  if (version != lpc::kFormatVersion)
    lpc::error(sta::format(
      "ldb-parse: format version {} not supported (this build expects {}); "
      "regenerate with this version of STA.",
      version, lpc::kFormatVersion));
  uint32_t endian = lpc::readU32(f);
  if (endian != lpc::kEndianSentinel)
    lpc::error("ldb-parse: endian mismatch (file was written on a host with "
               "the opposite byte order; not supported)");
  lpc::readU32(f); // flags (reserved)

  std::string sta_version = lpc::readString(f);
  if (sta_version != STA_VERSION)
    lpc::error(sta::format(
      "ldb-parse: STA version mismatch (file='{}', this build='{}'); "
      "regenerate.", sta_version, STA_VERSION));

  source_filename = lpc::readString(f);
  uint64_t source_size = lpc::readU64(f);
  int64_t source_mtime = lpc::readI64(f);

  if (ignore_source_check || source_filename.empty())
    return;

  std::error_code ec;
  std::filesystem::path path{source_filename};
  if (!std::filesystem::exists(path, ec))
    lpc::error(sta::format(
      "ldb-parse: source file '{}' is missing; pass -ignore_source_check "
      "to load anyway.", source_filename));

  uint64_t cur_size = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
  auto ftime = std::filesystem::last_write_time(path, ec);
  int64_t cur_mtime = std::chrono::duration_cast<std::chrono::seconds>(
                        ftime.time_since_epoch()).count();
  if (cur_size != source_size || cur_mtime != source_mtime)
    lpc::error(sta::format(
      "ldb-parse: source file '{}' has changed since the cache was written "
      "(size {}->{}, mtime {}->{}); regenerate or pass -ignore_source_check.",
      source_filename, source_size, cur_size, source_mtime, cur_mtime));
}

// Replays a recorded LibertyGroupVisitor event stream against an
// inner visitor (LibertyReader). Reconstructs LibertyGroup /
// LibertySimpleAttr / LibertyComplexAttr / LibertyAttrValue /
// LibertyVariable nodes on the fly so the visitor sees the same
// objects it would have seen during a normal parse.
class LpcReplayer
{
public:
  LpcReplayer(FILE *in_file, LibertyGroupVisitor *visitor)
    : in_(in_file), visitor_(visitor) {}

  void replay()
  {
    bool done = false;
    while (!done) {
      lpc::EventTag t = lpc::readTag(in_);
      switch (t) {
      case lpc::EventTag::kEof:           done = true; break;
      case lpc::EventTag::kStringPoolNew: handleNewString(); break;
      case lpc::EventTag::kBeginGroup:    handleBegin(); break;
      case lpc::EventTag::kEnd:           handleEnd(); break;
      case lpc::EventTag::kSimpleAttr:    handleSimpleAttr(); break;
      case lpc::EventTag::kComplexAttr:   handleComplexAttr(); break;
      case lpc::EventTag::kVariable:      handleVariable(); break;
      default:
        lpc::error(sta::format("ldb-parse: unknown event tag 0x{:x}",
                               static_cast<unsigned>(t)));
      }
    }
  }

private:
  void handleNewString()
  {
    pool_.push_back(lpc::readString(in_));
  }

  void handleBegin()
  {
    const std::string &type = lookup(lpc::readU32(in_));
    uint32_t param_count = lpc::readU32(in_);
    LibertyAttrValueSeq params;
    params.reserve(param_count);
    for (uint32_t i = 0; i < param_count; ++i)
      params.push_back(readAttrValuePtr());
    int32_t line = lpc::readI32(in_);

    LibertyGroup *parent = group_stack_.empty() ? nullptr : group_stack_.back();
    LibertyGroup *group = new LibertyGroup(type, std::move(params), line);
    visitor_->begin(group, parent);
    group_stack_.push_back(group);
  }

  void handleEnd()
  {
    LibertyGroup *group = group_stack_.back();
    group_stack_.pop_back();
    LibertyGroup *parent = group_stack_.empty() ? nullptr : group_stack_.back();
    if (parent)
      parent->addSubgroup(group);
    visitor_->end(group, parent);
  }

  void handleSimpleAttr()
  {
    const std::string &name = lookup(lpc::readU32(in_));
    LibertyAttrValue *value_ptr = readAttrValuePtr();
    int32_t line = lpc::readI32(in_);

    LibertySimpleAttr *attr =
      new LibertySimpleAttr(std::string(name), *value_ptr, line);
    delete value_ptr;

    LibertyGroup *group = group_stack_.back();
    group->addAttr(attr);
    visitor_->visitAttr(attr);
  }

  void handleComplexAttr()
  {
    const std::string &name = lookup(lpc::readU32(in_));
    uint32_t value_count = lpc::readU32(in_);
    LibertyAttrValueSeq values;
    values.reserve(value_count);
    for (uint32_t i = 0; i < value_count; ++i)
      values.push_back(readAttrValuePtr());
    int32_t line = lpc::readI32(in_);

    LibertyComplexAttr *attr =
      new LibertyComplexAttr(std::string(name), std::move(values), line);

    LibertyGroup *group = group_stack_.back();
    group->addAttr(attr);
    visitor_->visitAttr(attr);
  }

  void handleVariable()
  {
    const std::string &name = lookup(lpc::readU32(in_));
    float value = lpc::readFloat(in_);
    int32_t line = lpc::readI32(in_);

    LibertyVariable *var = new LibertyVariable(std::string(name), value, line);
    LibertyGroup *group = group_stack_.back();
    group->addVariable(var);
    visitor_->visitVariable(var);
  }

  LibertyAttrValue *readAttrValuePtr()
  {
    lpc::AttrValueTag t = lpc::readAttrValueTag(in_);
    if (t == lpc::AttrValueTag::kFloat) {
      float v = lpc::readFloat(in_);
      return new LibertyAttrValue(v);
    }
    // kString
    uint32_t id = lpc::readU32(in_);
    return new LibertyAttrValue(std::string(lookup(id)));
  }

  const std::string &lookup(uint32_t id) const
  {
    if (id >= pool_.size())
      lpc::error(sta::format("ldb-parse: string id {} out of range (pool size {})",
                             id, pool_.size()));
    return pool_[id];
  }

  FILE *in_;
  LibertyGroupVisitor *visitor_;
  std::vector<std::string> pool_;
  std::vector<LibertyGroup*> group_stack_;
};

} // namespace

LibertyLibrary *
readLibertyParseCache(std::string_view lpc_path,
                      bool ignore_source_check,
                      bool infer_latches,
                      Network *network)
{
  static constexpr size_t kIoBufSize = 1 << 20;
  std::vector<char> iobuf(kIoBufSize);

  std::unique_ptr<FILE, int(*)(FILE*)> in(
    fopen(std::string(lpc_path).c_str(), "rb"), &fclose);
  if (!in)
    throw FileNotReadable(std::string(lpc_path).c_str());
  FILE *f = in.get();
  setvbuf(f, iobuf.data(), _IOFBF, kIoBufSize);

  std::string source_filename;
  readHeader(f, ignore_source_check, source_filename);

  LibertyReader reader(source_filename, infer_latches, network);
  LpcReplayer replayer(f, &reader);
  replayer.replay();
  return reader.library();
}

} // namespace sta
