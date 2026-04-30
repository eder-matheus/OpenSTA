# Liberty binary cache benchmark -- phase 1: parse the source .lib,
# snapshot it to disk, and dump a canonical Liberty re-emission for
# the cross-phase diff. Three environment variables drive the run:
#
#   STA_LIB_PATH       (required) source .lib to parse
#   STA_CACHE_PATH     (required) destination cache file
#   STA_GOLDEN_LIB     (required) re-emitted Liberty (golden) for diff
#
# Output goes to stdout in a parser-friendly form so the wrapper
# script can pick fields out with `awk` / `grep`.

foreach v {STA_LIB_PATH STA_CACHE_PATH STA_GOLDEN_LIB} {
  if {![info exists ::env($v)]} {
    puts stderr "phase1: $v must be set"
    exit 1
  }
}

set lib_path    $::env(STA_LIB_PATH)
set cache_path  $::env(STA_CACHE_PATH)
set golden_path $::env(STA_GOLDEN_LIB)

if {![file exists $lib_path]} {
  puts stderr "phase1: source .lib not found: $lib_path"
  exit 1
}

puts "lib_path:    $lib_path"
puts "lib_bytes:   [file size $lib_path]"
puts "cache_path:  $cache_path"
puts "golden_path: $golden_path"

set t0 [clock microseconds]
read_liberty $lib_path
set parse_us [expr {[clock microseconds] - $t0}]
puts "parse_us:    $parse_us"

set libs [get_libs *]
if {[llength $libs] == 0} {
  puts stderr "phase1: read_liberty did not register a library"
  exit 1
}
set lib [lindex $libs 0]
puts "cell_count:  [llength [get_lib_cells *]]"

# Re-emit the in-memory library as Liberty text. This is the "golden"
# we'll compare against the post-cache-load re-emission in phase 2:
# byte-identical means the cache faithfully preserves everything STA
# captures during read_liberty.
foreach p {cache_path golden_path} {
  if {[file exists [set $p]]} { file delete [set $p] }
}

sta::write_liberty $lib $golden_path
puts "golden_bytes: [file size $golden_path]"

set t1 [clock microseconds]
write_ldb $lib $cache_path
set write_us [expr {[clock microseconds] - $t1}]
puts "write_us:    $write_us"
puts "cache_bytes: [file size $cache_path]"

# Optional: dump a Tcl-introspection view of the library for the
# cross-phase deep-equivalence check. Wrapper sets STA_DUMP_OUT when
# the dump diff is enabled; absent => skip.
if {[info exists ::env(STA_DUMP_OUT)]} {
  source [file dirname [info script]]/dump_lib.tcl
  set dump_path $::env(STA_DUMP_OUT)
  if {[file exists $dump_path]} { file delete $dump_path }
  ::cache_bench::dump_libraries $dump_path
  puts "dump_path:    $dump_path"
  puts "dump_bytes:   [file size $dump_path]"
}

# Optional: run report_checks against a netlist + SDC. Wrapper sets
# STA_REPORT_* when a design is mapped to the current library. The
# cache-mode counterpart in phase 2 runs the same report; the wrapper
# diffs them as a functional STA equivalence check.
if {[info exists ::env(STA_REPORT_OUT)]
    && [info exists ::env(STA_REPORT_VERILOG)]
    && [info exists ::env(STA_REPORT_SDC)]
    && [info exists ::env(STA_REPORT_TOP)]} {
  source [file dirname [info script]]/phase4_report.tcl
  ::cache_bench::report_design $::env(STA_REPORT_VERILOG) \
                                $::env(STA_REPORT_SDC) \
                                $::env(STA_REPORT_TOP) \
                                $::env(STA_REPORT_OUT)
  puts "report_path:  $::env(STA_REPORT_OUT)"
  puts "report_bytes: [file size $::env(STA_REPORT_OUT)]"
}
