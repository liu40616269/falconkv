add_library(falconkv_common STATIC
    config.cpp
    status.cpp
    buddy_allocator.cpp
    logging.cpp
)

target_include_directories(falconkv_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(falconkv_common PUBLIC
    Threads::Threads
)

if(FALCONKV_HAS_GLOG)
    target_compile_definitions(falconkv_common PUBLIC FALCONKV_HAS_GLOG)
    target_link_libraries(falconkv_common PUBLIC falconkv_glog)
endif()

target_include_directories(falconkv_common PUBLIC ${JSONCPP_INCLUDE_PATH})
target_link_libraries(falconkv_common PUBLIC ${JSONCPP_LIB})
