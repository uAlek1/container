add_executable(container-cli
  main.cpp
  commands.cpp
  client.cpp
  terminal.cpp
)

target_link_libraries(container-cli PRIVATE
  container-common
  proto_lib
)

target_compile_features(container-cli PRIVATE cxx_std_23)