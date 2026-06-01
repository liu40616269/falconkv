add_library(falconkv_meta STATIC
    meta_manager.cpp
    meta_service_impl.cpp
    meta_rpc_client.cpp
    meta_server.cpp
)

target_include_directories(falconkv_meta PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(falconkv_meta PUBLIC
    falconkv_common
    falconkv_proto
    falconkv_transfer
    Threads::Threads
)

if(FALCONKV_HAS_BRPC)
    target_link_libraries(falconkv_meta PUBLIC falconkv_brpc_deps)
endif()

# Standalone Meta master process
add_executable(falconkv_master falconkv_master_main.cpp)
target_link_libraries(falconkv_master PRIVATE falconkv_meta Threads::Threads)
if(FALCONKV_HAS_BRPC)
    target_link_libraries(falconkv_master PRIVATE falconkv_brpc_deps)
endif()
install(TARGETS falconkv_master RUNTIME DESTINATION bin)

