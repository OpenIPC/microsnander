# The native part of the app: the microsnander programmer drivers and
# operations from ../src (the USB device the app opened goes through the usbfs
# code, src/usbfs.c, no libusb), the web mode (src/web.c, remote access) and
# the JNI bridge.
cmake_minimum_required(VERSION 3.22)
project(microsnander C)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)

# the page of the web mode as a C string, as src/Makefile makes web_page.h
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${S}/web_page.html)
file(READ ${S}/web_page.html PAGE)
string(REPLACE "@VER@" "${MS_VERSION}" PAGE "${PAGE}")	# the fonts' ?v=, as src/Makefile does
string(REPLACE "\\" "\\\\" PAGE "${PAGE}")
string(REPLACE "\"" "\\\"" PAGE "${PAGE}")
string(REGEX REPLACE "\n$" "" PAGE "${PAGE}")
string(REPLACE "\n" "\\n\"\n\"" PAGE "${PAGE}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/web_page.h "static const char web_page[] =\n\"${PAGE}\\n\"\n;\n")

# the page's fonts (brass 1.5.0, src/fonts/OFL.txt) as arrays, as src/web_fonts.sh makes them
file(GLOB FONTS ${S}/fonts/*.woff2)
list(SORT FONTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FONTS})
set(FONTS_H "/* made by CMakeLists.txt from src/fonts at build time: do not edit */\n")
set(FONTS_TABLE "")
set(n 0)
foreach(f ${FONTS})
    file(READ ${f} HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HEX "${HEX}")
    get_filename_component(NAME ${f} NAME)
    string(APPEND FONTS_H "static const unsigned char web_font${n}[] = {${HEX}};\n")
    string(APPEND FONTS_TABLE "\t{ \"${NAME}\", web_font${n}, sizeof(web_font${n}) },\n")
    math(EXPR n "${n} + 1")
endforeach()
string(APPEND FONTS_H "static const struct { const char *name; const unsigned char *data; unsigned long len; } web_fonts[] = {\n${FONTS_TABLE}};\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/web_fonts.h "${FONTS_H}")

add_library(microsnander SHARED
    microsnander-jni.c
    ${S}/ops.c ${S}/voltage.c ${S}/selftest.c ${S}/usb.c ${S}/usbfs.c ${S}/programmer.c ${S}/flashcmd_api.c ${S}/spi_controller.c
    ${S}/spi_nand_flash.c ${S}/spi_nor_flash.c ${S}/ch341a_spi.c ${S}/ch347_spi.c ${S}/ezp.c ${S}/timer.c ${S}/web.c)
# web.c takes this page by its full path: a src/web_page.h that make left
# there (it is in .gitignore, maybe older) cannot win over it
target_include_directories(microsnander PRIVATE ${S})
target_compile_definitions(microsnander PRIVATE WEB_PAGE_H="${CMAKE_CURRENT_BINARY_DIR}/gen/web_page.h"
                           WEB_FONTS_H="${CMAKE_CURRENT_BINARY_DIR}/gen/web_fonts.h")
target_compile_options(microsnander PRIVATE -std=gnu99 -Wall -D_FILE_OFFSET_BITS=64)
# the version of _VER in src/main.c, from build.gradle.kts, for the page
target_compile_definitions(microsnander PRIVATE MS_VERSION="${MS_VERSION}")
target_link_libraries(microsnander log)
