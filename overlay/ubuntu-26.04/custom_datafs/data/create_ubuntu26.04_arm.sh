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

# Keep Mesa at 26.0.3: 26.0.8 shows GPU-rendered windows without alpha in
# greyscale in GPU mode (Zink bug, fixed upstream in 26.1.2)
cat > /etc/apt/preferences.d/mesa.pref <<'EOF'
Package: src:mesa
Pin: version 26.0.3-1ubuntu1
Pin-Priority: 1001
EOF

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

# Shared display socket /run/wayland/wayland-0, reachable by the render group
getent group render >/dev/null || groupadd -r render
usermod -aG render mango
mkdir -p /etc/tmpfiles.d
cat > /etc/tmpfiles.d/wayland.conf <<'EOF'
d /run/wayland 2770 mango render -
EOF
echo 'WAYLAND_DISPLAY=/run/wayland/wayland-0' >> /etc/environment

# Compositors: socket in /run/wayland, not nested (unset WAYLAND_DISPLAY)
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
SyslogIdentifier=weston
UMask=0007
ExecStart=/bin/sh -c 'export XDG_RUNTIME_DIR=/run/wayland; unset WAYLAND_DISPLAY; exec /usr/bin/weston --continue-without-input --socket=wayland-0'
Restart=on-failure

[Install]
WantedBy=graphical.target
EOF
systemctl enable weston.service

# GPU display mode (off by default): labwc compositing on the PowerVR GPU
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
SyslogIdentifier=labwc
# Render on the GPU: GLES2 -> Zink -> PowerVR Vulkan; inherited by apps
Environment=WLR_RENDERER=gles2
Environment=WLR_RENDER_DRM_DEVICE=/dev/dri/by-path/platform-fd00000.gpu-render
Environment=MESA_LOADER_DRIVER_OVERRIDE=zink
Environment=PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
# powervr is a module and can show up after graphical.target
ExecStartPre=/bin/sh -c 'until [ -e $WLR_RENDER_DRM_DEVICE ]; do sleep 0.2; done'
UMask=0007
# labwc takes wayland-0 in the empty /run/wayland
ExecStart=/bin/sh -c 'export XDG_RUNTIME_DIR=/run/wayland; unset WAYLAND_DISPLAY; exec /usr/bin/labwc -V'
Restart=on-failure

[Install]
WantedBy=graphical.target
EOF

cat > /usr/local/bin/display-mode <<'EOF'
#!/bin/sh
# Select what drives the LCD: weston (CPU, default) or gpu (labwc on the GPU).
# gpu also sets the Mesa GPU variables for all logins and services.
VARS="MESA_LOADER_DRIVER_OVERRIDE=zink PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1"
ENVFILE=/etc/environment
# set by older images
PROFILE=/etc/profile.d/gpu-mode.sh
SYSTEMD=/etc/systemd/system.conf.d/gpu-mode.conf
case "$1" in
weston|gpu)
	rm -f $PROFILE
	sed -i -e '/^MESA_LOADER_DRIVER_OVERRIDE=/d' \
		-e '/^PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=/d' $ENVFILE
	;;
esac
case "$1" in
weston)
	on=weston off=labwc
	rm -f $SYSTEMD
	;;
gpu)
	on=labwc off=weston
	for v in $VARS; do echo "$v"; done >> $ENVFILE
	mkdir -p ${SYSTEMD%/*}
	printf '[Manager]\nDefaultEnvironment=%s\n' "$VARS" > $SYSTEMD
	;;
*)
	systemctl -q is-enabled labwc.service && echo gpu || echo weston
	echo "usage: sudo $0 weston|gpu"
	exit 1
	;;
esac
# reload the default environment before starting the compositor
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
