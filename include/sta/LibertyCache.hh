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
//
// The origin of this software must not be misrepresented; you must not
// claim that you wrote the original software.
//
// Altered source versions must be plainly marked as such, and must not be
// misrepresented as being the original software.
//
// This notice may not be removed or altered from any source distribution.

#pragma once

#include <stdexcept>
#include <string>

#include "LibertyClass.hh"

namespace sta {

class StaState;

// Thrown when a cache file fails magic / version / endian /
// source-staleness checks. Distinct from FileNotReadable so callers
// can distinguish I/O failures from validation failures.
class LibertyCacheFormatError : public std::runtime_error
{
public:
  explicit LibertyCacheFormatError(const std::string &msg)
    : std::runtime_error(msg) {}
};

// Serialize a parsed LibertyLibrary to an LDB (Liberty database) file.
// The file is a self-sufficient checkpoint that subsequent runs can
// load via readLibertyCache, skipping the Liberty text parse. Throws
// FileNotWritable on I/O failure.
void
writeLibertyCache(LibertyLibrary *lib,
                  const char *filename,
                  StaState *sta);

// Load a LibertyLibrary from an LDB file produced by
// writeLibertyCache. Returns the new LibertyLibrary owned by the
// network on success.
//
// If ignore_source_check is false (the default flow), the recorded
// source-file size and mtime are compared against the source at the
// path stamped in the LDB header; a mismatch throws an error rather
// than serving potentially stale data. Set ignore_source_check to
// true to skip this check (useful when the LDB is intentionally
// distributed without the source).
//
// Throws FileNotReadable on I/O failure or
// LibertyCacheFormatError on magic / version / endian / staleness
// mismatches.
LibertyLibrary *
readLibertyCache(const char *filename,
                 bool ignore_source_check,
                 StaState *sta);

} // namespace sta
