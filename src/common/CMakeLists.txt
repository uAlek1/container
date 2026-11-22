add_library(container-common STATIC
  path.cpp
  framing.cpp
  state_store.cpp
)

target_include_directories(container-common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(container-common PUBLIC
  proto_lib
)

target_compile_features(container-common PUBLIC cxx_std_20)