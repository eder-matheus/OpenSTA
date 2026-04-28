# Smoke test for write_liberty_cache / read_liberty_cache.
# The deeper round-trip verification (every field per cell / port / arc)
# lives in liberty/test/cpp/TestLibertyCache.cc; this script just
# exercises the Tcl bindings end-to-end.
source ../../test/helpers.tcl

read_liberty liberty_ecsm.lib

# Pick the freshly-loaded library; write a cache snapshot.
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

# Reload the cache and confirm the binding is reachable. Use
# -ignore_source_check because the test rig may not preserve the
# source .lib's mtime/size when test fixtures are copied into the
# build tree.
read_liberty_cache -ignore_source_check $cache_file
puts "loaded cache: ok"

file delete $cache_file
