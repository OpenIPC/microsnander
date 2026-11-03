# The native part of the app: the microsnander programmer drivers and
# operations from ../src (the USB device the app opened goes through the usbfs
# code, src/usbfs.c, no libusb), and the JNI bridge.
cmake_minimum_required(VERSION 3.22)
project(microsnander C)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)
add_library(microsnander SHARED
    microsnander-jni.c
    ${S}/ops.c ${S}/voltage.c ${S}/usb.c ${S}/usbfs.c ${S}/programmer.c ${S}/flashcmd_api.c ${S}/spi_controller.c
    ${S}/spi_nand_flash.c ${S}/spi_nor_flash.c ${S}/ch341a_spi.c ${S}/ch347_spi.c ${S}/ezp.c ${S}/timer.c)
target_include_directories(microsnander PRIVATE ${S})
target_compile_options(microsnander PRIVATE -std=gnu99 -Wall -D_FILE_OFFSET_BITS=64)
target_link_libraries(microsnander log)
