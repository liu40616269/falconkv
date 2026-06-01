set(TRANSFER_TESTS
    test_transfer_channel
    test_transfer_manager
)

foreach(TEST_NAME ${TRANSFER_TESTS})
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE
        falconkv_transfer
        falconkv_common
        GTest::gtest_main
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
