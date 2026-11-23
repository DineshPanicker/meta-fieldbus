cmake_minimum_required(VERSION 3.22)
project(modbus_master LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared ABI header: ../include locally; the Yocto recipe passes its own path.
set(MODBUS_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include" CACHE PATH "Directory holding modbus_uapi.h")

set(WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

add_executable(modbus_master
    src/main.cpp
    src/gpio_line.cpp
    src/modbus_port.cpp
    src/rt.cpp)
target_include_directories(modbus_master PRIVATE src ${MODBUS_UAPI_DIR})
target_compile_options(modbus_master PRIVATE ${WARNINGS} -fno-exceptions -fno-rtti)

include(GNUInstallDirs)
install(TARGETS modbus_master RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Host tests (no hardware): frame logic + port logic against a fake driver.
option(MODBUS_BUILD_TESTS "Build host tests" OFF)
if(MODBUS_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(test_frame test/test_frame.cpp)
    target_include_directories(test_frame PRIVATE src)
    target_compile_options(test_frame PRIVATE ${WARNINGS})
    add_test(NAME frame COMMAND test_frame)

    add_executable(test_port test/test_port.cpp src/modbus_port.cpp)
    target_include_directories(test_port PRIVATE src ${MODBUS_UAPI_DIR})
    target_compile_options(test_port PRIVATE ${WARNINGS})
    target_link_libraries(test_port PRIVATE Threads::Threads)
    add_test(NAME port COMMAND test_port)
endif()
