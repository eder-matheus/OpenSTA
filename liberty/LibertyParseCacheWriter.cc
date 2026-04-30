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
#include <unordered_map>

#include "Error.hh"
#include "LibertyParseCacheFormat.hh"
#include "LibertyParser.hh"
#include "LibertyReaderPvt.hh"
#include "Network.hh"
#include "StaConfig.hh"

namespace sta {

namespace {

struct SourceMetadata
{
  uint64_t size = 0;
  int64_t mtime = 0;
};

SourceMetadata
sourceMetadata(std::string_view filename)
{
  SourceMetadata md;
  if (filename.empty())
    return md;
  std::error_code ec;
  std::filesystem::path path{std::string(filename)};
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
writeHeader(FILE *f, std::string_view source_lib_path)
{
  lpc::writeU32(f, lpc::kMagic);
  lpc::writeU32(f, lpc::kFormatVersion);
  lpc::writeU32(f, lpc::kEndianSentinel);
  lpc::writeU32(f, lpc::kFlagsNone);
  lpc::writeString(f, STA_VERSION);
  lpc::writeString(f, source_lib_path);
  SourceMetadata md = sourceMetadata(source_lib_path);
  lpc::writeU64(f, md.size);
  lpc::writeI64(f, md.mtime);
}

// Visitor wrapper that intercepts LibertyParser events, writes them
// to the output file, and forwards to the inner visitor (a real
// LibertyReader). This way the parse runs as normal AND a recording
// is produced as a side effect.
//
// String interning: each event handler runs in two phases. First it
// `intern`s every string the event will reference; intern() emits a
// kStringPoolNew record to the file if the string is new. Then the
// handler writes the event tag + payload using `lookup()` (no I/O,
// just hashmap reads). This way no kStringPoolNew record ever appears
// inside another event's payload bytes.
class LpcRecordingVisitor : public LibertyGroupVisitor
{
public:
  LpcRecordingVisitor(LibertyGroupVisitor *inner, FILE *out_file)
    : inner_(inner), out_(out_file) {}

  void begin(const LibertyGroup *group, LibertyGroup *parent_group) override
  {
    intern(group->type());
    for (const LibertyAttrValue *v : group->params())
      internIfString(v);

    lpc::writeTag(out_, lpc::EventTag::kBeginGroup);
    lpc::writeU32(out_, lookup(group->type()));
    const LibertyAttrValueSeq &params = group->params();
    lpc::writeU32(out_, static_cast<uint32_t>(params.size()));
    for (const LibertyAttrValue *v : params)
      writeAttrValue(v);
    lpc::writeI32(out_, group->line());
    inner_->begin(group, parent_group);
  }

  void end(const LibertyGroup *group, LibertyGroup *parent_group) override
  {
    lpc::writeTag(out_, lpc::EventTag::kEnd);
    inner_->end(group, parent_group);
  }

  void visitAttr(const LibertySimpleAttr *attr) override
  {
    intern(attr->name());
    internIfString(&attr->value());

    lpc::writeTag(out_, lpc::EventTag::kSimpleAttr);
    lpc::writeU32(out_, lookup(attr->name()));
    writeAttrValue(&attr->value());
    lpc::writeI32(out_, attr->line());
    inner_->visitAttr(attr);
  }

  void visitAttr(const LibertyComplexAttr *attr) override
  {
    intern(attr->name());
    for (const LibertyAttrValue *v : attr->values())
      internIfString(v);

    lpc::writeTag(out_, lpc::EventTag::kComplexAttr);
    lpc::writeU32(out_, lookup(attr->name()));
    const LibertyAttrValueSeq &vals = attr->values();
    lpc::writeU32(out_, static_cast<uint32_t>(vals.size()));
    for (const LibertyAttrValue *v : vals)
      writeAttrValue(v);
    lpc::writeI32(out_, attr->line());
    inner_->visitAttr(attr);
  }

  void visitVariable(LibertyVariable *variable) override
  {
    intern(variable->variable());

    lpc::writeTag(out_, lpc::EventTag::kVariable);
    lpc::writeU32(out_, lookup(variable->variable()));
    lpc::writeFloat(out_, variable->value());
    lpc::writeI32(out_, variable->line());
    inner_->visitVariable(variable);
  }

private:
  // Intern: if string is new, allocate a new id and emit a
  // kStringPoolNew record to the file. Idempotent.
  void intern(const std::string &s)
  {
    auto it = pool_.find(s);
    if (it != pool_.end())
      return;
    uint32_t id = next_id_++;
    pool_.emplace(s, id);
    lpc::writeTag(out_, lpc::EventTag::kStringPoolNew);
    lpc::writeString(out_, s);
  }

  void internIfString(const LibertyAttrValue *v)
  {
    if (v->isString())
      intern(v->stringValue());
  }

  // Pure lookup. Caller must have called intern() for this string in
  // the same event handler before writing the event payload.
  uint32_t lookup(const std::string &s) const
  {
    return pool_.at(s);
  }

  void writeAttrValue(const LibertyAttrValue *v)
  {
    if (v->isString()) {
      lpc::writeAttrValueTag(out_, lpc::AttrValueTag::kString);
      lpc::writeU32(out_, lookup(v->stringValue()));
    }
    else {
      lpc::writeAttrValueTag(out_, lpc::AttrValueTag::kFloat);
      auto [val, _ok] = v->floatValue();
      lpc::writeFloat(out_, val);
    }
  }

  LibertyGroupVisitor *inner_;
  FILE *out_;
  std::unordered_map<std::string, uint32_t> pool_;
  uint32_t next_id_ = 0;
};

} // namespace

LibertyLibrary *
writeLibertyParseCache(std::string_view source_lib_path,
                       std::string_view lpc_path,
                       bool infer_latches,
                       Network *network)
{
  // 1 MiB stdio buffer cuts syscall count for the multi-MB event
  // stream by ~250×. Allocated as a vector that outlives the FILE
  // (declared first; destructs last in reverse order).
  static constexpr size_t kIoBufSize = 1 << 20;
  std::vector<char> iobuf(kIoBufSize);

  std::unique_ptr<FILE, int(*)(FILE*)> out(
    fopen(std::string(lpc_path).c_str(), "wb"), &fclose);
  if (!out)
    throw FileNotWritable(std::string(lpc_path).c_str());
  FILE *f = out.get();
  setvbuf(f, iobuf.data(), _IOFBF, kIoBufSize);

  writeHeader(f, source_lib_path);

  LibertyReader reader(source_lib_path, infer_latches, network);
  LpcRecordingVisitor recorder(&reader, f);
  parseLibertyFile(source_lib_path, &recorder, network->report());
  lpc::writeTag(f, lpc::EventTag::kEof);
  return reader.library();
}

} // namespace sta
