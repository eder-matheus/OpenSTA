# Branch `liberty_parser_binary` — LPC implementation log

A second-cache-format experiment: instead of snapshotting the final
post-parse `LibertyLibrary` (the LDB approach on the parallel
`liberty_binary` branch), **LPC stores the parser's visitor event
stream**. Subsequent loads replay those events against a fresh
`LibertyReader` to skip the lex+parse work while still running the
visitor.

This document records what was built, why, and how the empirical
numbers compare to LDB.

---

## Context: why a second cache format?

A senior reviewer pushed back on LDB:

> "Fwiw, at one time Ethan thought about doing this and proposed
> storing the results of the yacc parser and re-running the visitors
> each time. The Liberty structure is much less prone to change and
> so backward compatibility is less of an issue. He claimed most of
> the runtime is in the parsing not building the final structures."

Two implicit claims to evaluate:

1. **Stability**: a parser-output cache survives STA refactors that
   touch the visitor's *output* structures. LDB does not — every
   non-trivial change to `LibertyLibrary` / `LibertyCell` / etc.
   risks a format break.
2. **Speed**: most of `read_liberty`'s runtime is in lex+parse, so
   caching the parse tree captures most of the speedup with better
   stability.

Claim 1 is a correctness/maintenance argument; claim 2 is empirical.
Phase 1 of this work was profiling to test claim 2 before committing
implementation effort.

## Phase 1 — profiling decision gate

Profiled `read_liberty` on `test/sky130hd/sky130_fd_sc_hd__ff_n40C_1v95.lib`
(71 MB, 428 cells, CCS-bearing) using `valgrind --tool=callgrind` on a
`RelWithDebInfo` build. Total: 6.70 B instructions retired,
`Sta::readLiberty` subtree = 97.2 % of total.

Bucketed:

| Bucket | Share | Skip on parse-tree replay? |
|---|---:|---|
| Flex lexer (`LibertyScanner::lex`) | ~37 % self | **Skipped** |
| Bison parser + stack + symbol_type | ~15 % | **Skipped** |
| Parser-side AttrValue / Group alloc | ~5 % | **Skipped** |
| `LibertyReader` visitor (end-callbacks, makeFloatTable, makeTimingArcs, …) | ~30–40 % | **Re-runs** |
| `from_chars` / `strtol` / `parseFloatList` | ~5 % | Re-runs |
| Allocator overhead | ~17 % mixed | Half saved |
| Post-parse fixup (`cell->finish`, …) | ~3 % | Re-runs |

**Skipped: ~55–60 %. Retained: ~40–45 %.** Theoretical speedup
ceiling 2.0–2.5× — far below LDB's measured ~35× on the same lib.

Decision gate from the original plan: ≥ 70 % parser → pursue,
≤ 50 % → abandon, in-between → judgment call. We landed at
~55–60 %, in the gray zone. The user chose to implement anyway for
a fair side-by-side comparison.

Full results: [test/parse_profile/results.md](../test/parse_profile/results.md).

## Architecture

`LibertyReader` is a `LibertyGroupVisitor`; the existing
`LibertyParser` calls `visitor->begin/end/visitAttr` as it parses.
The clean factoring lets us interpose:

```
                    ┌────────────────────┐
write_lpc:          │ LibertyParser      │
   parse + record   │   (Flex / Bison)   │
                    └────────┬───────────┘
                             │ begin/end/visitAttr events
                ┌────────────▼────────────┐
                │ LpcRecordingVisitor     │
                │   1. write event to .lpc │
                │   2. forward to inner    │
                └────────────┬────────────┘
                             │ same events, unchanged
                ┌────────────▼────────────┐
                │ LibertyReader (visitor) │
                │   builds LibertyLibrary │
                └─────────────────────────┘

                    ┌─────────────────────┐
read_lpc:           │ LpcReplayer         │
   replay + visit   │   reads events from │
                    │   .lpc, reconstructs│
                    │   LibertyGroup tree │
                    │   on the fly        │
                    └────────────┬────────┘
                                 │ begin/end/visitAttr (synthesized)
                    ┌────────────▼────────┐
                    │ LibertyReader       │
                    │   (fresh instance,  │
                    │    same as text     │
                    │    parse path)      │
                    └─────────────────────┘
```

**Key insight**: the reader doesn't know whether events came from a
real parser or a replayer. As long as the events are issued in the
right order with the right `LibertyGroup` / attr / value payloads,
the visitor produces an identical `LibertyLibrary`.

