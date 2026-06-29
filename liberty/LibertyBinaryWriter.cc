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
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
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
} // namespace

namespace sta {

void
writeLibertyBinary(std::istream *in_stream,
                   std::ostream *out_stream,
                   Report *report)
{
  LibertyBinaryWriter writer(out_stream);
  // Write Magic and Version.
  out_stream->write(LIBERTY_BINARY_MAGIC, 8);
  uint32_t version = 1;
  out_stream->write(reinterpret_cast<const char*>(&version), sizeof(version));
  uint64_t string_table_offset = 0;
  uint64_t string_table_offset_location = out_stream->tellp();
  out_stream->write(reinterpret_cast<const char*>(&string_table_offset),
                    sizeof(string_table_offset));

  parseLibertyFile(in_stream, "stream", &writer, report);

  // Write EOF tag.
  uint8_t eof_tag = static_cast<uint8_t>(LibertyBinaryTag::EOF_TAG);
  out_stream->write(reinterpret_cast<const char*>(&eof_tag), sizeof(eof_tag));

  // Backpatch the string table offset.
  string_table_offset = out_stream->tellp();
  out_stream->seekp(string_table_offset_location, out_stream->beg);
  out_stream->write(reinterpret_cast<const char*>(&string_table_offset),
                    sizeof(string_table_offset));

  // Write the string table at the end of the file.
  out_stream->seekp(0, out_stream->end);
  uint32_t string_table_size = writer.string_table().size();
  out_stream->write(reinterpret_cast<const char*>(&string_table_size),
                    sizeof(string_table_size));
  for (const auto &entry : writer.string_table()) {
    uint32_t string_length = entry.first.size();
    out_stream->write(reinterpret_cast<const char*>(&string_length),
                      sizeof(string_length));
    out_stream->write(entry.first.c_str(), entry.first.size());
    out_stream->write(reinterpret_cast<const char*>(&entry.second),
                      sizeof(entry.second));
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
  stream_->write(reinterpret_cast<const char*>(&param_count), sizeof(param_count));
  for (const LibertyAttrValue *val : params)
    writeValue(val);
}

void
LibertyBinaryWriter::end(const LibertyGroup *,
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
}

void
LibertyBinaryWriter::visitAttr(const LibertySimpleAttr *attr)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::ATTR_SIMPLE));
  writeString(attr->name());
  uint32_t count = 1;
  stream_->write(reinterpret_cast<const char*>(&count), sizeof(count));
  writeValue(&attr->value());
}

void
LibertyBinaryWriter::visitAttr(const LibertyComplexAttr *attr)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::ATTR_COMPLEX));
  writeString(attr->name());

  const LibertyAttrValueSeq &values = attr->values();
  uint32_t count = values.size();
  stream_->write(reinterpret_cast<const char*>(&count), sizeof(count));
  for (const LibertyAttrValue *val : values) {
    if (val->isString()) {
      std::vector<float> float_values;
      if (parseOptimisticFloatSeq(val->stringValue(), float_values)) {
        if (float_values.size() == 1) {
          LibertyAttrValue float_val(float_values[0]);
          writeValue(&float_val);
          continue;
        }
        LibertyAttrValue float_seq_val(std::move(float_values));
        writeValue(&float_seq_val);
      }
      else
        writeValue(val);
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
  stream_->write(reinterpret_cast<const char*>(&tag), sizeof(tag));
}

void
LibertyBinaryWriter::writeString(std::string_view str)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::STRING);
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));

  std::string key(str);
  auto it = string_table_.find(key);
  uint32_t offset;
  if (it != string_table_.end())
    offset = it->second;
  else {
    offset = string_table_.size();
    string_table_[key] = offset;
  }
  stream_->write(reinterpret_cast<const char*>(&offset), sizeof(offset));
}

void
LibertyBinaryWriter::writeFloat(float val)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::FLOAT);
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));
  stream_->write(reinterpret_cast<const char*>(&val), sizeof(val));
}

void
LibertyBinaryWriter::writeInt(int val)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::INT);
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));
  stream_->write(reinterpret_cast<const char*>(&val), sizeof(val));
}

void
LibertyBinaryWriter::writeBool(bool val)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::BOOLEAN);
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));
  uint8_t v = val ? 1 : 0;
  stream_->write(reinterpret_cast<const char*>(&v), sizeof(v));
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
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));
  uint32_t count = floats.size();
  stream_->write(reinterpret_cast<const char*>(&count), sizeof(count));
  if (count > 0)
    stream_->write(reinterpret_cast<const char*>(floats.data()),
                   count * sizeof(float));
}

} // namespace sta
