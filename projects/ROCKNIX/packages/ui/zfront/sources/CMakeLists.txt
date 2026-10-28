# zfront (C++17). Out of source:   cmake -S cpp -B /home/epsil/zf-target/<you>-cpp/x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
# Adding a module never needs a CMake edit: every src/*.cpp joins the `zf_core` library and every tests/test_*.cpp
# becomes its own ctest executable (CONFIGURE_DEPENDS: re-run `cmake` or just `ninja`, it notices new files).
cmake_minimum_required(VERSION 3.16)
project(zfront LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# no exceptions / RTTI; no FMA contraction so x86 and aarch64 give the same pixels
add_compile_options(-fno-exceptions -fno-rtti -ffp-contract=off -ffunction-sections -fdata-sections)
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")
add_link_options(-Wl,--gc-sections)

# ---- FreeType: dynamic libfreetype.so.6 (it is in the image). The PC has no -dev package: headers come from the
# image sysroot (same API) and the library is linked by file name.
set(ZF_SYSROOT_INC "/home/epsil/aurknix/build.ROCKNIX-RK3326.aarch64/toolchain/aarch64-rocknix-linux-gnu/sysroot/usr/include"
    CACHE PATH "headers used when the host has no freetype/SDL2 -dev packages")
find_path(ZF_FT_INC ft2build.h PATH_SUFFIXES freetype2 HINTS ${ZF_SYSROOT_INC}/freetype2)
find_library(ZF_FT_LIB NAMES freetype libfreetype.so.6 HINTS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
if(NOT ZF_FT_INC OR NOT ZF_FT_LIB)
  message(FATAL_ERROR "FreeType not found (ZF_FT_INC=${ZF_FT_INC} ZF_FT_LIB=${ZF_FT_LIB})")
endif()

# ---- SDL2 backend: only when SDL2 is available (aarch64 sysroot yes, the PC has no libSDL2).
find_path(ZF_SDL_INC SDL.h PATH_SUFFIXES SDL2)
find_library(ZF_SDL_LIB NAMES SDL2)
if(ZF_SDL_INC AND ZF_SDL_LIB)
  set(ZF_HAVE_SDL ON)
else()
  set(ZF_HAVE_SDL OFF)
endif()
option(ZF_SDL "build the SDL2 backend (needs SDL2)" ${ZF_HAVE_SDL})

add_subdirectory(third_party/libwebp)

file(GLOB ZF_SRC CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM ZF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
if(NOT ZF_SDL)
  list(REMOVE_ITEM ZF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/sdlr.cpp)
endif()
# OBJECT, not STATIC: ZF_REGISTER_SCREEN registrars live in static initialisers of otherwise unreferenced files
add_library(zf_core OBJECT ${ZF_SRC} third_party/pugixml/pugixml.cpp)
target_include_directories(zf_core PUBLIC src third_party/stb third_party/nanosvg third_party/pugixml)
# only the freetype2 directory: never the sysroot /usr/include (its libc headers are for another architecture)
target_include_directories(zf_core SYSTEM PUBLIC ${ZF_FT_INC})
target_compile_definitions(zf_core PUBLIC PUGIXML_NO_EXCEPTIONS PUGIXML_NO_XPATH)
# ScreenScraper developer login and software name, taken from the build environment like ES's package does (never committed);
# without them the scraper screens say this build cannot reach ScreenScraper. Only scraper_ss.cpp sees them.
if(NOT "$ENV{SCREENSCRAPER_DEV_LOGIN}" STREQUAL "")
  set_property(SOURCE src/scraper_ss.cpp APPEND PROPERTY COMPILE_DEFINITIONS "ZF_SS_DEV_LOGIN=\"$ENV{SCREENSCRAPER_DEV_LOGIN}\"")
endif()
if(NOT "$ENV{SCREENSCRAPER_SOFTNAME}" STREQUAL "")
  set_property(SOURCE src/scraper_ss.cpp APPEND PROPERTY COMPILE_DEFINITIONS "ZF_SS_SOFTNAME=\"$ENV{SCREENSCRAPER_SOFTNAME}\"")
endif()
target_compile_options(zf_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(zf_core PUBLIC zf_webp ${ZF_FT_LIB})
if(ZF_SDL)
  target_include_directories(zf_core SYSTEM PUBLIC ${ZF_SDL_INC})
  target_link_libraries(zf_core PUBLIC ${ZF_SDL_LIB})
  target_compile_definitions(zf_core PUBLIC ZF_WITH_SDL=1)
endif()
# vendored single-file libs: no warnings from them
set_source_files_properties(third_party/pugixml/pugixml.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/vendor_impl.cpp PROPERTIES COMPILE_OPTIONS "-w;-O2")

add_executable(zfront src/main.cpp)
target_link_libraries(zfront PRIVATE zf_core)

# ---- translations of zfront's own strings: locale/<lang>/LC_MESSAGES/zfront.po -> <build>/locale/<lang>/LC_MESSAGES/zfront.mo
# (domain `zfront`, loaded before ES's `emulationstation2`; the strings ES already translates are not repeated). On the console they go
# to /usr/share/zfront/locale (`make install`); `ZF_LOCALE_DIR` overrides the search path. Adding a language = adding a .po here.
find_program(ZF_MSGFMT msgfmt)
file(GLOB ZF_PO CONFIGURE_DEPENDS locale/*/LC_MESSAGES/zfront.po)
set(ZF_MO "")
if(ZF_MSGFMT)
  foreach(po ${ZF_PO})
    get_filename_component(dir ${po} DIRECTORY)
    get_filename_component(lcm ${dir} NAME)            # LC_MESSAGES
    get_filename_component(lang_dir ${dir} DIRECTORY)
    get_filename_component(lang ${lang_dir} NAME)
    set(mo ${CMAKE_BINARY_DIR}/locale/${lang}/LC_MESSAGES/zfront.mo)
    add_custom_command(OUTPUT ${mo} COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/locale/${lang}/LC_MESSAGES
                       COMMAND ${ZF_MSGFMT} --check-format -o ${mo} ${po} DEPENDS ${po} COMMENT "msgfmt ${lang}/zfront.po")
    list(APPEND ZF_MO ${mo})
  endforeach()
else()
  message(WARNING "msgfmt not found: zfront's own translations (locale/*/zfront.po) are not built")
endif()
add_custom_target(zf_locale ALL DEPENDS ${ZF_MO})
install(DIRECTORY ${CMAKE_BINARY_DIR}/locale/ DESTINATION share/zfront/locale OPTIONAL)

# ---- install: `cmake --install <build> --prefix /usr [--strip]` (DESTDIR works) puts on the target
#   bin/zfront                     the frontend
#   share/zfront/locale/<l>/LC_MESSAGES/zfront.mo   (above: msgfmt at build time)
#   share/zfront/theme-dark|light  the merged Zenith theme folders `--theme` takes (ZF_INSTALL_THEME=OFF when the OS already ships
#                                  /usr/share/themes/es-theme-zenith{,-dark}: boot::resolve_theme finds those too)
#   share/doc/zfront               install.md, os-contract.md and the OS patches
# There is no scripts folder to install: ES event scripts are the user's (`<config>/scripts/<event>/`) and zfront reads the same ones.
install(TARGETS zfront RUNTIME DESTINATION bin)
set(ZF_THEME_SRC "/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-zenith" CACHE PATH
    "es-theme-zenith package folder (theme/ = light, overlay-dark/ = the files that differ in dark), as ES's package builds it")
option(ZF_INSTALL_THEME "install the merged Zenith theme folders to share/zfront" ON)
if(ZF_INSTALL_THEME)
  if(EXISTS ${ZF_THEME_SRC}/theme/layout.xml AND EXISTS ${ZF_THEME_SRC}/overlay-dark)
    # dark = light + overlay-dark, the same merge as es-theme-zenith/package.mk (copy_directory overwrites)
    file(GLOB_RECURSE ZF_THEME_FILES CONFIGURE_DEPENDS ${ZF_THEME_SRC}/theme/* ${ZF_THEME_SRC}/overlay-dark/*)
    set(ZF_THEME_OUT ${CMAKE_BINARY_DIR}/themes)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/themes.stamp
      COMMAND ${CMAKE_COMMAND} -E remove_directory ${ZF_THEME_OUT}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${ZF_THEME_SRC}/theme ${ZF_THEME_OUT}/theme-light
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${ZF_THEME_SRC}/theme ${ZF_THEME_OUT}/theme-dark
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${ZF_THEME_SRC}/overlay-dark ${ZF_THEME_OUT}/theme-dark
      COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/themes.stamp
      DEPENDS ${ZF_THEME_FILES} COMMENT "merging the Zenith theme (light, dark)")
    add_custom_target(zf_themes ALL DEPENDS ${CMAKE_BINARY_DIR}/themes.stamp)
    install(DIRECTORY ${ZF_THEME_OUT}/theme-dark ${ZF_THEME_OUT}/theme-light DESTINATION share/zfront)
  else()
    message(WARNING "Zenith theme not found in ZF_THEME_SRC=${ZF_THEME_SRC}: the theme folders are not installed")
  endif()
endif()
install(FILES docs/install.md docs/os-contract.md DESTINATION share/doc/zfront)
install(DIRECTORY docs/os-patches DESTINATION share/doc/zfront)
# Licences travel with the binary (MIT/zlib/BSD ask for it). They go under share/zfront, NOT share/doc: the image build deletes
# share/doc from every package. LICENSE and NOTICE.md sit in the repo root, one level above cpp/, and next to this file in the
# image package's source snapshot.
foreach(f LICENSE NOTICE.md)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${f} DESTINATION share/zfront/licenses)
  elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../${f})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../${f} DESTINATION share/zfront/licenses)
  endif()
endforeach()
install(FILES third_party/pugixml/LICENSE.txt DESTINATION share/zfront/licenses RENAME pugixml-LICENSE.txt)
install(FILES third_party/nanosvg/nanosvg_license.txt DESTINATION share/zfront/licenses RENAME nanosvg-LICENSE.txt)
install(FILES third_party/libwebp/COPYING DESTINATION share/zfront/licenses RENAME libwebp-COPYING)
install(FILES third_party/libwebp/AUTHORS DESTINATION share/zfront/licenses RENAME libwebp-AUTHORS)
install(FILES third_party/libwebp/PATENTS DESTINATION share/zfront/licenses RENAME libwebp-PATENTS)
install(FILES docs/licenses/OFL-Rubik.txt DESTINATION share/zfront/licenses)

# ---- tests (the image package builds with -DZF_BUILD_TESTS=OFF)
option(ZF_BUILD_TESTS "build the test executables" ON)
if(ZF_BUILD_TESTS)
enable_testing()
file(GLOB ZF_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(t ${ZF_TESTS})
  get_filename_component(n ${t} NAME_WE)
  add_executable(${n} ${t})
  target_link_libraries(${n} PRIVATE zf_core)
  target_include_directories(${n} PRIVATE tests)
  target_compile_definitions(${n} PRIVATE ZF_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data" ZF_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                             ZF_LOCALE_BUILD="${CMAKE_BINARY_DIR}/locale")
  add_dependencies(${n} zf_locale)
  add_test(NAME ${n} COMMAND ${n} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endforeach()
endif()
