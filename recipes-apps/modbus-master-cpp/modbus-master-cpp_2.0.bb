SUMMARY = "100 Hz SCHED_DEADLINE Modbus RTU scan-cycle master (C++23)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# files/cpp/      CMakeLists.txt, src/, test/
# files/include/  modbus_uapi.h (the same file the driver recipe uses)
SRC_URI = "file://cpp/ \
           file://include/"

# scarthgap: local files unpack into WORKDIR (UNPACKDIR arrives in styhead)
S = "${WORKDIR}/cpp"

inherit cmake

# CMakeLists.txt defaults MODBUS_UAPI_DIR to ../include, which is
# ${WORKDIR}/include here, so nothing extra is needed. Tests stay off.
EXTRA_OECMAKE = "-DMODBUS_BUILD_TESTS=OFF"
