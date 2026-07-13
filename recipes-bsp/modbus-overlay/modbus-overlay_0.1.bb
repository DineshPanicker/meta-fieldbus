SUMMARY = "Device tree overlay binding the RS485 UART node"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://modbus-overlay.dts"
S = "${UNPACKDIR}"
DEPENDS = "dtc-native"
inherit deploy
do_compile() {
dtc -@ -I dts -O dtb -o ${B}/modbus.dtbo ${S}/modbus-overlay.dts
}
do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/modbus.dtbo ${DEPLOYDIR}/
}

addtask deploy after do_compile before do_build
