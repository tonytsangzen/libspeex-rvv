add_executable(speex_ab speex_ab.c)
target_link_libraries(speex_ab speex)

# Upstream standalone encoder/decoder sanity checks (narrow/wide/uwb).
foreach(t testenc testenc_wb testenc_uwb)
   add_executable(${t} ${SPEEX_SRC_DIR}/libspeex/${t}.c)
   target_link_libraries(${t} speex)
endforeach()
