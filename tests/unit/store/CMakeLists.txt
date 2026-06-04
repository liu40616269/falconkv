set(STORE_TESTS
    test_direct_io
    test_aligned_allocator
    test_aligned_buffer_pool
    test_file_naming
    test_node_local_accessor
    test_store_meta_index
    test_meta_sync_client
    test_pending_evict_queue
    test_evict_manager
    test_fd_cache
    test_io_thread_pool
    test_io_uring_engine
)

# Tests that require brpc (Store service / RPC)
set(STORE_RPC_TESTS
    test_store_service_impl
    test_store_rpc_client
)

foreach(TEST_NAME ${STORE_TESTS})
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE
        falconkv_store
        falconkv_common
        GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

if(FALCONKV_HAS_BRPC)
    foreach(TEST_NAME ${STORE_RPC_TESTS})
        add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
        target_link_libraries(${TEST_NAME} PRIVATE
            falconkv_store
            falconkv_common
            GTest::gtest_main
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()
