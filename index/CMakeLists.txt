# heimdall_index: cross-file symbol index (columnar shards + immutable snapshot).
# Depends on heimdall::semantic only; core and semantic never depend on it.
add_library(heimdall_index STATIC
  src/FileShard.cpp
  src/ShardBuilder.cpp
  src/IndexSnapshot.cpp
  src/IndexResolution.cpp
  src/IndexStore.cpp
)
add_library(heimdall::index ALIAS heimdall_index)
target_include_directories(heimdall_index PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(heimdall_index PUBLIC cxx_std_26)
target_link_libraries(heimdall_index PUBLIC heimdall::semantic)
target_precompile_headers(heimdall_index PRIVATE ${PROJECT_SOURCE_DIR}/cmake/pch.hpp)
