SUMMARY = "SCHED_DEADLINE 100Hz Modbus RTU scan-cycle master"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://modbus_master.c"

S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"

DEPENDS = "libgpiod"
RDEPENDS:${PN} = "libgpiod"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -O2 \
        -o modbus_master modbus_master.c -lgpiod
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 modbus_master ${D}${bindir}/
}
