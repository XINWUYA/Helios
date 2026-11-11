# iOS 交叉编译设置。用法：
#   cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/iOS.cmake
#
# 只做静态库与 .app，签名与打包交给后面的分发流程，所以这里关掉签名。

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 18.0)
set(CMAKE_OSX_SYSROOT iphoneos)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED OFF)
