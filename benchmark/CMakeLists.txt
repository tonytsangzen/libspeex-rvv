add_executable(bench_e2e bench_e2e.c)
target_link_libraries(bench_e2e speex)

# Batch transcoding harness (plan P2): pthread workers over independent
# streams; per-stream output must stay byte-identical to serial.
find_package(Threads REQUIRED)
add_executable(batch_parallel batch_parallel.c)
target_link_libraries(batch_parallel speex Threads::Threads)
# PROF_GATE needs the library's SPEEX_PROF define (config.h lives in the
# build dir, which is a PRIVATE include of the speex target — not visible
# here); without it the decode-phase profiler gate is a no-op and the
# warmup encode leaks into the attribution.
if(SPEEX_PROF)
  target_compile_definitions(bench_e2e PRIVATE SPEEX_PROF)
endif()
