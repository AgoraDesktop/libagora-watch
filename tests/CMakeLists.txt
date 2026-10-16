# libagora-watch's own suite.
#
# THE MUTATIONS HAPPEN IN A HELPER PROCESS (a shell), not in the test: a change made by the same thread that
# owns the watch is a different thing from a change made by another, and FSEvents exists for the second. The
# suite waits out the latency window between a mutation and its assertion, which is what makes an
# asynchronous stream testable without a sleep-until-green.

add_executable(eventstream eventstream.c)
target_link_libraries(eventstream PRIVATE Watch CoreFoundation::CoreFoundation Threads::Threads)

add_test(NAME eventstream COMMAND eventstream)

set(_agora_runtime_dirs
    "$<TARGET_FILE_DIR:Watch>"
    "$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
list(REMOVE_DUPLICATES _agora_runtime_dirs)
string(REPLACE ";" ":" _agora_ld_path "${_agora_runtime_dirs}")
set_tests_properties(eventstream PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${_agora_ld_path}")
