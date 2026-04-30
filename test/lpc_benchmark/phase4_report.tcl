# LPC benchmark -- phase 4: functional STA equivalence.
#
# Sourced from phase1_write.tcl and phase2_read.tcl after the library
# has been loaded (parse or cache). Reads a netlist + SDC, runs
# report_checks, and writes the report to a deterministic path. The
# wrapper script diffs the parse-mode report against the cache-mode
# report as a third equivalence check on top of the write_liberty
# diff and the Tcl-introspection dump.
#
# This is the strongest equivalence check: it exercises the actual
# delay-calc path on a real netlist, so any divergence in table
# interpolation, op_cond scaling, OCV derate application, or scene
# selection would surface as different slack values.
#
# Usage (from a phase script that already loaded the library):
#   source phase4_report.tcl
#   ::lpc_bench::report_design $verilog $sdc $top $out_path

namespace eval ::lpc_bench {

proc report_design {verilog sdc top out_path} {
  read_verilog $verilog
  link_design $top
  read_sdc $sdc
  if {[file exists $out_path]} { file delete $out_path }
  # group_path_count=3 -- a small fixed sample of the worst paths per
  # group. Cache-load output should be byte-identical to parse-load.
  report_checks -path_delay max -group_path_count 3 \
                -fields {capacitance slew fanout} -digits 4 \
                > $out_path
}

}  ;# namespace lpc_bench
