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

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
bool
parseOptimisticFloatSeq(const char *str, std::vector<float> &floats)
{
  floats.clear();
  const char *p = str;
  char *end;
  bool has_float = false;
  while (*p) {
    // Skip whitespace and delimiters
    while (*p && (isspace(*p) || *p == ',' || *p == '\"' || *p == '{' || *p == '}')) p++;
    if (!*p) break;
    
    float f = strtof(p, &end);
    if (p == end) return false; // Failed to parse float
    floats.push_back(f);
    has_float = true;
    p = end;
  }
  return has_float;
}
};

namespace sta {

void
writeLibertyBinary(std::istream *in_stream,
                   std::ostream *out_stream,
                   Report *report)
{
  LibertyBinaryWriter writer(out_stream);
  // Write Magic and Version
  out_stream->write(LIBERTY_BINARY_MAGIC, 8);
  uint32_t version = 1;
  out_stream->write(reinterpret_cast<const char*>(&version), sizeof(version));
  uint64_t string_table_offset = 0;
  uint64_t string_table_offset_location = out_stream->tellp();
  out_stream->write(reinterpret_cast<const char*>(&string_table_offset), sizeof(string_table_offset));

  parseLibertyFile(in_stream, "stream", &writer, report);
  
  // Write EOF tag
  uint8_t eof_tag = static_cast<uint8_t>(LibertyBinaryTag::EOF_TAG);
  out_stream->write(reinterpret_cast<const char*>(&eof_tag), sizeof(eof_tag));
  string_table_offset = out_stream->tellp();
  out_stream->seekp(string_table_offset_location, out_stream->beg);
  out_stream->write(reinterpret_cast<const char*>(&string_table_offset), sizeof(string_table_offset));

  // go back to end of file
  out_stream->seekp(0, out_stream->end);
  // Write string table
  uint32_t string_table_size = writer.string_table().size();
  out_stream->write(reinterpret_cast<const char*>(&string_table_size), sizeof(string_table_size));
  for (const auto &entry : writer.string_table()) {
    uint32_t string_length = entry.first.size();
    out_stream->write(reinterpret_cast<const char*>(&string_length), sizeof(string_length));
    out_stream->write(entry.first.c_str(), entry.first.size());
    out_stream->write(reinterpret_cast<const char*>(&entry.second), sizeof(entry.second));
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
LibertyBinaryWriter::begin(LibertyGroup *group)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::GROUP_BEGIN));
  writeString(group->type());
  
  LibertyAttrValueSeq *params = group->params();
  uint32_t param_count = params ? params->size() : 0;
  stream_->write(reinterpret_cast<const char*>(&param_count), sizeof(param_count));
  
  if (params) {
    for (LibertyAttrValue *val : *params) {
      writeValue(val);
    }
  }
}

void
LibertyBinaryWriter::end(LibertyGroup *group)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::GROUP_END));
}

void
LibertyBinaryWriter::visitAttr(LibertyAttr *attr)
{
  if (attr->isSimple()) {
    writeTag(static_cast<uint8_t>(LibertyBinaryTag::ATTR_SIMPLE));
    writeString(attr->name());
    
    // Simple attr has 1 value
    uint32_t count = 1;
    stream_->write(reinterpret_cast<const char*>(&count), sizeof(count));
    writeValue(attr->firstValue());
  }
  else if (attr->isComplex()) {
    writeTag(static_cast<uint8_t>(LibertyBinaryTag::ATTR_COMPLEX));
    writeString(attr->name());
    
    LibertyAttrValueSeq *values = attr->values();
    uint32_t count = values ? values->size() : 0;
    stream_->write(reinterpret_cast<const char*>(&count), sizeof(count));
    if (!values) {
      return;
    }
    for (LibertyAttrValue *val : *values) {
      if (val->isString()) {
        std::vector<float> float_values;
        if (parseOptimisticFloatSeq(val->stringValue(), float_values)) {
          if (float_values.size() == 1) {
            LibertyFloatAttrValue float_val(float_values[0]);
            writeValue(&float_val);
            continue;
          }
          FloatSeq* float_seq = new FloatSeq();
          float_seq->reserve(float_values.size());
          for (float f : float_values) {
            float_seq->push_back(f);
          }
          // ownership of float_seq is transferred to float_seq_val
          LibertyFloatSeqAttrValue float_seq_val(float_seq);
          writeValue(&float_seq_val);
        } else {
          writeValue(val);
        }
      }
      else {
        writeValue(val);
      }
    }
  }
}

void
LibertyBinaryWriter::visitVariable(LibertyVariable *variable)
{
  writeTag(static_cast<uint8_t>(LibertyBinaryTag::VARIABLE));
  writeString(variable->variable());
  writeFloat(variable->value());
}

bool
LibertyBinaryWriter::save(LibertyGroup *group)
{
  return false;
}

bool
LibertyBinaryWriter::save(LibertyAttr *attr)
{
  return false;
}

bool
LibertyBinaryWriter::save(LibertyVariable *variable)
{
  return false;
}

void
LibertyBinaryWriter::writeTag(uint8_t tag)
{
  stream_->write(reinterpret_cast<const char*>(&tag), sizeof(tag));
}

void
LibertyBinaryWriter::writeString(const char *str)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::STRING);
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));
  
  if (string_table_.find(str) != string_table_.end()) {
    uint32_t offset = string_table_[str];
    stream_->write(reinterpret_cast<const char*>(&offset), sizeof(offset));
    return;
  } else {
    uint32_t offset = string_table_.size();
    string_table_[str] = offset;
    stream_->write(reinterpret_cast<const char*>(&offset), sizeof(offset));
  }
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
LibertyBinaryWriter::writeValue(LibertyAttrValue *value)
{
  if (value->isString()) {
    writeString(value->stringValue());
  }
  else if (value->isFloat()) {
    writeFloat(value->floatValue());
  }
  else if (value->isFloatSeq()) {
    FloatSeq *floats = value->floatValues();
    writeFloatSeq(*floats);
  }
}

void
LibertyBinaryWriter::writeFloatSeq(const std::vector<float> &floats)
{
  uint8_t type = static_cast<uint8_t>(LibertyBinaryValueType::FLOAT_SEQ);
  stream_->write(reinterpret_cast<const char*>(&type), sizeof(type));
  uint32_t count = floats.size();
  stream_->write(reinterpret_cast<const char*>(&count), sizeof(count));
  if (count > 0) {
    stream_->write(reinterpret_cast<const char*>(floats.data()), count * sizeof(float));
  }
}

} // namespace
