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

  parseLibertyFile(in_stream, "stream", &writer, report);
  
  // Write EOF tag
  uint8_t eof_tag = static_cast<uint8_t>(LibertyBinaryTag::EOF_TAG);
  out_stream->write(reinterpret_cast<const char*>(&eof_tag), sizeof(eof_tag));
}

LibertyBinaryWriter::LibertyBinaryWriter(std::ostream *stream) : // Changed FILE* to std::ostream*
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
    
    if (values) {
      for (LibertyAttrValue *val : *values) {
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
  
  uint32_t len = str ? strlen(str) : 0;
  stream_->write(reinterpret_cast<const char*>(&len), sizeof(len));
  if (len > 0)
    stream_->write(str, len);
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
}

} // namespace
