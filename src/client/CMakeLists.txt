# Client library
add_library(falconkv_client STATIC
    key_desc_cache.cpp
    falconkv_client_impl.cpp
)

target_include_directories(falconkv_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(falconkv_client PUBLIC
    falconkv_common
    falconkv_proto
    falconkv_store
    falconkv_meta
    falconkv_scheduler
    Threads::Threads
)

if(FALCONKV_HAS_BRPC)
    target_link_libraries(falconkv_client PUBLIC falconkv_brpc_deps)
endif()
