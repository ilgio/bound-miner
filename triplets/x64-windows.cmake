# vcpkg's own x64-windows triplet, plus the toolset line at the end.
#
# vcpkg builds packages with the newest MSVC toolset installed, while the project is built with
# whichever toolset the developer shell selects, which can be older. A static library from a newer
# toolset doesn't link into an older one's build (leveldb is static-only), so build the packages
# with the shell's toolset too.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

if(DEFINED ENV{VCToolsVersion})
  set(VCPKG_PLATFORM_TOOLSET_VERSION "$ENV{VCToolsVersion}")
endif()
