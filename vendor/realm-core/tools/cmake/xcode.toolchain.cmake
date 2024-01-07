if(NOT CMAKE_GENERATOR STREQUAL Xcode)
    message(FATAL_ERROR "Building for Apple platforms is only supported with the Xcode generator.")
endif()

# Callers can pick their own sysroot for packaging purposes, currently only needed for plain macosx builds
if(NOT DEFINED CMAKE_SYSTEM_NAME)
    set(CMAKE_SYSTEM_NAME iOS)
    set(CPACK_SYSTEM_NAME "\$ENV{PLATFORM_NAME}")
endif()

set(CMAKE_XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator appletvos appletvsimulator watchos watchsimulator macosx")
set(CMAKE_XCODE_EFFECTIVE_PLATFORMS "-iphoneos;-iphonesimulator;-appletvos;-appletvsimulator;-watchos;-watchsimulator;-maccatalyst")

set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "9.0")
set(CMAKE_XCODE_ATTRIBUTE_TVOS_DEPLOYMENT_TARGET "9.0")
set(CMAKE_XCODE_ATTRIBUTE_WATCHOS_DEPLOYMENT_TARGET "2.0")
set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "$(MACOSX_DEPLOYMENT_TARGET_CATALYST_$(IS_MACCATALYST))")
set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET_CATALYST_NO "10.9")
set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET_CATALYST_YES "10.15")

if(NOT CMAKE_OSX_ARCHITECTURES)
    # Explicitly enumerate all the possible architectures this build tree can produce.
    # The actual build-time values are overriden below in the ARCHS setting,
    # but CMake needs this list at configure-time to generate multi-arch build rules.
    set(CMAKE_OSX_ARCHITECTURES "arm64;i386;x86_64;armv7;armv7k;arm64_32")
endif()

set(CMAKE_XCODE_ATTRIBUTE_ARCHS "$(ARCHS_STANDARD)")
set(CMAKE_XCODE_ATTRIBUTE_ARCHS[sdk=iphoneos*] "armv7 arm64")

set(CMAKE_XCODE_ATTRIBUTE_REALM_ALIGN_FLAG "")
set(CMAKE_XCODE_ATTRIBUTE_REALM_ALIGN_FLAG[arch=armv7] "-fno-aligned-new")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} $(REALM_ALIGN_FLAG)")

# Bitcode
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE[sdk=iphone*] "YES")
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE[sdk=appletv*] "YES")
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE[sdk=watch*] "YES")
set(CMAKE_XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE "\$(BITCODE_GENERATION_MODE_\$(CONFIGURATION))")
set(CMAKE_XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE_Debug "marker")
set(CMAKE_XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE_Release "bitcode")
set(CMAKE_XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE_RelWithDebInfo "bitcode")
set(CMAKE_XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE_MinSizeRel "bitcode")

set(REALM_ENABLE_ASSERTIONS ON CACHE BOOL "Enable release assertions")
set(REALM_XCODE_TOOLCHAIN TRUE)
