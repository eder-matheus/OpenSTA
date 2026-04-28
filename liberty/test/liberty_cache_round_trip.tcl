# Smoke test for the write_liberty_cache / read_liberty_cache Tcl
# bindings. We deliberately do *not* call read_liberty_cache after
# read_liberty in the same process: that would load the same library
# twice into one network and trip Liberty's duplicate-cell scene-map
# warnings, which aren't a real cache bug. The deeper round-trip
# verification (every field per cell / port / arc) lives in
# liberty/test/cpp/TestLibertyCache.cc.
source ../../test/helpers.tcl

read_liberty liberty_ecsm.lib

set lib [lindex [get_libs *] 0]
set cache_file /tmp/sta_libcache_regression.cache
file delete $cache_file

write_liberty_cache $lib $cache_file

if {![file exists $cache_file]} {
  error "write_liberty_cache produced no file"
}
set bytes [file size $cache_file]
if {$bytes < 100} {
  error "cache file is implausibly small: $bytes bytes"
}
puts "wrote cache: ok"
file delete $cache_file
