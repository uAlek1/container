file(GLOB PROTO_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.proto"
)

set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

protobuf_generate(
    LANGUAGE cpp
    OUT_VAR PROTO_GENERATED_FILES
    PROTOS ${PROTO_FILES}
    PROTOC_OUT_DIR ${PROTO_GEN_DIR}
)

add_library(proto_lib STATIC
    ${PROTO_GENERATED_FILES}
)

target_include_directories(proto_lib
    PUBLIC
        ${PROTO_GEN_DIR}
)

target_link_libraries(proto_lib
    PUBLIC
        protobuf::libprotobuf
)