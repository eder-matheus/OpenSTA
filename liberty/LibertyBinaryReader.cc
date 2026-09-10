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
#include <istream>
#include <memory>
#include <vector>

namespace sta {

// Header is magic(8) + version(4) + string table offset(8).
static constexpr size_t header_size = 20;
// A string table entry is at least length(4) + index(4).
static constexpr size_t min_string_entry_size = 8;
// The smallest encoded value is a type byte + 4 bytes of payload.
static constexpr size_t min_value_size = 5;

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

void
LibertyBinaryReader::corruptError()
{
  report_->error(1900, "{} is not a valid binary liberty file.",
                 parser_.filename());
}

void
LibertyBinaryReader::require(size_t bytes)
{
  if (cursor_.remaining() < bytes)
    corruptError();
}

void
LibertyBinaryReader::read(std::istream *stream)
{
  stream->seekg(0, std::ios::end);
  std::streamoff stream_size = stream->tellg();
  // A negative size covers tellg() failure on an unseekable stream.
  if (stream_size < static_cast<std::streamoff>(header_size))
    corruptError();
  stream->seekg(0, std::ios::beg);

  size_t size = stream_size;
  // Uninitialized; read() fills it and the short-read check below rejects
  // anything less, so no zero fill pass over the buffer is needed.
  std::unique_ptr<char[]> buffer(new char[size]);
  stream->read(buffer.get(), size);
  if (static_cast<size_t>(stream->gcount()) != size)
    corruptError();

  cursor_ = BinaryCursor(buffer.get(), size);

  char magic[9];
  cursor_.readBytes(magic, 8);
  magic[8] = '\0';
  if (strcmp(magic, LIBERTY_BINARY_MAGIC) != 0)
    corruptError();

  cursor_.readU32(); // version
  std::uint64_t string_table_offset = cursor_.readU64();

  // Reject offsets outside the file or inside the header (e.g. the header-only
  // stub a failed write leaves behind) rather than reading out of bounds.
  if (string_table_offset < header_size
      || !cursor_.inBounds(string_table_offset))
    corruptError();

  const char *body_start = cursor_.current();
  cursor_.seek(string_table_offset);
  readStringTable();
  cursor_.setPtr(body_start);

  readStatements(/*top_level=*/true);
}

void
LibertyBinaryReader::readStatements(bool top_level)
{
  LibertyBinaryTag terminator = top_level ? LibertyBinaryTag::EOF_TAG
                                          : LibertyBinaryTag::GROUP_END;
  while (true) {
    require(1);
    LibertyBinaryTag tag = static_cast<LibertyBinaryTag>(cursor_.readU8());
    if (tag == terminator)
      return;
    if (tag == LibertyBinaryTag::GROUP_BEGIN)
      readGroup();
    // Attributes and variables belong to a group; at the top level (or for
    // any unknown tag) the file is corrupt.
    else if (top_level)
      corruptError();
    else if (tag == LibertyBinaryTag::ATTR_SIMPLE)
      readSimpleAttr();
    else if (tag == LibertyBinaryTag::ATTR_COMPLEX)
      readComplexAttr();
    else if (tag == LibertyBinaryTag::VARIABLE)
      readVariable();
    else
      corruptError();
  }
}

void
LibertyBinaryReader::readGroup()
{
  std::string type = readString();
  std::uint32_t param_count = readUInt32();
  if (param_count > cursor_.remaining() / min_value_size)
    corruptError();

  LibertyAttrValueSeq *params = new LibertyAttrValueSeq;
  params->reserve(param_count);
  for (std::uint32_t i = 0; i < param_count; i++)
    params->push_back(readValue());

  // groupBegin takes ownership of params. Each group gets a distinct line so
  // LibertyReader's line-keyed maps treat sibling groups as distinct.
  int line = next_line_++;
  parser_.groupBegin(std::move(type), params, line);

  readStatements(/*top_level=*/false);

  parser_.groupEnd();
}

void
LibertyBinaryReader::readSimpleAttr()
{
  std::string name = readString();
  readUInt32(); // Consume count (always 1 in the format).
  LibertyAttrValue *val = readValue();
  // makeSimpleAttr takes ownership of the value and dispatches to the visitor.
  parser_.makeSimpleAttr(std::move(name), val, next_line_++);
}

void
LibertyBinaryReader::readComplexAttr()
{
  std::string name = readString();
  std::uint32_t count = readUInt32();
  if (count > cursor_.remaining() / min_value_size)
    corruptError();

  LibertyAttrValueSeq *values = new LibertyAttrValueSeq;
  values->reserve(count);
  for (std::uint32_t i = 0; i < count; i++)
    values->push_back(readValue());
  // makeComplexAttr takes ownership of the values and dispatches to the visitor.
  parser_.makeComplexAttr(std::move(name), values, next_line_++);
}

void
LibertyBinaryReader::readVariable()
{
  std::string name = readString();
  float val = readFloat();
  parser_.makeVariable(std::move(name), val, next_line_++);
}

std::string
LibertyBinaryReader::readString()
{
  require(min_value_size);
  std::uint8_t type = cursor_.readU8();
  if (static_cast<LibertyBinaryValueType>(type) != LibertyBinaryValueType::STRING)
    corruptError();

  std::uint32_t index = cursor_.readU32();
  if (index >= string_table_.size())
    corruptError();
  return string_table_[index];
}

void
LibertyBinaryReader::readStringTable()
{
  require(4);
  std::uint32_t size = cursor_.readU32();
  // Validate the count before allocating; each entry needs at least 8 bytes.
  if (size > cursor_.remaining() / min_string_entry_size)
    corruptError();
  string_table_.resize(size);
  for (std::uint32_t i = 0; i < size; i++) {
    require(4);
    std::uint32_t len = cursor_.readU32();
    // len for the string bytes plus the 4-byte index that follows.
    require(static_cast<size_t>(len) + 4);
    std::string str(len, '\0');
    cursor_.readBytes(str.data(), len);
    // The writer stores each string's dense index; place the string there
    // because the writer iterates an unordered_map in arbitrary order.
    std::uint32_t index = cursor_.readU32();
    if (index >= string_table_.size())
      corruptError();
    string_table_[index] = std::move(str);
  }
}

float
LibertyBinaryReader::readFloat()
{
  require(min_value_size);
  std::uint8_t type = cursor_.readU8();
  if (static_cast<LibertyBinaryValueType>(type) != LibertyBinaryValueType::FLOAT)
    corruptError();
  return cursor_.readFloat();
}

std::uint32_t
LibertyBinaryReader::readUInt32()
{
  require(4);
  return cursor_.readU32();
}

LibertyAttrValue *
LibertyBinaryReader::readValue()
{
  require(1);
  std::uint8_t type = cursor_.peekU8();
  LibertyBinaryValueType val_type = static_cast<LibertyBinaryValueType>(type);

  if (val_type == LibertyBinaryValueType::STRING) {
    std::string str = readString();
    return new LibertyAttrValue(std::move(str));
  }
  else if (val_type == LibertyBinaryValueType::FLOAT)
    return new LibertyAttrValue(readFloat());
  else if (val_type == LibertyBinaryValueType::FLOAT_SEQ) {
    cursor_.readU8(); // type byte
    std::uint32_t count = readUInt32();
    require(static_cast<size_t>(count) * sizeof(float));
    std::vector<float> seq(count);
    cursor_.readBytes(reinterpret_cast<char*>(seq.data()),
                      static_cast<size_t>(count) * sizeof(float));
    return new LibertyAttrValue(std::move(seq));
  }
  corruptError();
  return nullptr; // Unreachable; corruptError throws.
}

} // namespace sta
