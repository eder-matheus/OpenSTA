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

#include <iostream>
#include <vector>
#include <cstdint>
#include "liberty/LibertyParser.hh"

namespace sta {

class LibertyGroupVisitor;
class Report;

class LibertyBinaryReader
{
public:
  LibertyBinaryReader(LibertyGroupVisitor *visitor, Report *report);
  virtual ~LibertyBinaryReader();

  bool read(std::istream *stream);

private:
  void readGroup(LibertyGroup *parent);
  void readSimpleAttr(LibertyGroup *parent);
  void readComplexAttr(LibertyGroup *parent);
  void readVariable(LibertyGroup *parent);
  
  // Helpers
  std::string readString();
  float readFloat();
  int readInt();
  std::uint32_t readUInt32();
  bool readBool();
  LibertyAttrValue *readValue();
  
  LibertyGroupVisitor *visitor_;
  Report *report_;
  std::istream *stream_;
  std::vector<LibertyStmt*> stmts_; // To manage memory of created stmts if not saved
};

} // namespace
