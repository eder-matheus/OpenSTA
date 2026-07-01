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

#include "LibertyBinaryReader.hh"
#include "LibertyBinaryCommon.hh"
#include "LibertyParser.hh"
#include "Report.hh"

#include <cstring>
#include <fstream>
#include <vector>

namespace sta {

LibertyBinaryReader::LibertyBinaryReader(LibertyGroupVisitor *visitor,
                                         std::string_view filename,
                                         Report *report) :
  parser_(filename, visitor, report),
  report_(report),
  cursor_(nullptr, 0)
{
}

LibertyBinaryReader::~LibertyBinaryReader()
{
}

bool
LibertyBinaryReader::read(std::istream *stream)
{
  stream->seekg(0, std::ios::end);
  size_t size = stream->tellg();
  stream->seekg(0, std::ios::beg);

  std::vector<char> buffer(size);
  stream->read(buffer.data(), size);

  cursor_ = BinaryCursor(buffer.data(), size);

  char magic[9];
  cursor_.readBytes(magic, 8);
  magic[8] = '\0';
  if (strcmp(magic, LIBERTY_BINARY_MAGIC) != 0)
    return false;

  cursor_.readU32(); // version
  std::uint64_t string_table_offset = cursor_.readU64();

  const char *body_start = cursor_.current();
  cursor_.seek(string_table_offset);
  readStringTable();
  cursor_.setPtr(body_start);

  while (true) {
    std::uint8_t tag_val = cursor_.readU8();
    LibertyBinaryTag tag = static_cast<LibertyBinaryTag>(tag_val);

    if (tag == LibertyBinaryTag::EOF_TAG)
      return true;
    else if (tag == LibertyBinaryTag::GROUP_BEGIN)
      readGroup();
    else if (tag == LibertyBinaryTag::ATTR_SIMPLE)
      readSimpleAttr();
    else if (tag == LibertyBinaryTag::ATTR_COMPLEX)
      readComplexAttr();
    else if (tag == LibertyBinaryTag::VARIABLE)
      readVariable();
    else
      break;
  }
  return true;
}

void
LibertyBinaryReader::readGroup()
{
  std::string type = readString();
  std::uint32_t param_count = readUInt32();

  LibertyAttrValueSeq *params = new LibertyAttrValueSeq;
  params->reserve(param_count);
  for (std::uint32_t i = 0; i < param_count; i++)
    params->push_back(readValue());

  // groupBegin takes ownership of params.
  parser_.groupBegin(std::move(type), params, 0);

  while (true) {
    std::uint8_t tag_val = cursor_.readU8();
    LibertyBinaryTag tag = static_cast<LibertyBinaryTag>(tag_val);

    if (tag == LibertyBinaryTag::GROUP_END)
      break;
    else if (tag == LibertyBinaryTag::GROUP_BEGIN)
      readGroup();
    else if (tag == LibertyBinaryTag::ATTR_SIMPLE)
      readSimpleAttr();
    else if (tag == LibertyBinaryTag::ATTR_COMPLEX)
      readComplexAttr();
    else if (tag == LibertyBinaryTag::VARIABLE)
      readVariable();
    else if (tag == LibertyBinaryTag::EOF_TAG)
      break; // Should not happen inside a group.
  }

  parser_.groupEnd();
}

void
LibertyBinaryReader::readSimpleAttr()
{
  std::string name = readString();
  readUInt32(); // Consume count (should be 1).
  LibertyAttrValue *val = readValue();
  // makeSimpleAttr copies the value, deletes it, and dispatches to the visitor.
  parser_.makeSimpleAttr(std::move(name), val, 0);
}

void
LibertyBinaryReader::readComplexAttr()
{
  std::string name = readString();
  std::uint32_t count = readUInt32();

  LibertyAttrValueSeq *values = new LibertyAttrValueSeq;
  values->reserve(count);
  for (std::uint32_t i = 0; i < count; i++)
    values->push_back(readValue());
  // makeComplexAttr takes ownership of the values and dispatches to the visitor.
  parser_.makeComplexAttr(std::move(name), values, 0);
}

void
LibertyBinaryReader::readVariable()
{
  std::string name = readString();
  float val = readFloat();
  parser_.makeVariable(std::move(name), val, 0);
}

std::string
LibertyBinaryReader::readString()
{
  std::uint8_t type = cursor_.readU8();
  if (static_cast<LibertyBinaryValueType>(type) != LibertyBinaryValueType::STRING)
    return "";

  std::uint32_t index = cursor_.readU32();
  if (index >= string_table_.size())
    return "";
  return string_table_[index];
}

void
LibertyBinaryReader::readStringTable()
{
  std::uint32_t size = cursor_.readU32();
  string_table_.resize(size);
  for (std::uint32_t i = 0; i < size; i++) {
    std::uint32_t len = cursor_.readU32();
    std::string str(len, '\0');
    cursor_.readBytes(str.data(), len);
    // The writer stores each string's dense index; place the string there
    // because the writer iterates an unordered_map in arbitrary order.
    std::uint32_t index = cursor_.readU32();
    if (index < string_table_.size())
      string_table_[index] = std::move(str);
  }
}

float
LibertyBinaryReader::readFloat()
{
  cursor_.readU8(); // type byte
  return cursor_.readFloat();
}

int
LibertyBinaryReader::readInt()
{
  cursor_.readU8(); // type byte
  return cursor_.readI32();
}

std::uint32_t
LibertyBinaryReader::readUInt32()
{
  return cursor_.readU32();
}

bool
LibertyBinaryReader::readBool()
{
  cursor_.readU8(); // type byte
  return cursor_.readU8() != 0;
}

LibertyAttrValue *
LibertyBinaryReader::readValue()
{
  std::uint8_t type = cursor_.peekU8();
  LibertyBinaryValueType val_type = static_cast<LibertyBinaryValueType>(type);

  if (val_type == LibertyBinaryValueType::STRING) {
    std::string str = readString();
    return new LibertyAttrValue(std::move(str));
  }
  else if (val_type == LibertyBinaryValueType::FLOAT)
    return new LibertyAttrValue(readFloat());
  else if (val_type == LibertyBinaryValueType::INT)
    return new LibertyAttrValue(static_cast<float>(readInt()));
  else if (val_type == LibertyBinaryValueType::BOOLEAN) {
    bool b = readBool();
    return new LibertyAttrValue(std::string(b ? "true" : "false"));
  }
  else if (val_type == LibertyBinaryValueType::FLOAT_SEQ) {
    cursor_.readU8(); // type byte
    std::uint32_t count = readUInt32();
    std::vector<float> seq(count);
    for (std::uint32_t i = 0; i < count; i++)
      seq[i] = cursor_.readFloat();
    return new LibertyAttrValue(std::move(seq));
  }
  return nullptr;
}

} // namespace sta
