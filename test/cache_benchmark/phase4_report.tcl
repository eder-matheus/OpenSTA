# Liberty binary cache benchmark -- phase 4: functional STA equivalence.
#
# Sourced from phase1_write.tcl and phase2_read.tcl after the library
# has been loaded (parse or cache). Reads a netlist + SDC, runs
# report_checks, and writes the report to a deterministic path. The
# wrapper script diffs the parse-mode report against the cache-mode
# report as a third equivalence check on top of the write_liberty diff
# and the Tcl-introspection dump.
#
# This is the strongest equivalence check in the suite: it exercises
# the actual delay-calc path on a real netlist, so anything that
# affects timing numbers (table values, interpolation, scaling at
# op_cond, OCV derate application, scene/min_max selection) is
# covered. If the cache produced a divergent in-memory state at any
# of those layers, slack values would diverge here.
#
# Usage (from a phase script that already loaded the library):
#   source phase4_report.tcl
#   ::cache_bench::report_design $verilog $sdc $top $out_path

namespace eval ::cache_bench {

proc report_design {verilog sdc top out_path} {
  read_verilog $verilog
  link_design $top
  read_sdc $sdc
  if {[file exists $out_path]} { file delete $out_path }
  # group_path_count=3 is a small fixed sample of the worst paths per
  # group. The flow that loaded via cache should produce identical
  # bytes to the flow that loaded via parse -- if it doesn't, slack
  # numbers have drifted somewhere in the cache round-trip.
  report_checks -path_delay max -group_path_count 3 \
                -fields {capacitance slew fanout} -digits 4 \
                > $out_path
}

}  ;# namespace cache_bench
