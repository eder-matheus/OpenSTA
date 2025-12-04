# Liberty Binary Serialization Implementation Plan

## Goal
Implement a binary read/write mechanism for Liberty files to improve loading time by skipping text parsing.
- `read_liberty` will be enhanced to transparently support `.blib` files.
- A new `write_liberty_binary` command will be added to convert Liberty files to binary format.

## User Review Required
- **New Command**: `write_liberty_binary <in_file> <out_file>`
    - Reads `<in_file>` (text or binary) and writes it to `<out_file>` in binary format.
- **Transparent Read**: `read_liberty <file>` will automatically detect binary format based on the `.blib` file extension.
- **Binary Format**: Custom binary format (not standard).

## Proposed Changes

### [liberty]

#### [NEW] [LibertyBinaryWriter.hh](file:///usr/local/google/home/ethanmoon/OpenSTA/liberty/LibertyBinaryWriter.hh)
- Class `LibertyBinaryWriter` inheriting from `LibertyGroupVisitor`.
- Implements `begin`, `end`, `visitAttr`, `visitVariable` to write binary tokens.

#### [NEW] [LibertyBinaryWriter.cc](file:///usr/local/google/home/ethanmoon/OpenSTA/liberty/LibertyBinaryWriter.cc)
- Implementation of `LibertyBinaryWriter`.
- `write_liberty_binary` function that:
    1. Opens output file.
    2. Instantiates `LibertyBinaryWriter`.
    3. Calls `parseLibertyFile(in_filename, &writer, report)`.

#### [NEW] [LibertyBinaryReader.hh](file:///usr/local/google/home/ethanmoon/OpenSTA/liberty/LibertyBinaryReader.hh)
- Class `LibertyBinaryReader`.
- Reads binary tokens and calls `LibertyGroupVisitor` methods (acting as a parser).

#### [NEW] [LibertyBinaryReader.cc](file:///usr/local/google/home/ethanmoon/OpenSTA/liberty/LibertyBinaryReader.cc)
- Implementation of `LibertyBinaryReader`.
- `read_liberty_binary` function (internal helper).

#### [MODIFY] [LibertyReader.cc](file:///usr/local/google/home/ethanmoon/OpenSTA/liberty/LibertyReader.cc)
- Modify `readLibertyFile` to check for `.blib` extension.
- If `.blib`, instantiate `LibertyBinaryReader` and pass the `LibertyReader` instance (as `LibertyGroupVisitor`) to it.

#### [MODIFY] [Liberty.i](file:///usr/local/google/home/ethanmoon/OpenSTA/liberty/Liberty.i)
- Add `write_liberty_binary(char *in_filename, char *out_filename)` command.
    - Implementation:
        1. Create `LibertyBinaryWriter`.
        2. Call `parseLibertyFile(in_filename, &writer, ...)` to parse and write simultaneously.

## Binary Format
- **Magic**: "STALIB01"
- **Version**: uint32_t
- **Tags**:
    - `GROUP_BEGIN` (1)
    - `GROUP_END` (2)
    - `ATTR` (3)
    - `DEFINE` (4)
    - `VARIABLE` (5)
- **Value Types**:
    - `STRING` (1)
    - `FLOAT` (2)
    - `INT` (3) (if needed, else float)
    - `BOOLEAN` (4) (if needed)

## Verification Plan

### Automated Tests
- Create `test/liberty_binary.tcl`:
    1. `write_liberty_binary liberty_float_as_str.lib test.blib`
    2. `read_liberty test.blib`
    3. Verify correctness (check if library is loaded and content matches).
- Run `test/liberty_binary.tcl` using `sta`.

### Manual Verification
- Benchmark loading time on a large Liberty file (if available) to confirm speedup.
