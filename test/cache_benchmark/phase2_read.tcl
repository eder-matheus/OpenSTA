# Liberty binary cache benchmark -- phase 2: load the cache produced
# by phase 1 in a fresh STA process, time the load, dump a canonical
# Liberty re-emission for the cross-phase diff, and report a few
# structural sanity counts.
#
# Environment:
#
#   STA_CACHE_PATH      (required) cache file to read
#   STA_RECOVERED_LIB   (required) Liberty re-emission output

foreach v {STA_CACHE_PATH STA_RECOVERED_LIB} {
  if {![info exists ::env($v)]} {
    puts stderr "phase2: $v must be set"
    exit 1
  }
}

set cache_path     $::env(STA_CACHE_PATH)
set recovered_path $::env(STA_RECOVERED_LIB)

if {![file exists $cache_path]} {
  puts stderr "phase2: cache file not found: $cache_path"
  exit 1
}

puts "cache_path:     $cache_path"
puts "cache_bytes:    [file size $cache_path]"
puts "recovered_path: $recovered_path"

set t0 [clock microseconds]
# -ignore_source_check so the run isn't tied to whether the source
# .lib is still on disk (deploy scenario: ship cache, drop source).
read_ldb -ignore_source_check $cache_path
set load_us [expr {[clock microseconds] - $t0}]
puts "load_us:        $load_us"

set libs [get_libs *]
if {[llength $libs] == 0} {
  puts stderr "phase2: read_ldb did not register a library"
  exit 1
}
set lib [lindex $libs 0]

# Structural sanity. The byte-level equivalence is decided by the
# golden vs recovered diff in run.sh; these counts give the user a
# quick at-a-glance summary in the per-phase output.
set cells [get_lib_cells *]
puts "cell_count:     [llength $cells]"

set arc_total 0
set port_total 0
foreach cell $cells {
  set arc_total [expr {$arc_total + [llength [$cell timing_arc_sets]]}]
  set iter [$cell liberty_port_iterator]
  while {[$iter has_next]} {
    $iter next
    incr port_total
  }
  $iter finish
}
puts "arc_sets:       $arc_total"
puts "ports:          $port_total"

# Re-emit and write the recovered Liberty. The wrapper script diffs
# this against the phase-1 golden.
if {[file exists $recovered_path]} { file delete $recovered_path }
sta::write_liberty $lib $recovered_path
puts "recovered_bytes: [file size $recovered_path]"

# Optional: dump a Tcl-introspection view of the library. Wrapper sets
# STA_DUMP_OUT when the deep-equivalence diff is enabled.
if {[info exists ::env(STA_DUMP_OUT)]} {
  source [file dirname [info script]]/dump_lib.tcl
  set dump_path $::env(STA_DUMP_OUT)
  if {[file exists $dump_path]} { file delete $dump_path }
  ::cache_bench::dump_libraries $dump_path
  puts "dump_path:    $dump_path"
  puts "dump_bytes:   [file size $dump_path]"
}

# Optional: run report_checks against a netlist + SDC, mirroring
# phase 1's report. The wrapper diffs the two reports as a functional
# STA equivalence check.
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