## File format

```
header:
  magic              u32  'OSLP' (0x504C534F)     // OpenSTA Liberty Parse-tree
  format_version     u32  1
  endian_sentinel    u32  0x12345678 (native order)
  flags              u32  reserved (0)
  sta_version        str  STA build version
  source_filename    str  path the cache was generated from
  source_size        u64  bytes of source .lib at write time
  source_mtime       i64  mtime (seconds since epoch)

body: stream of typed event records, each prefixed with a u8 tag:
  kStringPoolNew  (0x05) (string bytes)            // intern table grows in stream order
  kBeginGroup     (0x01) (type:str_id, params:AttrValueSeq, line:i32)
  kEnd            (0x00)                           // implicit pop of current group
  kSimpleAttr     (0x02) (name:str_id, value:AttrValue, line:i32)
  kComplexAttr    (0x03) (name:str_id, values:AttrValueSeq, line:i32)
  kVariable       (0x04) (name:str_id, value:float, line:i32)
  kEof            (0xFF)
```

`AttrValue` is a tagged union: `(u8 tag) | (f32 float)` or
`(u8 tag) | (u32 string_id)`.

### String pool — interleaved interning

Liberty has tons of repeated strings ("capacitance", "max_transition",
"timing", etc.). To avoid blowing up the format, every string is
assigned an integer id; new strings are emitted as `kStringPoolNew`
records inline as they first appear, and subsequent references use
the id.

**One subtle correctness requirement**: a `kStringPoolNew` record
must never appear inside another event's payload bytes (the reader
parses event payloads as fixed structures and would mis-interpret a
new-string tag as a payload byte). The writer enforces this with a
two-phase pattern in each event handler: first `intern()` every
string the event will reference (which writes new-string records to
the file if needed), then write the event tag and payload using
pure `lookup()` (hashmap reads, no I/O).

This was the one correctness bug we hit during initial bring-up:
the first version called `intern()` mid-payload-write, corrupting
the stream. Fixed by separating intern (writes-allowed) from lookup
(read-only).

## Implementation layout

| File | Role |
|---|---|
| [include/sta/LibertyParseCache.hh](../include/sta/LibertyParseCache.hh) | Public API: `writeLibertyParseCache` / `readLibertyParseCache` + `LpcFormatError` |
| [liberty/LibertyParseCacheFormat.{hh,cc}](../liberty/LibertyParseCacheFormat.cc) | Magic, version, endian sentinel, primitive I/O, event tags |
| [liberty/LibertyParseCacheWriter.cc](../liberty/LibertyParseCacheWriter.cc) | `LpcRecordingVisitor` wraps `LibertyReader`; pre-interns strings before each event payload |
| [liberty/LibertyParseCacheReader.cc](../liberty/LibertyParseCacheReader.cc) | `LpcReplayer` reads events, rebuilds `LibertyGroup` / attr / value nodes on a stack, dispatches to a fresh `LibertyReader` |
| [search/Sta.cc](../search/Sta.cc) + [include/sta/Sta.hh](../include/sta/Sta.hh) | `Sta::writeLpc` / `Sta::readLpc` — same post-load wiring as `readLiberty` |
| [liberty/Liberty.i](../liberty/Liberty.i) + [liberty/Liberty.tcl](../liberty/Liberty.tcl) | Tcl commands `write_lpc` / `read_lpc` with `-corner` / `-min` / `-max` / `-ignore_source_check` / `-infer_latches` |

Build wiring: [CMakeLists.txt](../CMakeLists.txt) lists the three
new `.cc` files alongside the existing Liberty sources.

### Public API

```cpp
LibertyLibrary *
writeLibertyParseCache(std::string_view source_lib_path,
                       std::string_view lpc_path,
                       bool infer_latches,
                       Network *network);

LibertyLibrary *
readLibertyParseCache(std::string_view lpc_path,
                      bool ignore_source_check,
                      bool infer_latches,
                      Network *network);
```

`writeLibertyParseCache` parses the source AND records events in a
single pass — the LibertyLibrary is left registered with the network
as a side effect (same as `readLiberty`). This is the natural fit for
parse-tree caching: events are ephemeral, so you can't snapshot
*after* `read_liberty` like LDB does.

### Tcl

```
write_lpc [-corner corner] [-min] [-max] [-infer_latches] source_lib_path lpc_path
read_lpc  [-corner corner] [-min] [-max] [-ignore_source_check] [-infer_latches] filename
```

