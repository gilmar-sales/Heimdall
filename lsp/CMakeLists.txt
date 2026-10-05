# heimdall-lsp: JSON-RPC stdio server. simdjson stays PRIVATE to this exe.
add_executable(heimdall-lsp
  main.cpp
  Document.cpp
  JsonRpc.cpp
  Server.cpp
)

target_link_libraries(heimdall-lsp PRIVATE heimdall::analysis heimdall::config simdjson::simdjson)
target_compile_features(heimdall-lsp PRIVATE cxx_std_26)

target_precompile_headers(heimdall-lsp PRIVATE
  ${PROJECT_SOURCE_DIR}/cmake/pch.hpp
)
