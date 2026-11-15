#!/bin/sh
set -e

useradd -G sudo -m -s /bin/bash mango
echo mango:mango | chpasswd

passwd root <<EOF
root
root
EOF

mkdir -p /etc/network

echo mango > /etc/hostname
echo 127.0.0.1 localhost > /etc/hosts
echo 127.0.0.1 mango >> /etc/hosts
echo "nameserver 8.8.8.8" > /etc/resolv.conf
echo "/dev/mmcblk1p2	/	ext4	defaults,noatime	0	1" >> /etc/fstab
echo "auto lo" > /etc/network/interfaces
echo "iface lo inet loopback" >> /etc/network/interfaces

mknod -m 666 /dev/urandom c 1 9
rm -f /dev/null && mknod -m 666 /dev/null c 1 3
mknod -m 666 /dev/zero c 1 5
mknod -m 666 /dev/random c 1 8

rm -f /bin/sh
ln -s /bin/bash /bin/sh
mkdir -p /data/db

mount -t proc proc /proc

apt-get update -y
apt-get upgrade -y

apt-get install locales -y
locale-gen en_US.UTF-8
apt-get install dialog perl -y
apt-get install sudo net-tools ethtool udev iputils-ping wget apt-utils nano -y
apt-get install network-manager netplan.io -y

apt-get install systemd -y
apt-get install vim udhcpc -y
apt-get install systemd-sysv nfs-common -y

# Login prompt on the AM62x debug UART (ttyS2)
mkdir -p /etc/systemd/system/getty.target.wants
ln -sf /usr/lib/systemd/system/serial-getty@.service /etc/systemd/system/getty.target.wants/serial-getty@ttyS2.service

apt-get install openjdk-21-jdk curl dirmngr gnupg apt-transport-https ca-certificates software-properties-common -y

apt-get install openssh-server -y

echo 'iperf3 iperf3/start_daemon boolean false' | debconf-set-selections
apt-get install iperf3 evtest kmod mtd-utils rdate mmc-utils libts-dev libts-bin -y
apt-get install gpiod mplayer -y
#apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-doc gstreamer1.0-tools gstreamer1.0-x gstreamer1.0-alsa gstreamer1.0-gl gstreamer1.0-gtk3 gstreamer1.0-qt5 gstreamer1.0-pulseaudio -y

apt-get install alsa-tools alsa-utils devmem2 usbutils fbset libdrm-tests -y

apt-get install weston glmark2-es2-drm glmark2-es2-wayland mesa-utils freeglut3-dev libglu1-mesa-dev mesa-common-dev -y

apt-get install fdisk -y

cat > /etc/systemd/system/weston.service <<'EOF'
[Unit]
Description=Weston Wayland compositor
Requires=systemd-user-sessions.service
After=systemd-user-sessions.service
Conflicts=getty@tty1.service
After=getty@tty1.service

[Service]
User=mango
PAMName=login
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
TTYVTDisallocate=yes
UtmpIdentifier=tty1
UtmpMode=user
StandardOutput=journal
StandardError=journal
ExecStart=/usr/bin/weston --continue-without-input
Restart=on-failure

[Install]
WantedBy=graphical.target
EOF
systemctl enable weston.service

# GPU display mode, off by default: labwc compositing on the PowerVR GPU
# (wlroots GLES2 renderer -> Mesa Zink -> Mesa PowerVR Vulkan driver) in place
# of Weston, which composites on the CPU. Select with "sudo display-mode gpu"
# and go back with "sudo display-mode weston".
apt-get install labwc mesa-vulkan-drivers -y

cat > /etc/systemd/system/labwc.service <<'EOF'
[Unit]
Description=labwc Wayland compositor rendering on the PowerVR GPU (GPU display mode)
Requires=systemd-user-sessions.service
After=systemd-user-sessions.service
Conflicts=getty@tty1.service weston.service
After=getty@tty1.service

[Service]
User=mango
PAMName=login
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
TTYVTDisallocate=yes
UtmpIdentifier=tty1
UtmpMode=user
StandardOutput=journal
StandardError=journal
# wlroots GLES2 renderer -> Mesa Zink -> Mesa PowerVR Vulkan driver (render
# node of the GPU); the frame buffers are allocated on tidss (the display).
# Apps started by labwc inherit these, so their GLES and Vulkan also run on
# the GPU (GLES 2.0 only; unset MESA_LOADER_DRIVER_OVERRIDE for an app that
# needs more, it then renders on the CPU as in Weston mode).
Environment=WLR_RENDERER=gles2
Environment=WLR_RENDER_DRM_DEVICE=/dev/dri/by-path/platform-fd00000.gpu-render
Environment=MESA_LOADER_DRIVER_OVERRIDE=zink
Environment=PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
# powervr is a module and can show up after graphical.target
ExecStartPre=/bin/sh -c 'until [ -e $WLR_RENDER_DRM_DEVICE ]; do sleep 0.2; done'
ExecStart=/usr/bin/labwc -V
Restart=on-failure

[Install]
WantedBy=graphical.target
EOF

cat > /usr/local/bin/display-mode <<'EOF'
#!/bin/sh
# Select what drives the LCD, now and on the next boots:
#   weston - Weston, composited on the CPU (default)
#   gpu    - labwc, composited on the PowerVR GPU
#
# GPU mode also sets the two Mesa variables that put GLES and Vulkan programs
# on the GPU for the whole system, not only for programs started by labwc:
# for login shells (from the next login) and for systemd services (from their
# next start). Weston mode removes them again.
VARS="MESA_LOADER_DRIVER_OVERRIDE=zink PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1"
PROFILE=/etc/profile.d/gpu-mode.sh
SYSTEMD=/etc/systemd/system.conf.d/gpu-mode.conf
case "$1" in
weston)
	on=weston off=labwc
	rm -f $PROFILE $SYSTEMD
	;;
gpu)
	on=labwc off=weston
	for v in $VARS; do echo "export $v"; done > $PROFILE
	mkdir -p ${SYSTEMD%/*}
	printf '[Manager]\nDefaultEnvironment=%s\n' "$VARS" > $SYSTEMD
	;;
*)
	systemctl -q is-enabled labwc.service && echo gpu || echo weston
	echo "usage: sudo $0 weston|gpu"
	exit 1
	;;
esac
# systemd re-reads its default environment here, before the compositor starts
systemctl daemon-reload
systemctl disable --now $off.service
systemctl enable --now $on.service
EOF
chmod 755 /usr/local/bin/display-mode

curl -fsSL https://deb.nodesource.com/setup_current.x | sudo -E bash - &&sudo apt-get install -y nodejs

mkdir -p /etc/netplan
cat > /etc/netplan/01-network-manager-all.yaml <<EOF
network:
  version: 2
  renderer: NetworkManager
EOF
chmod 600 /etc/netplan/01-network-manager-all.yaml
apt-get purge -y ifupdown || true

umount /proc

echo "DNS=8.8.8.8" >> /etc/systemd/resolved.conf
ln -sf ../run/systemd/resolve/stub-resolv.conf /etc/resolv.conf

echo "Enter exit on the shell to continue.."
