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
#include <cstdio>
#include "liberty/LibertyParser.hh"

#include <iostream>
#include <unordered_map>

namespace sta {

class StaState;

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
  std::unordered_map<std::string, std::uint32_t>& string_table() { return string_table_; }

private:
  void writeTag(std::uint8_t tag);
  void writeString(std::string_view str);
  void writeFloat(float val);
  void writeInt(int val);
  void writeBool(bool val);
  void writeValue(const LibertyAttrValue *value);
  void writeFloatSeq(const std::vector<float> &floats);

  std::ostream *stream_;
  std::unordered_map<std::string, std::uint32_t> string_table_;
  // Open group nesting depth; used to free top-level group subtrees once
  // serialized so large libraries don't accumulate in memory.
  int depth_ = 0;
};

void
writeLibertyBinary(std::istream *in_stream,
                   std::ostream *out_stream,
                   Report *report);

} // namespace
