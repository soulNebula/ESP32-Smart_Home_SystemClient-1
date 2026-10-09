# 配置说明版本，项目
cmake_minimum_required(VERSION 3.16)

# 沙箱离线编译时会把组件管理器关掉（IDF_COMPONENT_MANAGER=0）。
# 依赖包已经躺在 managed_components/ 里了，这里手动挂进组件搜索路径，
# 效果和组件管理器自己挂载时完全一样；开了组件管理器就交给它管，避免重复注册。
if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/managed_components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_smart_home)
