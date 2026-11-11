# iOS 交叉编译设置。用法：
#   cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/iOS.cmake
# 模拟器：
#   cmake -S . -B build-ios-sim -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/iOS.cmake \
#         -DHELIOS_IOS_SYSROOT=iphonesimulator
#
# 只做静态库与 .app，签名与打包交给后面的分发流程，所以这里关掉签名。

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 18.0)

# 真机与模拟器只差 sysroot，切换靠这个变量
set(HELIOS_IOS_SYSROOT "iphoneos" CACHE STRING "iOS sysroot: iphoneos or iphonesimulator")
set(CMAKE_OSX_SYSROOT ${HELIOS_IOS_SYSROOT})

# 命令行出包（make / ninja）不需要签名，关掉省事；
# 交给 Xcode 的工程要留给人自己配团队，这里不能替它关。
if(NOT CMAKE_GENERATOR MATCHES "Xcode")
    set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED OFF)
endif()
