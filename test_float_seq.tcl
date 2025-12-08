# Fix paths
write_liberty_binary ../test_float_seq.lib test_float_seq.blib
read_liberty test_float_seq.blib
puts "Verification complete."
exit