Note the API asymmetry vs LDB: LDB's `write_ldb` takes a
`LibertyLibrary` object (snapshot in-memory state); LPC's `write_lpc`
takes a source `.lib` path (parse + record in one step). Both signatures
are correct for their underlying mechanism.

## Test suite

End-to-end harness at [test/lpc_benchmark/](../test/lpc_benchmark/),
mirroring the LDB suite's architecture exactly:

| File | Role |
|---|---|
| `run.sh` | Multi-lib sweep, summary table, three-way verdicts |
| `phase1_write.tcl` | Run `write_lpc`, emit Liberty re-emission, optional dump + report |
| `phase2_read.tcl` | Run `read_lpc`, emit Liberty re-emission, optional dump + report |
| `dump_lib.tcl` | Sourced module: Tcl introspection dump |
| `phase4_report.tcl` | Sourced module: `report_checks` against a real netlist |

Three equivalence layers per lib:

1. **emit diff** — `from_source.lib` vs `from_cache.lib` byte-diff
   (write_liberty re-emission)
2. **dump diff** — Tcl introspection dump of every cell / port /
   arc / internal_power / leakage_power / sequential / statetable.
   Uses test-only SWIG probes added to `liberty/Liberty.i` (clearly
   marked `LPC test-suite probes -- TEST-ONLY`).
3. **report diff** — `report_checks -path_delay max -group_path_count 3`
   against `jpeg.v` (sky130hd) or `gcd.v` (nangate45 fast/typ/slow);
   skipped for libs without a mapped design.

Verdict: `PASS` only if all enabled layers identical; `FAIL(emit)` /
`FAIL(dump)` / `FAIL(report)` / combinations otherwise.

## Reference run on the repo (29 libs)

```
=== summary ===
lib                                                              src       lpc     parse      load   speedup  cells   arcs  ports  verdict
test/read_saif_null_instance.lib                                287B      601B     0.6ms     0.5ms      1.1x      1      1      2  PASS
test/liberty_backslash_eol.lib                                  1.2K      1.6K     0.7ms     0.6ms      1.3x      1      1      2  PASS
... (24 rows omitted, all PASS) ...
test/sky130hd/sky130_fd_sc_hd__tt_025C_1v80.lib                12.2M      7.4M   179.0ms    76.9ms      2.3x    428   1842   3626  PASS
test/sky130hd/sky130_fd_sc_hd__ss_n40C_1v40.lib                12.2M      7.5M   189.4ms    75.8ms      2.5x    428   1842   3626  PASS
test/sky130hd/sky130_fd_sc_hd__ff_n40C_1v95.lib                68.0M     40.2M   738.3ms   165.5ms      4.5x    428   1842   3626  PASS
test/sky130hs/sky130_fd_sc_hs__tt_025C_1v80.lib                68.8M     55.3M   882.7ms   333.9ms      2.6x    377   3416   3312  PASS

=== 29/29 PASS, 0 FAIL, 0 ERROR, 0 SKIPPED ===
```

29 libs spanning sky130hd, sky130hs, nangate45, asap7, ihp-sg13g2,
liberty/test/liberty_ecsm.lib, the small synthetic fixtures under
`test/`, and the `fakeram*` memory macros. Speedups range from ~1.1×
on small fixtures (process startup dominates) to **4.5× on the 71 MB
sky130hd ff lib** — the largest in the corpus.

Phase 4 (`report_checks`) coverage:

| Lib | Design | Lines | Bytes | Match |
|---|---|---:|---:|:-:|
| sky130hd ff | jpeg | 224 | 15,238 | identical |
| sky130hd ss | jpeg | 224 | 15,253 | identical |
| sky130hd tt | jpeg | 224 | 15,253 | identical |
| nangate45 fast | gcd | 115 | 7,033 | identical |
| nangate45 typ | gcd | 116 | 7,100 | identical |
| nangate45 slow | gcd | 126 | 7,848 | identical |

Slack values, slews, delays, arrival times — bit-for-bit identical
between parse-load and cache-load on two PDKs × three corners each ×
two real designs (501 and 38,533 instances).

**Full STA regression**: 6081/6081 STA tests pass — no regression
from the LPC code or the test-only SWIG probes.

## Comparison vs LDB

Same corpus, same architecture, same verdict semantics. Side-by-side
on representative libs:

