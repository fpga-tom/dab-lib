Place Android ABI builds of librtlsdr here:

- arm64-v8a/librtlsdr.so
- armeabi-v7a/librtlsdr.so
- x86/librtlsdr.so
- x86_64/librtlsdr.so

The native RTL-SDR HAL loads librtlsdr dynamically by name. Android packages
shared libraries from this directory into the APK under the app native library
directory, where dlopen("librtlsdr.so") can find them.
