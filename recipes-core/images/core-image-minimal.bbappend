ROOTFS_POSTPROCESS_COMMAND += "setup_wifi_interfaces;"

setup_wifi_interfaces() {
    printf 'auto lo\niface lo inet loopback\n\nauto wlan0\niface wlan0 inet dhcp\n\tpre-up wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -P /run/wpa_supplicant.pid\n\tpost-down kill $(cat /run/wpa_supplicant.pid) 2>/dev/null || true\n' \
        > ${IMAGE_ROOTFS}/etc/network/interfaces
}
