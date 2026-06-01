add_library(falconkv_store STATIC
    falconkv_store.cpp
    aligned_buffer_pool.cpp
    fd_cache.cpp
    node_local_accessor.cpp
    store_service_impl.cpp
    store_rpc_client.cpp
    store_rpc_client_manager.cpp
    store_server.cpp
    store_meta_index.cpp
    meta_sync_client.cpp
    pending_evict_queue.cpp
    evict_manager.cpp
)

target_include_directories(falconkv_store PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(falconkv_store PUBLIC
    falconkv_common
    falconkv_proto
    falconkv_scheduler
    Threads::Threads
)

if(FALCONKV_HAS_BRPC)
    target_link_libraries(falconkv_store PUBLIC falconkv_brpc_deps)
endif()
