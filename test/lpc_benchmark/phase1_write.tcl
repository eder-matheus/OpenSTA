# LPC benchmark -- phase 1: parse the source .lib, snapshot it as a
# .lpc file, dump a canonical Liberty re-emission for the cross-phase
# diff, and (optionally) emit Tcl introspection + report_checks
# dumps.
#
# Environment:
#
#   STA_LIB_PATH       (required) source .lib to parse
#   STA_LPC_PATH       (required) destination .lpc file
#   STA_GOLDEN_LIB     (required) re-emitted Liberty (golden) for diff
#   STA_DUMP_OUT       (optional) Tcl introspection dump output
#   STA_REPORT_VERILOG (optional) verilog netlist for phase 4
#   STA_REPORT_SDC     (optional) SDC for phase 4
#   STA_REPORT_TOP     (optional) top module for phase 4
#   STA_REPORT_OUT     (optional) report_checks output file

foreach v {STA_LIB_PATH STA_LPC_PATH STA_GOLDEN_LIB} {
  if {![info exists ::env($v)]} {
    puts stderr "phase1: $v must be set"
    exit 1
  }
}

set lib_path    $::env(STA_LIB_PATH)
set lpc_path    $::env(STA_LPC_PATH)
set golden_path $::env(STA_GOLDEN_LIB)

if {![file exists $lib_path]} {
  puts stderr "phase1: source .lib not found: $lib_path"
  exit 1
}

puts "lib_path:    $lib_path"
puts "lib_bytes:   [file size $lib_path]"
puts "lpc_path:    $lpc_path"
puts "golden_path: $golden_path"

# Single combined step: parse + record events + register library.
foreach p {lpc_path golden_path} {
  if {[file exists [set $p]]} { file delete [set $p] }
}

set t0 [clock microseconds]
write_lpc $lib_path $lpc_path
set parse_us [expr {[clock microseconds] - $t0}]
puts "parse_us:    $parse_us"
puts "lpc_bytes:   [file size $lpc_path]"

set libs [get_libs *]
if {[llength $libs] == 0} {
  puts stderr "phase1: write_lpc did not register a library"
  exit 1
}
set lib [lindex $libs 0]
puts "cell_count:  [llength [get_lib_cells *]]"

# Re-emit the in-memory library as Liberty text. Compared against
# phase 2's recovered re-emission.
sta::write_liberty $lib $golden_path
puts "golden_bytes: [file size $golden_path]"

# Optional: Tcl-introspection dump.
if {[info exists ::env(STA_DUMP_OUT)]} {
  source [file dirname [info script]]/dump_lib.tcl
  set dump_path $::env(STA_DUMP_OUT)
  if {[file exists $dump_path]} { file delete $dump_path }
  ::lpc_bench::dump_libraries $dump_path
  puts "dump_path:    $dump_path"
  puts "dump_bytes:   [file size $dump_path]"
}

# Optional: report_checks against a netlist + SDC.
if {[info exists ::env(STA_REPORT_OUT)]
    && [info exists ::env(STA_REPORT_VERILOG)]
    && [info exists ::env(STA_REPORT_SDC)]
    && [info exists ::env(STA_REPORT_TOP)]} {
  source [file dirname [info script]]/phase4_report.tcl
  ::lpc_bench::report_design $::env(STA_REPORT_VERILOG) \
                              $::env(STA_REPORT_SDC) \
                              $::env(STA_REPORT_TOP) \
                              $::env(STA_REPORT_OUT)
  puts "report_path:  $::env(STA_REPORT_OUT)"
  puts "report_bytes: [file size $::env(STA_REPORT_OUT)]"
}
