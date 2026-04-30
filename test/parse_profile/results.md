# Phase 1 — runtime breakdown of `read_liberty`

## Setup

- Build: `RelWithDebInfo` (optimized + symbols)
- Profiler: `valgrind --tool=callgrind` (counter: `Ir`, instructions retired)
- Workload: `read_liberty test/sky130hd/sky130_fd_sc_hd__ff_n40C_1v95.lib` (71 MB,
  428 cells, CCS-bearing). One iteration.
- Native wall clock: ~570 ms. Under callgrind: ~17.9 s (~30× slowdown,
  expected).
- Total instructions retired: 6.70 B.

`Sta::readLiberty` subtree = **97.19 %** of total program time. The
remaining ~3 % is startup, Tcl init, exit. Profiling is anchored on
the right thing.

## Top contributors

### Lex + parse (the bucket parse-tree caching could skip)

| Function (self) | Self %  |
| --- | ---: |
| `LibertyScanner::lex` (Flex lexer) | **36.78 %** |
| `LibertyParse::parse` (Bison driver) | 8.01 % |
| Bison parser stack push (template) | 3.57 % |
| Bison parser stack push (specialization) | 3.01 % |
| Bison parse-related (.hh / templates) | ~3 % |
| `LibertyParse::stack_symbol_type` ctors / moves | ~2 % |
| `parseTokens` | ~1.85 % |
| `LibExprParse::parse` (FuncExpr text → tree) | 0.28 % |

### Float-string conversion (called from visitor; replay incurs this)

| Function (self) | Self % |
| --- | ---: |
| `std::from_chars(... float ...)` | 2.06 % |
| `strtol` | 0.72 % |

### Visitor (`LibertyReader`, the bucket parse-tree caching CANNOT skip)

| Function (inclusive) | Inclusive % |
| --- | ---: |
| `LibertyReader::end` (group end-callback dispatch) | 18.14 % |
| `LibertyReader::endCell` | 18.07 % |
| `LibertyReader::readCell` | 13.08 % |
| `LibertyReader::readTableModel` (RF overload) | 9.23 % |
| `LibertyReader::readTableModel` (template overload) | 9.17 % |
| `LibertyReader::parseFloatList` | 8.57 % |
| `LibertyReader::makeTimingArcs` | 6.80 % |
| `LibertyReader::makeFloatTable` | 6.77 % |
| `LibertyReader::makeTableModels` | 6.67 % |
| `LibertyReader::readInternalPowerGroups` | 3.48 % |
| `LibertyReader::makeTableAxis` | 2.29 % |
| `LibertyReader::readFloatSeq` | 2.08 % |
| `LibertyReader::parseFunc` (FuncExpr) | 2.05 % |
| `LibertyReader::readCellAttributes` | 1.96 % |
| `LibertyReader::readLeakageGroups` | 1.93 % |

(Inclusive figures overlap parent/child — don't sum them; the
top-level visitor entry `LibertyReader::end` at 18.14 % is the
bounding figure for the cell-end-callback tree.)

### Allocator overhead

| Function (self) | Self % |
| --- | ---: |
| `_int_free` | 4.39 % |
| `malloc` | 3.21 % |
| `_int_malloc` | 2.15 % |
| `free` | 1.91 % |
| `operator new` | 1.15 % |
| `memcpy` (avx2) | 2.06 % |
| `memchr` (avx2) | 2.48 % |
| **malloc+free+new+memcpy+memchr ≈ 17 %** | |

The allocator weight splits between parser (LibertyAttrValue,
LibertyGroup nodes) and visitor (Tables, FuncExprs, TableModels).
A parse-tree cache eliminates the parser side; the visitor side
remains.

## Bucketed verdict

| Bucket | Cost share | Parse-tree cache effect |
| --- | ---: | --- |
| Lex (`LibertyScanner::lex`) | ~37 % | **Skipped on replay** |
| Bison parse + stack + symbol_type | ~15 % | **Skipped on replay** |
| Parser-side AttrValue / Group allocation | ~5 % | **Skipped on replay** |
| `LibertyReader` visitor (end-callbacks + makers) | ~30–40 % | **Re-runs on every replay** |
| Float string→float (`from_chars`/`strtol`/`parseFloatList`) | ~5 % | Re-runs (called from visitor) |
| Allocator overhead | mixed (~17 % total) | Half saved, half retained |
| Post-parse fixup (`cell->finish`, etc.) | ~3 % | Re-runs |

**Skipped (saved by parse-tree cache): ~55–60 %**
**Retained (still incurred on replay): ~40–45 %**

## Speedup ceiling

If a parse-tree cache could replay events with zero overhead beyond
disk reads, the theoretical maximum speedup is:

```
ceiling = 1 / (1 − skip_share)
       = 1 / (1 − 0.60)         (optimistic 60 % skip)
       = 2.5×

       = 1 / (1 − 0.55)         (conservative 55 % skip)
       = 2.2×
```

Add ~5 % for disk-read + event-decode overhead in a real implementation:

```
realistic ≈ 2.0–2.3× speedup
```

## Comparison

| Approach | Skip share | Measured speedup on this lib |
| --- | ---: | ---: |
| Parse-tree cache (Ethan's idea) | ~60 % | ~2× *(estimated; not implemented)* |
| Final-structure cache (LDB, prior branch) | ~95 % | **~35×** *(measured)* |

## Decision

The senior's premise was directionally correct — lex+parse is the
single largest component (lexer alone is 37 %). But the visitor is
not negligible: it's ~35 % of the total, and parse-tree caching
cannot skip any of it.

**Verdict**: parse-tree caching's ~2× speedup is too small to
justify the implementation cost given the LDB approach delivers
measured ~35× on the same lib. The cross-version-stability
advantage of the parse-tree approach is real but doesn't close a
17× speed gap.

**Recommended action**: stick with LDB. The user's 80 GB lib will
likely show a similar split (CCS-heavy libs have proportionally
more table data, which the visitor handles too — visitor cost
should grow alongside parser cost, keeping the ratio similar).
