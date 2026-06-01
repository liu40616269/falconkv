add_library(falconkv_transfer STATIC
    transfer_manager.cpp
    retry_policy.cpp
    brpc_channel.cpp
)

target_include_directories(falconkv_transfer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(falconkv_transfer PUBLIC
    falconkv_common
    falconkv_proto
    Threads::Threads
)

if(FALCONKV_HAS_BRPC)
    target_compile_definitions(falconkv_transfer PUBLIC FALCONKV_HAS_BRPC)
    target_link_libraries(falconkv_transfer PUBLIC falconkv_brpc_deps)
endif()
