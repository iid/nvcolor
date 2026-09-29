cmake_minimum_required(VERSION 3.15)
project(nvcolor C)

if(MSVC)
  enable_language(RC)
endif()

set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(nvcolor WIN32
  src/nvcolor.c
  src/nvapi_min.c
  src/gamma.c
  src/state.c
  src/values.c
)

if(MSVC)
  # nvcolor.rc carries VERSIONINFO only; the application manifest is injected
  # post-link, the same way build-msvc.bat does it. Doing it in the .rc
  # instead depends on rc resolving a relative path against the CWD.
  target_sources(nvcolor PRIVATE src/nvcolor.rc)
  find_program(MT_EXE mt.exe)
  if(MT_EXE)
    add_custom_command(TARGET nvcolor POST_BUILD
      COMMAND "${MT_EXE}" -manifest "${CMAKE_CURRENT_SOURCE_DIR}/src/nvcolor.manifest"
              -outputresource:"$<TARGET_FILE:nvcolor>";#1
      COMMENT "Injecting application manifest")
  else()
    message(WARNING "mt.exe not found; nvcolor.exe will be built without its manifest")
  endif()
else()
  # windres takes one script at a time.
  target_sources(nvcolor PRIVATE src/nvcolor.rc src/nvcolor_res.rc)
endif()

target_link_libraries(nvcolor PRIVATE user32 gdi32)

# Compile-time assertions on every NVAPI id and struct size. Object library
# only - it defines no symbols, so it is never linked.
add_library(nvcolor_verify OBJECT verify_structs.c)
target_include_directories(nvcolor_verify PRIVATE src)
target_compile_definitions(nvcolor_verify PRIVATE
  _WIN32_WINNT=0x0601 WINVER=0x0601 UNICODE _UNICODE NOMINMAX)
target_compile_definitions(nvcolor PRIVATE
  _WIN32_WINNT=0x0601
  WINVER=0x0601
  UNICODE
  _UNICODE
  NOMINMAX
)

# Single-config generators
set_target_properties(nvcolor PROPERTIES
  OUTPUT_NAME "nvcolor"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
  target_compile_options(nvcolor PRIVATE /W3 /O2 /GS /guard:cf)
  target_link_options(nvcolor PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup)
else()
  target_compile_options(nvcolor PRIVATE -Wall -Wextra -O2)
  target_link_options(nvcolor PRIVATE -municode -mwindows -static)
endif()
