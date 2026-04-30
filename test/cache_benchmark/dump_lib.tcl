# Liberty binary cache benchmark -- structural query dump.
#
# Sourced from phase1_write.tcl and phase2_read.tcl. Walks every
# cell / port / timing arc set / arc via Tcl introspection and emits a
# deterministic line-per-fact text file. The wrapper script diffs the
# parse-mode dump against the cache-mode dump as a second equivalence
# check on top of the write_liberty diff.
#
# This is a stronger equivalence check than write_liberty alone:
# write_liberty silently drops several fields STA does load (notably
# internal_power and leakage_power groups, and most port flag bits),
# whereas the Tcl probe layer surfaces every fact reachable through
# the public introspection API.
#
# Usage:
#   source dump_lib.tcl
#   dump_libraries /tmp/dump.txt

namespace eval ::cache_bench {

# Format a float with fixed precision so two STA processes stamping
# the same value produce identical text. get_property already returns
# a printable string, but for raw float accessors we go through this
# to avoid platform-dependent default formatting.
proc f6 {v} { return [format "%.6g" $v] }

proc safe_prop {obj name} {
  if {[catch {get_property $obj $name} val]} { return "?" }
  return $val
}

proc dump_port {fp port} {
  set name [$port name]
  set dir  [safe_prop $port direction]
  set cap  [safe_prop $port capacitance]
  set func [$port function]
  set tri  [$port tristate_enable]
  set is_bus    [$port is_bus]
  set is_bundle [$port is_bundle]
  set has_mem   [$port has_members]
  set is_pg     [$port is_pwr_gnd]
  set scan      [$port scan_signal_type]

  puts $fp "  port $name dir=$dir cap=[f6 $cap] func={$func} tri={$tri} is_bus=$is_bus is_bundle=$is_bundle has_members=$has_mem pg=$is_pg scan=$scan"

  # Bus / bundle members in iteration order (writer's emission order;
  # the cache must preserve it).
  if {$has_mem} {
    set mi [$port member_iterator]
    while {[$mi has_next]} {
      set m [$mi next]
      puts $fp "    member [$m name] dir=[safe_prop $m direction] cap=[f6 [safe_prop $m capacitance]]"
    }
    $mi finish
  }
}

proc dump_cell {fp cell} {
  set name [get_name $cell]
  puts $fp "cell $name area=[f6 [safe_prop $cell area]] dont_use=[safe_prop $cell dont_use] is_buffer=[safe_prop $cell is_buffer] is_inverter=[safe_prop $cell is_inverter] is_macro=[safe_prop $cell is_macro] is_memory=[safe_prop $cell is_memory] is_clock_gate=[safe_prop $cell is_clock_gate]"

  set port_iter [$cell liberty_port_iterator]
  set ports {}
  while {[$port_iter has_next]} {
    lappend ports [$port_iter next]
  }
  $port_iter finish
  foreach port $ports {
    dump_port $fp $port
  }

  # Timing arc sets in storage order. The cache preserves insertion
  # order across the round-trip; reordering is itself a divergence
  # we want to surface.
  foreach as [$cell timing_arc_sets] {
    set from [$as from]
    set to   [$as to]
    # SWIG's TimingRole / Transition bindings already string-convert
    # via a typemap, so `$as role` and `$arc from_edge` print the
    # name directly -- don't try to call `to_string` on the result.
    set role [$as role]
    set when [$as when]
    set sdf  [$as sdf_cond]
    set fname [expr {$from ne "NULL" ? [$from name] : ""}]
    set tname [expr {$to   ne "NULL" ? [$to   name] : ""}]
    puts $fp "  arc_set from=$fname to=$tname role=$role when={$when} sdf={$sdf}"
    foreach arc [$as timing_arcs] {
      puts $fp "    arc [$arc from_edge] -> [$arc to_edge]"
    }
  }

  # Power groups, sequentials, statetable. These probes go through
  # the cache-benchmark-only SWIG bindings declared at the bottom of
  # Liberty.i. The data is stored in cell->internalPowers() etc. and
  # round-trips through the cache, but isn't reached by either
  # write_liberty (it skips the textual emission) or the standard Tcl
  # introspection API.
  set ip_n [$cell internal_power_count]
  for {set i 0} {$i < $ip_n} {incr i} {
    set ip [$cell internal_power_at $i]
    puts $fp "  internal_power port=[$ip port_name] related=[$ip related_port_name] pg=[$ip related_pg_pin_name] when={[$ip when_str]}"
  }
  set lp_n [$cell leakage_power_count]
  for {set i 0} {$i < $lp_n} {incr i} {
    set lp [$cell leakage_power_at $i]
    puts $fp "  leakage_power pg=[$lp related_pg_port_name] power=[f6 [$lp power_value]] when={[$lp when_str]}"
  }
  set sq_n [$cell sequential_count]
  for {set i 0} {$i < $sq_n} {incr i} {
    set sq [$cell sequential_at $i]
    puts $fp "  sequential is_reg=[$sq is_register] clock={[$sq clock_str]} data={[$sq data_str]} clear={[$sq clear_str]} preset={[$sq preset_str]} q=[$sq output_name] qbar=[$sq output_inv_name]"
  }
  set st [$cell statetable_or_null]
  if {$st ne "NULL"} {
    set in_n [$st input_port_count]
    set int_n [$st internal_port_count]
    set rows [$st row_count]
    set in_names {}
    for {set i 0} {$i < $in_n} {incr i} {
      lappend in_names [[$st input_port_at $i] name]
    }
    set int_names {}
    for {set i 0} {$i < $int_n} {incr i} {
      lappend int_names [[$st internal_port_at $i] name]
    }
    puts $fp "  statetable inputs=\[[join $in_names ,]\] internal=\[[join $int_names ,]\] rows=$rows"
  }
}

proc dump_libraries {out_path} {
  set fp [open $out_path w]

  # Sort libraries by name for determinism (single-lib in our flow,
  # but the sort is free).
  set lib_names {}
  foreach l [get_libs *] { lappend lib_names [list [get_name $l] $l] }
  foreach pair [lsort -index 0 -dictionary $lib_names] {
    set lib [lindex $pair 1]
    puts $fp "library [get_name $lib] filename=[safe_prop $lib filename]"

    # Cells sorted by name. STA's get_lib_cells iteration order isn't
    # documented as stable across load methods, so we sort to immunize
    # the comparison from any future churn there.
    set cell_names {}
    foreach c [get_lib_cells *] { lappend cell_names [list [get_name $c] $c] }
    foreach cpair [lsort -index 0 -dictionary $cell_names] {
      dump_cell $fp [lindex $cpair 1]
    }
  }

  close $fp
}

}  ;# namespace cache_bench
