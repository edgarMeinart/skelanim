cmake_minimum_required(VERSION 3.24)
project(skelanim CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()
include(FetchContent)

# --- Dependencies -----------------------------------------------------------

find_package(OpenGL REQUIRED)

find_package(glfw3 3.3 QUIET)
if(NOT TARGET glfw)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.92.9b GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
set(IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
# imgui: the editor's build. imgui_testhooks: same sources with item hooks for app_tests,
# which locates widgets by label to drive the real UI with synthetic input.
foreach(lib imgui imgui_testhooks)
  add_library(${lib} STATIC ${IMGUI_SOURCES})
  target_include_directories(${lib} PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_link_libraries(${lib} PUBLIC glfw OpenGL::GL)
  if(APPLE)
    target_compile_definitions(${lib} PUBLIC GL_SILENCE_DEPRECATION)
  endif()
endforeach()
target_compile_definitions(imgui_testhooks PUBLIC IMGUI_ENABLE_TEST_ENGINE)

FetchContent_Declare(nfd GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended GIT_TAG v1.4.1 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nfd)

# --- Project targets ----------------------------------------------------------

function(skelanim_setup target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
  if(APPLE)
    target_compile_definitions(${target} PRIVATE GL_SILENCE_DEPRECATION)
  endif()
endfunction()

add_library(ska INTERFACE)
target_include_directories(ska INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(skelanim_core STATIC
  src/core/skeleton_ops.cpp
  src/core/history.cpp)
target_include_directories(skelanim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skelanim_core PUBLIC ska)
skelanim_setup(skelanim_core)

add_library(skelanim_video STATIC
  src/video/video_decoder.cpp)
target_include_directories(skelanim_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(skelanim_video PUBLIC PkgConfig::FFMPEG)
skelanim_setup(skelanim_video)

set(SKELANIM_APP_SOURCES
  src/app/app.cpp
  src/app/settings.cpp
  src/app/tutorial.cpp
  src/app/video_texture.cpp
  src/app/viewport.cpp
  src/app/panels.cpp)

add_executable(skelanim src/app/main.cpp ${SKELANIM_APP_SOURCES})
target_link_libraries(skelanim PRIVATE skelanim_core skelanim_video imgui nfd)
skelanim_setup(skelanim)

add_executable(ska_dump tools/ska_dump.cpp)
target_link_libraries(ska_dump PRIVATE ska)
skelanim_setup(ska_dump)

# --- Tests ------------------------------------------------------------------

add_executable(ska_tests tests/ska_tests.cpp)
target_link_libraries(ska_tests PRIVATE ska)
skelanim_setup(ska_tests)
add_test(NAME ska_tests COMMAND ska_tests)

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE skelanim_core)
skelanim_setup(core_tests)
add_test(NAME core_tests COMMAND core_tests)

add_executable(video_tests tests/video_tests.cpp)
target_link_libraries(video_tests PRIVATE skelanim_video)
skelanim_setup(video_tests)

find_program(FFMPEG_EXE ffmpeg)
if(FFMPEG_EXE)
  add_test(NAME gen_video
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR} sh -c
      "${FFMPEG_EXE} -v error -y -f lavfi -i testsrc=duration=5:size=640x360:rate=30 -pix_fmt yuv420p test.mp4 && ${FFMPEG_EXE} -v error -y -display_rotation 90 -i test.mp4 -c copy test_rot.mp4")
  set_tests_properties(gen_video PROPERTIES FIXTURES_SETUP video_files)
  add_test(NAME video_tests COMMAND video_tests ${CMAKE_BINARY_DIR}/test.mp4 ${CMAKE_BINARY_DIR}/test_rot.mp4)
  set_tests_properties(video_tests PROPERTIES FIXTURES_REQUIRED video_files)

  # Drives the real App (hidden GLFW window) with synthetic ImGui input. Exit 77 = no display.
  add_executable(app_tests tests/app_tests.cpp ${SKELANIM_APP_SOURCES})
  target_link_libraries(app_tests PRIVATE skelanim_core skelanim_video imgui_testhooks nfd)
  skelanim_setup(app_tests)
  add_test(NAME app_tests COMMAND app_tests ${CMAKE_BINARY_DIR}/test.mp4 ${CMAKE_BINARY_DIR}/app_test.ska)
  set_tests_properties(app_tests PROPERTIES FIXTURES_REQUIRED video_files SKIP_RETURN_CODE 77)
endif()
