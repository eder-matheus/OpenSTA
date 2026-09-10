// OpenSTA, Static Timing Analyzer
// Copyright (c) 2025, Parallax Software, Inc.
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

#include "LibertyBinaryWriter.hh"
#include "LibertyBinaryCommon.hh"
#include "LibertyParser.hh"
#include "Report.hh"
#include "sta/Error.hh"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace {

// Write a scalar's raw little-endian bytes. Centralizes the cast/sizeof
// pairing so a mismatched address/size cannot creep into one call site.
template <typename T>
void
writeRaw(std::ostream *stream,
         const T &val)
{
  stream->write(reinterpret_cast<const char*>(&val), sizeof(val));
}

// Liberty stores numeric index/value sequences as quoted, comma separated
// strings.  Recognize those so they can be stored as native floats.
bool
parseOptimisticFloatSeq(const std::string &str, std::vector<float> &floats)
{
  floats.clear();
  const char *p = str.c_str();
  char *end;
  bool has_float = false;
  while (*p) {
    while (*p && (isspace(static_cast<unsigned char>(*p)) || *p == ',' || *p == '\"'
                  || *p == '{' || *p == '}'))
      p++;
    if (!*p)
      break;
    float f = strtof(p, &end);
    if (p == end)
      return false; // Not a float list.
    floats.push_back(f);
    has_float = true;
    p = end;
  }
  return has_float;
}

// Only table-style attributes are converted to native floats. Converting any
// numeric-looking string would change the type of values whose consumers
// need a string (e.g. a mode named "1" or a bundle member named "INF").
bool
isFloatSeqAttr(const std::string &name)
{
  return name == "values"
    || name.starts_with("index_");
}

} // namespace

namespace sta {

void
writeLibertyBinary(const char *in_filename,
                   const char *out_filename,
                   Report *report)
{
  std::ofstream out_stream(out_filename, std::ios::binary);
  if (!out_stream)
    throw FileNotWritable(out_filename);

  try {
    LibertyBinaryWriter writer(&out_stream);
    // Write Magic and Version.
    out_stream.write(LIBERTY_BINARY_MAGIC, 8);
    uint32_t version = 1;
    writeRaw(&out_stream, version);
    uint64_t string_table_offset = 0;
    uint64_t string_table_offset_location = out_stream.tellp();
    writeRaw(&out_stream, string_table_offset);

    parseLibertyFile(in_filename, &writer, report);

    // Write EOF tag.
    uint8_t eof_tag = static_cast<uint8_t>(LibertyBinaryTag::EOF_TAG);
    writeRaw(&out_stream, eof_tag);

    // Backpatch the string table offset.
    string_table_offset = out_stream.tellp();
    out_stream.seekp(string_table_offset_location, out_stream.beg);
    writeRaw(&out_stream, string_table_offset);

    // Write the string table at the end of the file.
    out_stream.seekp(0, out_stream.end);
    uint32_t string_table_size = writer.string_table().size();
    writeRaw(&out_stream, string_table_size);
    for (const auto &entry : writer.string_table()) {
      uint32_t string_length = entry.first.size();
      writeRaw(&out_stream, string_length);
      out_stream.write(entry.first.c_str(), entry.first.size());
      writeRaw(&out_stream, entry.second);
    }

    // A failed write (e.g. disk full) silently poisons the stream, so check
    // once at the end rather than reporting success for a corrupt file.
    if (!out_stream.good())
      report->error(1901, "error writing {}.", out_filename);
  }
  catch (...) {
    // Don't leave behind a partial file with a valid magic number.
    out_stream.close();
    std::remove(out_filename);
    throw;
  }
}

LibertyBinaryWriter::LibertyBinaryWriter(std::ostream *stream) :
  stream_(stream)
{
}

LibertyBinaryWriter::~LibertyBinaryWriter()
{
}

void
LibertyBinaryWriter::begin(const LibertyGroup *group,
                           LibertyGroup *)
{
  depth_++;
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::GROUP_BEGIN));
  writeString(group->type());

  const LibertyAttrValueSeq &params = group->params();
  uint32_t param_count = params.size();
  writeRaw(stream_, param_count);
  for (const LibertyAttrValue *val : params)
    writeValue(val);
}

void
LibertyBinaryWriter::end(const LibertyGroup *group,
                         LibertyGroup *parent_group)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::GROUP_END));
  depth_--;
  // LibertyParser retains the whole group tree as it parses. Once a top-level
  // group (a cell, table template, etc. directly under the library) has been
  // fully serialized, its subtree is no longer needed, so release it to bound
  // peak memory. This mirrors LibertyReader::endCell clearing library_group,
  // and lets large libraries stream with ~one-cell memory instead of holding
  // the entire (uncompressed) file in RAM.
  if (depth_ == 1 && parent_group)
    parent_group->clear();
  // The parser pops the library group with no owner and the visitor is the
  // last to see it; the text reader's endLibrary deletes it the same way.
  else if (depth_ == 0)
    delete group;
}

void
LibertyBinaryWriter::visitAttr(const LibertySimpleAttr *attr)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::ATTR_SIMPLE));
  writeString(attr->name());
  uint32_t count = 1;
  writeRaw(stream_, count);
  writeValue(&attr->value());
}

void
LibertyBinaryWriter::visitAttr(const LibertyComplexAttr *attr)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::ATTR_COMPLEX));
  writeString(attr->name());

  const LibertyAttrValueSeq &values = attr->values();
  uint32_t count = values.size();
  writeRaw(stream_, count);
  bool float_seq_attr = isFloatSeqAttr(attr->name());
  std::vector<float> float_values;
  for (const LibertyAttrValue *val : values) {
    if (float_seq_attr
        && val->isString()
        && parseOptimisticFloatSeq(val->stringValue(), float_values)) {
      if (float_values.size() == 1)
        writeFloat(float_values[0]);
      else
        writeFloatSeq(float_values);
    }
    else
      writeValue(val);
  }
}

void
LibertyBinaryWriter::visitVariable(LibertyVariable *variable)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::VARIABLE));
  writeString(variable->variable());
  writeFloat(variable->value());
}

void
LibertyBinaryWriter::writeTag(uint8_t tag)
{
  writeRaw(stream_, tag);
}

void
LibertyBinaryWriter::writeString(std::string_view str)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::STRING);
  writeRaw(stream_, type);

  // Heterogeneous find so table hits (the common case) don't allocate a key.
  auto it = string_table_.find(str);
  uint32_t offset;
  if (it != string_table_.end())
    offset = it->second;
  else {
    offset = string_table_.size();
    string_table_.emplace(std::string(str), offset);
  }
  writeRaw(stream_, offset);
}

void
LibertyBinaryWriter::writeFloat(float val)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::FLOAT);
  writeRaw(stream_, type);
  writeRaw(stream_, val);
}

void
LibertyBinaryWriter::writeValue(const LibertyAttrValue *value)
{
  if (value->isString())
    writeString(value->stringValue());
  else if (value->isFloatSeq()) {
    std::vector<float> floats;
    value->fillFloatSeq(&floats);
    writeFloatSeq(floats);
  }
  else if (value->isFloat()) {
    auto [val, exists] = value->floatValue();
    writeFloat(val);
  }
}

void
LibertyBinaryWriter::writeFloatSeq(const std::vector<float> &floats)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::FLOAT_SEQ);
  writeRaw(stream_, type);
  uint32_t count = floats.size();
  writeRaw(stream_, count);
  if (count > 0)
    stream_->write(reinterpret_cast<const char*>(floats.data()),
                   count * sizeof(float));
}

} // namespace sta
