#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBUSB_DIR="$ROOT_DIR/third_party/libusb"
RTLSDR_DIR="$ROOT_DIR/third_party/rtl-sdr"
JNI_LIBS_DIR="$ROOT_DIR/adab/app/src/main/jniLibs"
BUILD_DIR="$ROOT_DIR/build-android/rtl-sdr"

ABIS="${ABIS:-arm64-v8a armeabi-v7a x86 x86_64}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-24}"
ANDROID_NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-${NDK_HOME:-}}}"

if [[ -z "$ANDROID_NDK" ]]; then
  DEFAULT_NDK="$HOME/Library/Android/sdk/ndk/28.2.13676358"
  if [[ -d "$DEFAULT_NDK" ]]; then
    ANDROID_NDK="$DEFAULT_NDK"
  fi
fi

if [[ -z "$ANDROID_NDK" || ! -x "$ANDROID_NDK/ndk-build" ]]; then
  cat >&2 <<'EOF'
Android NDK not found.

Set one of:
  export ANDROID_NDK=/path/to/android/ndk
  export ANDROID_NDK_HOME=/path/to/android/ndk
  export NDK_HOME=/path/to/android/ndk
EOF
  exit 1
fi

if [[ ! -f "$LIBUSB_DIR/android/jni/Android.mk" ]]; then
  echo "Missing libusb sources at $LIBUSB_DIR" >&2
  echo "Clone with: git clone https://github.com/libusb/libusb.git third_party/libusb" >&2
  exit 1
fi

if [[ ! -f "$RTLSDR_DIR/CMakeLists.txt" ]]; then
  echo "Missing rtl-sdr sources at $RTLSDR_DIR" >&2
  echo "Clone with: git clone https://github.com/osmocom/rtl-sdr.git third_party/rtl-sdr" >&2
  exit 1
fi

echo "Using Android NDK: $ANDROID_NDK"
echo "Building ABIs: $ABIS"
echo "Android platform: $ANDROID_PLATFORM"

echo
echo "==> Building libusb"
(
  cd "$LIBUSB_DIR/android/jni"
  "$ANDROID_NDK/ndk-build" APP_ABI="$ABIS" USE_PC_NAME=0
)

for ABI in $ABIS; do
  LIBUSB_SO="$LIBUSB_DIR/android/libs/$ABI/libusb1.0.so"
  if [[ ! -f "$LIBUSB_SO" ]]; then
    echo "libusb build did not produce $LIBUSB_SO" >&2
    exit 1
  fi
done

for ABI in $ABIS; do
  echo
  echo "==> Building rtl-sdr for $ABI"
  cmake -S "$RTLSDR_DIR" -B "$BUILD_DIR/$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE \
    -DLIBUSB_INCLUDE_DIRS="$LIBUSB_DIR/libusb" \
    -DLIBUSB_LIBRARIES="$LIBUSB_DIR/android/libs/$ABI/libusb1.0.so" \
    -DINSTALL_UDEV_RULES=OFF \
    -DDETACH_KERNEL_DRIVER=OFF \
    -DENABLE_ZEROCOPY=OFF

  cmake --build "$BUILD_DIR/$ABI" --target rtlsdr

  RTLSDR_SO="$BUILD_DIR/$ABI/src/librtlsdr.so"
  if [[ ! -f "$RTLSDR_SO" ]]; then
    echo "rtl-sdr build did not produce $RTLSDR_SO" >&2
    exit 1
  fi

  mkdir -p "$JNI_LIBS_DIR/$ABI"
  cp "$LIBUSB_DIR/android/libs/$ABI/libusb1.0.so" "$JNI_LIBS_DIR/$ABI/"
  cp "$RTLSDR_SO" "$JNI_LIBS_DIR/$ABI/"
done

echo
echo "==> Packaged native libraries:"
find "$JNI_LIBS_DIR" -type f \( -name 'librtlsdr.so' -o -name 'libusb1.0.so' \) | sort

echo
echo "Done. Rebuild the Android app with:"
echo "  cd \"$ROOT_DIR/adab\" && ./gradlew :app:assembleDebug"
