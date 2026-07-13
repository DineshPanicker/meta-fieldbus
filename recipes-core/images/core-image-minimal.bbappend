do_image_wic[depends] += "modbus-overlay:do_deploy"

ROOTFS_POSTPROCESS_COMMAND += "unlock_root; setup_wifi; setup_wifi_initd; setup_sshd; setup_wifi_modconf; setup_serial_getty()"

unlock_root() {
    sed -i 's/^root:[^:]*:/root::/' ${IMAGE_ROOTFS}/etc/shadow
}

setup_wifi() {
    printf 'ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n\nnetwork={\n\tssid="Vodafone-41CC"\n\tpsk="pehDfgkP8QcHQhG7"\n}\n' \
        > ${IMAGE_ROOTFS}/etc/wpa_supplicant.conf
    chmod 0600 ${IMAGE_ROOTFS}/etc/wpa_supplicant.conf
}

setup_wifi_initd() {
    printf '#!/bin/sh\n### BEGIN INIT INFO\n# Provides: wifi\n# Required-Start: $local_fs\n# Default-Start: 2 3 4 5\n# Default-Stop: 0 1 6\n### END INIT INFO\nwpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -P /run/wpa_supplicant.pid\nsleep 4\nudhcpc -i wlan0 -t 10 &\n' \
        > ${IMAGE_ROOTFS}/etc/init.d/wifi
    chmod 0755 ${IMAGE_ROOTFS}/etc/init.d/wifi
    mkdir -p ${IMAGE_ROOTFS}/etc/rc5.d
    ln -sf ../init.d/wifi ${IMAGE_ROOTFS}/etc/rc5.d/S99wifi
    mkdir -p ${IMAGE_ROOTFS}/etc/rc2.d
    ln -sf ../init.d/wifi ${IMAGE_ROOTFS}/etc/rc2.d/S99wifi
    mkdir -p ${IMAGE_ROOTFS}/etc/rc3.d
    ln -sf ../init.d/wifi ${IMAGE_ROOTFS}/etc/rc3.d/S99wifi
}

setup_sshd() {
    sed -i 's/#PermitEmptyPasswords no/PermitEmptyPasswords yes/' \
        ${IMAGE_ROOTFS}/etc/ssh/sshd_config
    sed -i 's/#PermitRootLogin.*/PermitRootLogin yes/' \
        ${IMAGE_ROOTFS}/etc/ssh/sshd_config
}

setup_wifi_modconf() {
    echo "options brcmfmac roamoff=1 feature_disable=0x82000" \
        > ${IMAGE_ROOTFS}/etc/modprobe.d/brcmfmac.conf
}

setup_serial_getty() {
    # ensure getty respawns properly on ttyAMA0...
    # but actually the issue is root login
    echo "ttyAMA0::respawn:/sbin/getty -L ttyAMA0 115200 vt100" \
        >> ${IMAGE_ROOTFS}/etc/inittab
}