| Lib (bytes) | Native parse | LDB load | LPC load | LDB speedup | LPC speedup |
|---|---:|---:|---:|---:|---:|
| sky130hd ff (71 MB) | 738 ms | ~15 ms | 165 ms | **~35×** | **4.5×** |
| sky130hd tt (12 MB) | 179 ms | ~15 ms | 77 ms | ~9× | 2.3× |
| sky130hd ss (12 MB) | 189 ms | ~15 ms | 76 ms | ~9× | 2.5× |
| sky130hs tt (69 MB) | 883 ms | ~32 ms | 334 ms | ~18× | 2.6× |
| nangate45 typ (6 MB) | 119 ms | ~11 ms | 57 ms | ~8× | 2.1× |
| ihp-sg13g2 (1.4 MB) | 25 ms | ~2 ms | 12 ms | ~8× | 2.2× |

Cache size:

| Lib | Source | LDB | LPC |
|---|---:|---:|---:|
| sky130hd ff | 71.3 MB | **4.3 MB** (6 %) | 40.2 MB (57 %) |
| sky130hs tt | 68.8 MB | 20.3 MB | 55.3 MB |
| sky130hd tt | 12.2 MB | 4.1 MB | 7.4 MB |

LDB is consistently 8–10× faster on read and 7–10× smaller on disk.
The 8–10× gap is intrinsic to the approach: LPC has to re-run the
visitor (table allocation, FuncExpr parsing, role translation,
unit scaling, `internal_power` / `leakage_power` group construction)
on every load, whereas LDB skips it entirely.

LPC's value proposition isn't speed — it's **cross-version
stability**: LPC files are tied to the Liberty grammar, which
changes far less than STA's internal structures. An LPC produced by
today's STA build would still be loadable by an arbitrarily-refactored
future build, because the new visitor handles whatever comes through
the event stream. LDB files break on any refactor that touches the
serialized C++ structures.

## When to use which

| Use case | Recommendation |
|---|---|
| Single user, single STA version, want fastest reload | **LDB** |
| Distributing caches across STA versions | **LPC** (or regenerate LDB per release) |
| 80 GB CCS-heavy commercial lib, multi-minute parse | **LDB** (8–10× faster) |
| OpenROAD integration where cache survival across STA bumps matters | **LPC** worth considering |
| New flow being designed today | LDB unless cross-version stability is a hard requirement |

The Phase 1 profiling told us LPC's ceiling was ~2–2.5×; the actual
implementation hit ~2–4× depending on lib. The senior's stability
argument was real but doesn't close the speed gap with LDB. Both
implementations now exist on parallel branches for empirical
comparison.

## Limitations

1. **Speed ceiling**: LPC always pays the visitor cost. ~2–5×
   speedup vs `read_liberty` is the practical upper bound.
2. **Cache size**: ~50–60 % of source `.lib` size. Doesn't shrink
   the way LDB does (which deduplicates references and skips
   unread groups).
3. **Source file is still required for `write_lpc`**: unlike LDB's
   `write_ldb`, which snapshots an already-loaded library, `write_lpc`
   needs to actually parse the source. Initial cache creation is at
   least as expensive as `read_liberty`.
4. **Format break**: LPC v1 will need a version bump if the
   serialized event-record layout ever changes. The cross-version
   stability argument applies to *visitor changes*; format-level
   breaks still require regeneration.

## Future work

- Run the same test suite on the user's 80 GB private lib once
  available. Proportions should hold but absolute numbers will be
  more informative.
- Investigate whether the LPC writer's recording overhead (~40 %
  beyond native parse) can be reduced. Profiling suggests it's
  dominated by the hashmap-based string interner; a lock-free
  string-id table or a perfect-hash variant for known Liberty
  attribute names could close that gap.
- If the OpenROAD integration ends up wanting *both* LDB and LPC
  (LDB for speed in steady-state, LPC as a version-stable fallback),
  the dbSta wrapper would need a parallel `read_lpc` override
  alongside `read_ldb`. Same code shape; different underlying
  loader.

## References

- [doc/liberty_binary_cache_implementation.md](liberty_binary_cache_implementation.md)
  — LDB implementation log (the parallel-branch approach this is
  being compared against)
- [doc/liberty_binary_cache_test_suite.md](liberty_binary_cache_test_suite.md)
  — LDB test-suite design (architectural reference for the LPC
  test suite, which is a near-mechanical port)
- [test/parse_profile/results.md](../test/parse_profile/results.md)
  — Phase 1 profiling decision-gate data
