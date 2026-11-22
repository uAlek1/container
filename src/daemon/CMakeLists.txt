add_executable(container-daemon
  main.cpp
  runtime_service.cpp
  registry.cpp
  shim_manager.cpp
  state_poller.cpp
  recovery.cpp
)

target_include_directories(container-daemon PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(container-daemon PRIVATE
  container-common
  proto_lib
)

target_compile_features(container-daemon PRIVATE cxx_std_23)