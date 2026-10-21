# heimdall_profiling: Perfetto backend for the heimdall::trace sink. Core only emits
# events through function pointers; this library is the sole place that includes
# perfetto.h, and it is a set of stubs unless HEIMDALL_PROFILING=ON.
add_library(heimdall_profiling STATIC
  src/Profiling.cpp
)
add_library(heimdall::profiling ALIAS heimdall_profiling)

target_include_directories(heimdall_profiling PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(heimdall_profiling PUBLIC cxx_std_26)
target_link_libraries(heimdall_profiling PUBLIC heimdall::core)

if (HEIMDALL_PROFILING)
  target_compile_definitions(heimdall_profiling PRIVATE HEIMDALL_PROFILING=1)
  target_link_libraries(heimdall_profiling PRIVATE perfetto)
endif ()
