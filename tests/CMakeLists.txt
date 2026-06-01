include(FetchContent)

if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Unit tests
add_subdirectory(unit/client)
add_subdirectory(unit/meta)
add_subdirectory(unit/store)
add_subdirectory(unit/transfer)
add_subdirectory(unit/scheduler)

# Module tests
add_subdirectory(module)
