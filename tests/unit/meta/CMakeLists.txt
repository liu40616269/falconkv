set(META_TESTS
    test_buddy_allocator
    test_meta_service_impl
    test_meta_rpc_client
    test_meta_sync_commit
)

foreach(TEST_NAME ${META_TESTS})
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE
        falconkv_meta
        falconkv_common
        GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# The RPC tests additionally need brpc deps
if(FALCONKV_HAS_BRPC)
    target_link_libraries(test_meta_service_impl PRIVATE falconkv_brpc_deps)
    target_link_libraries(test_meta_rpc_client PRIVATE falconkv_brpc_deps)
endif()
