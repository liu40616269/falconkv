# python/_pyfalconkv_internal/CMakeLists.txt
# Builds the FalconKVBridge static library and _pyfalconkv_internal C Extension module.

# ==================== FalconKVBridge 静态库 ====================
# 隔离层：C Extension 只依赖此库的头文件，不直接依赖 FalconKVClientImpl
add_library(FalconKVBridge STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/falconkv_bridge.cpp
)
set_target_properties(FalconKVBridge PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(FalconKVBridge PUBLIC
    ${CMAKE_SOURCE_DIR}/src            # FalconKVClientImpl 等头文件
    ${CMAKE_CURRENT_SOURCE_DIR}        # falconkv_bridge.h
)
target_link_libraries(FalconKVBridge PUBLIC
    falconkv_client                    # src/client/ 的 CMake target
)

# ==================== _pyfalconkv_internal C Extension 模块 ====================
add_library(_pyfalconkv_internal MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/_pyfalconkv_internal.cpp
)
set_target_properties(_pyfalconkv_internal PROPERTIES
    PREFIX ""
    SUFFIX ".${Python3_SOABI}.so"       # 生成 _pyfalconkv_internal.cpython-312-x86_64-linux-gnu.so
)
target_include_directories(_pyfalconkv_internal PRIVATE
    ${Python3_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}        # falconkv_bridge.h
)
target_link_libraries(_pyfalconkv_internal PRIVATE
    FalconKVBridge
    ${Python3_LIBRARIES}
    dl
)

# 安装到 pyfalconkv 包目录，使 `import pyfalconkv` 能找到 _pyfalconkv_internal
install(TARGETS _pyfalconkv_internal
    LIBRARY DESTINATION ${CMAKE_SOURCE_DIR}/python/pyfalconkv
)

# 构建后自动复制 .so 到 pyfalconkv 包目录，方便开发调试
add_custom_command(TARGET _pyfalconkv_internal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:_pyfalconkv_internal>
        ${CMAKE_SOURCE_DIR}/python/pyfalconkv/
    COMMENT "Copying _pyfalconkv_internal to pyfalconkv package"
)
