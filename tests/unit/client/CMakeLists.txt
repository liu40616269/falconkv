set(CLIENT_TESTS
    test_key_desc_cache
    test_data_structures
)

foreach(TEST_NAME ${CLIENT_TESTS})
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE
        falconkv_client
        falconkv_common
        GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# test_alloc_policy needs falconkv_store for FalconKVStore and NodeLocalAccessor
add_executable(test_alloc_policy test_alloc_policy.cpp)
target_link_libraries(test_alloc_policy PRIVATE
    falconkv_client
    falconkv_store
    falconkv_common
    GTest::gtest_main
)
add_test(NAME test_alloc_policy COMMAND test_alloc_policy)
