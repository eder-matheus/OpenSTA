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

#pragma once

#include <cstdint>
#include <iosfwd>
#include <unordered_map>
#include "liberty/LibertyParser.hh"

namespace sta {

// Heterogeneous lookup so the table can be probed with a string_view
// without allocating a key.
struct StringViewHash {
  using is_transparent = void;
  size_t operator()(std::string_view str) const {
    return std::hash<std::string_view>()(str);
  }
};
using LibertyStringTable =
  std::unordered_map<std::string, std::uint32_t, StringViewHash, std::equal_to<>>;

class LibertyBinaryWriter : public LibertyGroupVisitor
{
public:
  LibertyBinaryWriter(std::ostream *stream);
  virtual ~LibertyBinaryWriter();

  virtual void begin(const LibertyGroup *group,
                     LibertyGroup *parent_group) override;
  virtual void end(const LibertyGroup *group,
                   LibertyGroup *parent_group) override;
  virtual void visitAttr(const LibertySimpleAttr *attr) override;
  virtual void visitAttr(const LibertyComplexAttr *attr) override;
  virtual void visitVariable(LibertyVariable *variable) override;
  const LibertyStringTable &string_table() const { return string_table_; }

private:
  void writeTag(std::uint8_t tag);
  void writeString(std::string_view str);
  void writeFloat(float val);
  void writeValue(const LibertyAttrValue *value);
  void writeFloatSeq(const std::vector<float> &floats);

  std::ostream *stream_;
  LibertyStringTable string_table_;
  // Open group nesting depth; used to free top-level group subtrees once
  // serialized so large libraries don't accumulate in memory.
  int depth_ = 0;
};

void
writeLibertyBinary(const char *in_filename,
                   const char *out_filename,
                   Report *report);

} // namespace
