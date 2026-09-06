cmake_minimum_required(VERSION 3.25)
project(Alioth VERSION 0.1.0 LANGUAGES CXX)

# Alioth — 跨平台專業 PDF 審閱工作站
# 分層規則見 CLAUDE.md：呈現層 → 應用層 → 領域層 → 引擎轉接層 → 平台層。
# 只有 alioth_engine 可以連結 PDFium；只有 alioth_platform 收斂作業系統差異。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /guard:cf)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  add_link_options(/DYNAMICBASE /NXCOMPAT /GUARD:CF)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(PDFium_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium 預編譯套件位置")
find_package(PDFium REQUIRED PATHS "${PDFium_DIR}" NO_DEFAULT_PATH)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent PrintSupport)

qt_standard_project_setup()

enable_testing()

add_subdirectory(src/domain)
add_subdirectory(src/platform)
add_subdirectory(src/engine)
add_subdirectory(src/app)
add_subdirectory(src/app/uisystem)
add_subdirectory(src/ui)
add_subdirectory(src)
add_subdirectory(i18n)
add_subdirectory(tools/bench)
add_subdirectory(tools/uishot)
add_subdirectory(tests)
