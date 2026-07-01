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
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstring>
#include "liberty/LibertyParser.hh"

namespace sta {

class LibertyGroupVisitor;
class Report;

class BinaryCursor {
    const char* ptr_;
    const char* end_;
    const char* start_;
public:
    BinaryCursor(const char* data, size_t size) : ptr_(data), end_(data + size), start_(data) {}

    // Force inline these to make them disappear in assembly
    inline std::uint8_t readU8() { return *reinterpret_cast<const std::uint8_t*>(ptr_++); }
    
    inline std::uint32_t readU32() {
        // Assumes little-endian architecture (standard x86/ARM)
        std::uint32_t val;
        std::memcpy(&val, ptr_, 4); // memcpy is optimized away by compiler to a simple mov
        ptr_ += 4;
        return val;
    }

    inline std::int32_t readI32() {
        std::int32_t val;
        std::memcpy(&val, ptr_, 4);
        ptr_ += 4;
        return val;
    }

    inline std::uint64_t readU64() {
        std::uint64_t val;
        std::memcpy(&val, ptr_, 8);
        ptr_ += 8;
        return val;
    }

    inline float readFloat() {
        float val;
        std::memcpy(&val, ptr_, 4);
        ptr_ += 4;
        return val;
    }

    inline void readBytes(char* dest, size_t len) {
        std::memcpy(dest, ptr_, len);
        ptr_ += len;
    }

    inline void seek(size_t offset) { ptr_ = start_ + offset; }
    inline void setPtr(const char* ptr) { ptr_ = ptr; }
    
    inline const char* current() const { return ptr_; }
    inline bool eof() const { return ptr_ >= end_; }
    
    // For peeking without consuming
    inline std::uint8_t peekU8() const { return *reinterpret_cast<const std::uint8_t*>(ptr_); }
};

class LibertyBinaryReader
{
public:
  LibertyBinaryReader(LibertyGroupVisitor *visitor,
                      std::string_view filename,
                      Report *report);
  virtual ~LibertyBinaryReader();

  bool read(std::istream *stream);

private:
  // Walk the binary stream, driving the parser's builder methods so the
  // group/attribute ownership and visitor dispatch match the text reader.
  void readGroup();
  void readSimpleAttr();
  void readComplexAttr();
  void readVariable();

  // Helpers
  void readStringTable();
  std::string readString();
  float readFloat();
  int readInt();
  std::uint32_t readUInt32();
  bool readBool();
  LibertyAttrValue *readValue();

  LibertyParser parser_;
  Report *report_;
  BinaryCursor cursor_;
  std::vector<std::string> string_table_;
};
} // namespace
