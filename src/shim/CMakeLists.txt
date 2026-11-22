add_executable(your-shim
  main.cpp
  src/shim_service.cpp
  src/container_process.cpp
  src/pty.cpp
  src/log_writer.cpp
)

target_include_directories(your-shim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(your-shim PRIVATE
  your-common
  your-proto
  gRPC::grpc++
)

target_compile_features(your-shim PRIVATE cxx_std_20)