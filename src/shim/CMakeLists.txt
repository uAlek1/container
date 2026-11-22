add_executable(shim
  main.cpp
  service.cpp
  container_process.cpp
  # src/pty.cpp
  # src/log_writer.cpp
)

target_include_directories(shim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(shim PRIVATE
  container-common
  proto_lib
)

target_compile_features(shim PRIVATE cxx_std_23)