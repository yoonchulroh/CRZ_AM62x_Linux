#!/bin/sh
# Downloads what the build needs that is not part of the TI SDK sysroot, into
# audio-event-recognition/third_party (run once, cached afterwards):
#
#  - the YAMNet TFLite classification model and its class label list
#  - TensorFlow Lite headers and arm64 libraries from the Ubuntu 26.04 archive.
#    They are only used to compile and link aer-infer; the board gets the same
#    library from apt (ubuntu-26.04/custom_datafs/data/create_ubuntu26.04_arm.sh).
set -e

TOP=$(cd "$(dirname "$0")/.." && pwd)
OUT=$TOP/third_party
MIRROR=${UBUNTU_PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}
SUITES="resolute-updates resolute-security resolute"
COMPONENTS="main universe"
# aer-infer links against libtensorflow-lite; the others are its dependencies,
# needed so the linker can check every symbol resolves.
PKGS="libtensorflow-lite2.14.1 libtensorflow-lite-dev libabsl20260107 libcpuinfo0 libfarmhash0 libpthreadpool0"

MODEL_URL=https://storage.googleapis.com/download.tensorflow.org/models/tflite/task_library/audio_classification/android/lite-model_yamnet_classification_tflite_1.tflite
MODEL_SHA256=10c95ea3eb9a7bb4cb8bddf6feb023250381008177ac162ce169694d05c317de

mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---------------------------------------------------------------- model
if [ ! -f "$OUT/yamnet.tflite" ] || [ ! -s "$OUT/yamnet_labels.txt" ]; then
	echo "fetch-deps: downloading the YAMNet model"
	wget -q -O "$TMP/yamnet.tflite" "$MODEL_URL"
	echo "$MODEL_SHA256  $TMP/yamnet.tflite" | sha256sum -c --quiet -
	# The .tflite is also a zip archive carrying the 521 class names.
	if command -v unzip >/dev/null 2>&1; then
		unzip -p "$TMP/yamnet.tflite" yamnet_label_list.txt > "$TMP/yamnet_labels.txt"
	else
		python3 -I -c 'import sys, zipfile; sys.stdout.write(zipfile.ZipFile(sys.argv[1]).read("yamnet_label_list.txt").decode())' \
			"$TMP/yamnet.tflite" > "$TMP/yamnet_labels.txt"
	fi
	if [ "$(wc -l < "$TMP/yamnet_labels.txt")" -lt 520 ]; then
		echo "fetch-deps: unexpected label list in the model" >&2
		exit 1
	fi
	mv "$TMP/yamnet.tflite" "$TMP/yamnet_labels.txt" "$OUT/"
fi

# ---------------------------------------------------- TensorFlow Lite
if [ ! -f "$OUT/tflite/.done" ]; then
	echo "fetch-deps: resolving arm64 packages from $MIRROR"
	: > "$TMP/files"
	for suite in $SUITES; do
		for comp in $COMPONENTS; do
			wget -q -O- "$MIRROR/dists/$suite/$comp/binary-arm64/Packages.xz" | xz -dc |
				awk -v want=" $PKGS " -v tag="$suite" '
					/^Package: / { p = $2 }
					/^Filename: / { if (index(want, " " p " ")) print tag, p, $2 }' \
				>> "$TMP/files" || true
		done
	done
	rm -rf "$OUT/tflite"
	mkdir -p "$OUT/tflite"
	for pkg in $PKGS; do
		# first suite listed wins (updates > security > release)
		file=$(awk -v p="$pkg" '$2 == p { print $3; exit }' "$TMP/files")
		if [ -z "$file" ]; then
			echo "fetch-deps: $pkg not found for arm64 in $SUITES" >&2
			exit 1
		fi
		echo "fetch-deps:   $file"
		wget -q -O "$TMP/pkg.deb" "$MIRROR/$file"
		dpkg-deb -x "$TMP/pkg.deb" "$OUT/tflite"
	done
	touch "$OUT/tflite/.done"
fi

echo "fetch-deps: dependencies ready in $OUT"
