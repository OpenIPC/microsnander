# The native part of the app: the microsnander programmer drivers and
# operations from ../src, libusb, and the JNI bridge.
cmake_minimum_required(VERSION 3.22)
project(microsnander C)

include(FetchContent)
# The libusb of the release binaries (ci/build-musl.sh), checked by SHA256.
FetchContent_Declare(libusb
    URL https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
    URL_HASH SHA256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf)
FetchContent_GetProperties(libusb)
if(NOT libusb_POPULATED)
    FetchContent_Populate(libusb)
endif()
set(U ${libusb_SOURCE_DIR})
add_library(usb-1.0 STATIC
    ${U}/libusb/core.c ${U}/libusb/descriptor.c ${U}/libusb/hotplug.c ${U}/libusb/io.c
    ${U}/libusb/strerror.c ${U}/libusb/sync.c ${U}/libusb/os/linux_usbfs.c
    ${U}/libusb/os/linux_netlink.c ${U}/libusb/os/events_posix.c ${U}/libusb/os/threads_posix.c)
target_include_directories(usb-1.0 PRIVATE ${U}/android ${U}/libusb ${U}/libusb/os)
target_include_directories(usb-1.0 PUBLIC ${U})
target_compile_options(usb-1.0 PRIVATE -w)
# the drivers include <libusb-1.0/libusb.h>
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/inc/libusb-1.0)
file(COPY ${U}/libusb/libusb.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/inc/libusb-1.0)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)
add_library(microsnander SHARED
    microsnander-jni.c
    ${S}/ops.c ${S}/voltage.c ${S}/usb.c ${S}/programmer.c ${S}/flashcmd_api.c ${S}/spi_controller.c
    ${S}/spi_nand_flash.c ${S}/spi_nor_flash.c ${S}/ch341a_spi.c ${S}/ch347_spi.c ${S}/ezp.c ${S}/timer.c)
target_include_directories(microsnander PRIVATE ${S} ${CMAKE_CURRENT_BINARY_DIR}/inc)
target_compile_options(microsnander PRIVATE -std=gnu99 -Wall -D_FILE_OFFSET_BITS=64)
target_link_libraries(microsnander usb-1.0 log)
