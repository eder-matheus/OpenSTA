# Profile harness for read_liberty.
#
# We loop the read N times so the workload runs long enough to give
# perf a useful sample count, even at default rates. Each iteration
# is a fresh read_liberty into a separate library entry.

set lib_path $::env(STA_LIB_PATH)
set iters [expr {[info exists ::env(STA_ITERS)] ? $::env(STA_ITERS) : 5}]

puts "lib_path: $lib_path"
puts "lib_bytes: [file size $lib_path]"
puts "iters: $iters"

set t0 [clock microseconds]
for {set i 0} {$i < $iters} {incr i} {
  read_liberty $lib_path
}
set total_us [expr {[clock microseconds] - $t0}]
puts "total_us: $total_us"
puts "per_iter_us: [expr {$total_us / $iters}]"
