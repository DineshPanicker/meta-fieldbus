# Modbus RTU master v2 (C++23)

100 Hz SCHED_DEADLINE scan-cycle master for the /dev/modbus0 serdev driver.

    include/modbus_uapi.h   ABI shared with the kernel driver
    cpp/                    C++23 master (CMake), binary: modbus_master
    yocto/recipes-apps/     scarthgap recipes, files/ already populated

Host build and tests:
    In Yocto, add this to image install appends:
    IMAGE_INSTALL:append = "modbus-master-cpp"

    cmake -S cpp -B build -DMODBUS_BUILD_TESTS=ON && cmake --build build && (cd build && ctest)

    For running a Debug Address sanitizer build, use this command:
    cmake -S cpp -B build-asan/ -DMODBUS_BUILD_TEST=ON -DCMAKE_BUILD_TYPE=Debug \
>       -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    and :
    cmake --build build-asan/ -j && (cd build-asan/ && ctest --output-on-failure)

    DISCLAIMER: Address sanitizer test cases were not added here
Before running on the Pi the driver needs: O_NONBLOCK -> -EAGAIN in read(),
a .poll handler, and zeroing each record before copy_to_user.
Run one master at a time: both open /dev/modbus0 and request BCM27.
