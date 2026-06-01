# Module tests — test real module interactions without RPC.

set(MODULE_TESTS
    test_scheduler_service
    test_meta_rpc_service
    test_store_engine
    test_client_batch_ops
)

foreach(TEST_NAME ${MODULE_TESTS})
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE
        falconkv_client
        falconkv_store
        falconkv_meta
        falconkv_scheduler
        falconkv_common
        falconkv_proto
        GTest::gtest_main
        Threads::Threads
    )
    if(FALCONKV_HAS_BRPC)
        target_link_libraries(${TEST_NAME} PRIVATE falconkv_brpc_deps)
    endif()
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# Tests that require brpc (TransferManager / bypass end-to-end)
set(MODULE_BRPC_TESTS
    test_transfer_brpc
    test_bypass_e2e
)

if(FALCONKV_HAS_BRPC)
    foreach(TEST_NAME ${MODULE_BRPC_TESTS})
        add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
        target_link_libraries(${TEST_NAME} PRIVATE
            falconkv_client
            falconkv_store
            falconkv_meta
            falconkv_scheduler
            falconkv_common
            falconkv_proto
            GTest::gtest_main
            Threads::Threads
            falconkv_brpc_deps
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()
