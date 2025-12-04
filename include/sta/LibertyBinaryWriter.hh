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

namespace sta {

class StaState;

class LibertyBinaryWriter : public LibertyGroupVisitor
{
public:
  LibertyBinaryWriter(std::ostream *stream);
  virtual ~LibertyBinaryWriter();

  virtual void begin(LibertyGroup *group);
  virtual void end(LibertyGroup *group);
  virtual void visitAttr(LibertyAttr *attr);
  virtual void visitVariable(LibertyVariable *variable);
  virtual bool save(LibertyGroup *group);
  virtual bool save(LibertyAttr *attr);
  virtual bool save(LibertyVariable *variable);

private:
  void writeTag(std::uint8_t tag);
  void writeString(const char *str);
  void writeFloat(float val);
  void writeInt(int val);
  void writeBool(bool val);
  void writeValue(LibertyAttrValue *value);

  std::ostream *stream_;
};

void
writeLibertyBinary(std::istream *in_stream,
                   std::ostream *out_stream,
                   Report *report);

} // namespace
