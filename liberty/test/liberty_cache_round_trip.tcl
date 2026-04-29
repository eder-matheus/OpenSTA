# Smoke test for the write_ldb / read_ldb Tcl bindings. We
# deliberately do *not* call read_ldb after read_liberty in the same
# process: that would load the same library twice into one network
# and trip Liberty's duplicate-cell scene-map warnings, which aren't
# a real cache bug. The deeper round-trip verification (every field
# per cell / port / arc) lives in liberty/test/cpp/TestLibertyCache.cc.
source ../../test/helpers.tcl

read_liberty liberty_ecsm.lib

set lib [lindex [get_libs *] 0]
set ldb_file /tmp/sta_ldb_regression.ldb
file delete $ldb_file

write_ldb $lib $ldb_file

if {![file exists $ldb_file]} {
  error "write_ldb produced no file"
}
set bytes [file size $ldb_file]
if {$bytes < 100} {
  error "ldb file is implausibly small: $bytes bytes"
}
puts "wrote ldb: ok"
file delete $ldb_file
