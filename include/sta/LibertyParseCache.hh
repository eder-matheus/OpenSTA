// OpenSTA, Static Timing Analyzer
// Copyright (c) 2026, Parallax Software, Inc.
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

#include <stdexcept>
#include <string>
#include <string_view>

#include "NetworkClass.hh"

namespace sta {

class LibertyLibrary;
class Network;

// Thrown when an LPC file fails magic / version / endian / staleness
// checks, or has malformed event records.
class LpcFormatError : public std::runtime_error
{
public:
  explicit LpcFormatError(const std::string &msg)
    : std::runtime_error(msg) {}
};

// Parse `source_lib_path` and write a Liberty-Parse-Cache (.lpc) file
// to `lpc_path` capturing the LibertyGroupVisitor event stream. The
// .lpc file is a self-sufficient checkpoint of the parser's output;
// subsequent runs can load directly via readLibertyParseCache,
// skipping the lex+parse work.
//
// Like read_liberty, this also leaves the parsed LibertyLibrary
// registered with the network -- the recording is a side effect on
// top of a normal parse. Returns the resulting LibertyLibrary.
LibertyLibrary *
writeLibertyParseCache(std::string_view source_lib_path,
                       std::string_view lpc_path,
                       bool infer_latches,
                       Network *network);

// Load a LibertyLibrary from a .lpc file produced by
// writeLibertyParseCache. Replays the recorded visitor events
// against a fresh LibertyReader, bypassing lex+parse. Returns the
// new LibertyLibrary owned by the network on success.
//
// If ignore_source_check is false, the recorded source-file size
// and mtime are compared against the source at the path stamped in
// the LPC header; a mismatch throws an error rather than serving
// potentially stale data.
LibertyLibrary *
readLibertyParseCache(std::string_view lpc_path,
                      bool ignore_source_check,
                      bool infer_latches,
                      Network *network);

} // namespace sta
