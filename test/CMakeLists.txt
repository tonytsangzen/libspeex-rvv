add_executable(speex_ab speex_ab.c)
target_link_libraries(speex_ab speex)

# Upstream standalone encoder/decoder sanity checks (narrow/wide/uwb).
foreach(t testenc testenc_wb testenc_uwb)
   add_executable(${t} ${SPEEX_SRC_DIR}/libspeex/${t}.c)
   target_link_libraries(${t} speex)
endforeach()

# Kernel-level differential tests (review M1): verbatim scalar references
# vs the linked library (RVV overrides when enabled). Each is also a CTest
# entry so a plain `ctest` exercises the kernels on any configuration.
foreach(t
   batch5_diff gshelp_diff open_loop_diff vqtail_diff
   inner_prod_diff gainvq_diff qmf_diff filter_diff)
   add_executable(${t} ${t}.c)
   target_link_options(${t} PRIVATE -static)
   if(_USE_RVV AND RVV_MARCH)
      target_compile_options(${t} PRIVATE -march=${RVV_MARCH})
   endif()
   target_include_directories(${t} PRIVATE
      ${CMAKE_SOURCE_DIR}/src/libspeex
      ${CMAKE_SOURCE_DIR}/src/include
      ${CMAKE_SOURCE_DIR}/cmake/conf
      ${CMAKE_BINARY_DIR})
   target_compile_definitions(${t} PRIVATE HAVE_CONFIG_H FIXED_POINT)
   target_link_libraries(${t} speex m)
   add_test(NAME ${t} COMMAND ${t})
endforeach()
set_tests_properties(batch5_diff gshelp_diff open_loop_diff vqtail_diff
   inner_prod_diff gainvq_diff qmf_diff filter_diff PROPERTIES
   ENVIRONMENT "CTEST_OUTPUT_ON_FAILURE=1")
