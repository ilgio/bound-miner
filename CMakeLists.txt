cmake_minimum_required(VERSION 3.25...3.31)

# Milestone 9: the desktop wallet, bound-qt, needs Qt 6.
# - On Linux it's the system's package (qt6-base-dev on Debian or Ubuntu), so it's on by default.
#   Without Qt, everything else still builds.
# - On Windows vcpkg has to build Qt (the "gui" feature in vcpkg.json): one to two hours and several
#   GB of memory the first time. So it's off unless asked for: cmake --preset msvc -DBOUND_GUI=ON.
if(CMAKE_HOST_WIN32)
  set(bound_gui_default OFF)
else()
  set(bound_gui_default ON)
endif()
option(BOUND_GUI "Build the desktop wallet (bound-qt), with Qt 6" ${bound_gui_default})
if(BOUND_GUI)
  list(APPEND VCPKG_MANIFEST_FEATURES "gui")
endif()

# The one place BOUND's version is set (milestone 13): --version, the user agent peers see, the
# Windows file properties and the release packages all take it from here.
project(bound VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# On Windows these come from vcpkg (vcpkg.json). On Linux, from the system's packages (on Debian or
# Ubuntu: libsodium-dev libleveldb-dev nlohmann-json3-dev libgtest-dev), whose libsodium has a
# pkg-config file rather than a CMake package.
find_package(unofficial-sodium CONFIG QUIET)
if(NOT unofficial-sodium_FOUND)
  find_package(PkgConfig REQUIRED)
  # Milestone 12: consensus depends on libsodium's exact Ed25519 verification (non-canonical S and
  # small-order points rejected; see docs/consensus-review.md). 1.0.18 has those rules; older
  # versions could accept signatures newer ones refuse, and split the chain.
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
  add_library(unofficial-sodium::sodium ALIAS PkgConfig::SODIUM)
endif()
find_package(GTest CONFIG REQUIRED)
find_package(leveldb CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  # Keep <windows.h> (pulled in later by the networking code) from defining min/max macros.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Milestone 12: -DBOUND_SANITIZE=address,undefined (GCC or Clang) builds everything with those
# sanitizers, so the tests stop at the first memory error or undefined behaviour. CI runs that.
set(BOUND_SANITIZE "" CACHE STRING "Sanitizers to build with (GCC or Clang), e.g. address,undefined")
if(BOUND_SANITIZE)
  add_compile_options(-fsanitize=${BOUND_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${BOUND_SANITIZE})
endif()
# -DBOUND_FUZZ=libfuzzer (clang): every library is built with libFuzzer's coverage
# instrumentation, so the fuzzer sees which paths through the node an input reaches.
set(BOUND_FUZZ "" CACHE STRING "libfuzzer: build bound_fuzz for clang's libFuzzer")
if(BOUND_FUZZ STREQUAL "libfuzzer")
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Milestone 13: the version, and the git commit recorded at every build (cmake/build_info.cmake), in
# a library of their own so that a new commit recompiles one file.
set(bound_generated ${CMAKE_BINARY_DIR}/generated)
add_custom_target(bound_build_info
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${bound_generated}/bound_build_info.h
          -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${bound_generated}/bound_build_info.h
  COMMENT "Recording the git commit"
  VERBATIM)
add_library(bound_version STATIC src/util/version.cpp)
add_dependencies(bound_version bound_build_info)
target_include_directories(bound_version PUBLIC src PRIVATE ${bound_generated})
target_compile_definitions(bound_version PRIVATE BOUND_VERSION="${PROJECT_VERSION}")

# Everything except main() lives in bound_core, so the tests link the same code boundd runs.
add_library(bound_core STATIC
  src/util/hex.cpp
  src/util/serialize.cpp
  src/crypto/sha256.cpp
  src/core/uint256.cpp
  src/core/pow.cpp
  src/core/block.cpp
  src/core/miner.cpp
  src/core/params.cpp
  src/core/chain.cpp
  # milestone 2
  src/util/base58.cpp
  src/crypto/ed25519.cpp
  src/core/address.cpp
  src/core/transaction.cpp
  src/core/merkle.cpp
  src/core/consensus.cpp
  # milestone 3
  src/core/coins.cpp
  src/core/validation.cpp
  src/core/wallet.cpp
  # milestone 4
  src/core/arith_uint256.cpp
  # milestone 5
  src/core/storage.cpp
  src/core/leveldb_store.cpp
  src/core/chaindb.cpp
  # milestone 6
  src/net/socket.cpp
  src/net/protocol.cpp
  src/net/addrman.cpp
  src/net/peerman.cpp
  src/net/connman.cpp
  # milestone 7
  src/core/amount.cpp
  src/core/mempool.cpp
  src/util/base64.cpp
  src/wallet/crypter.cpp
  src/wallet/wallet.cpp
  src/rpc/http.cpp
  src/rpc/server.cpp
  src/rpc/methods.cpp
  # hardening
  src/net/banman.cpp
  src/net/headerssync.cpp
  # milestone 8
  src/index/chainindex.cpp
  src/util/time.cpp
  src/rpc/client.cpp
  src/explorer/explorer.cpp
  src/net/seeds.cpp
  src/seeder/dns.cpp
  src/seeder/dns_server.cpp
  src/seeder/crawler.cpp
  # milestone 9
  src/node/node.cpp
  # milestone 15
  src/core/genesis.cpp
  # milestone 16
  src/core/boundhash.cpp
  # milestone 17
  src/wallet/mnemonic.cpp
  src/wallet/wordlist.cpp
)
target_include_directories(bound_core PUBLIC src)
target_link_libraries(bound_core PUBLIC bound_version unofficial-sodium::sodium nlohmann_json::nlohmann_json Threads::Threads PRIVATE leveldb::leveldb)
if(WIN32)
  target_link_libraries(bound_core PUBLIC ws2_32)
endif()

# Milestone 13: on Windows each program carries the BOUND icon and its description and version
# (Explorer's Properties > Details), from cmake/program.rc.in.
function(bound_windows_resources target description)
  if(WIN32)
    set(BOUND_PROGRAM ${target})
    set(BOUND_DESCRIPTION "${description}")
    set(BOUND_ICON "${CMAKE_SOURCE_DIR}/src/qt/res/bound.ico")
    configure_file(${CMAKE_SOURCE_DIR}/cmake/program.rc.in ${bound_generated}/${target}.rc @ONLY)
    target_sources(${target} PRIVATE ${bound_generated}/${target}.rc)
  endif()
endfunction()

add_executable(boundd src/app/main.cpp)
target_link_libraries(boundd PRIVATE bound_core)
bound_windows_resources(boundd "BOUND node")

add_executable(bound-cli src/app/cli.cpp)
target_link_libraries(bound-cli PRIVATE bound_core)
bound_windows_resources(bound-cli "BOUND command-line RPC client")

add_executable(bound-explorer src/app/explorer_main.cpp)
target_link_libraries(bound-explorer PRIVATE bound_core)
bound_windows_resources(bound-explorer "BOUND block explorer")

add_executable(bound-seeder src/app/seeder_main.cpp)
target_link_libraries(bound-seeder PRIVATE bound_core)
bound_windows_resources(bound-seeder "BOUND DNS seeder")

enable_testing()
add_executable(bound_tests
  tests/test_hex.cpp
  tests/test_serialize.cpp
  tests/test_sha256.cpp
  tests/test_uint256.cpp
  tests/test_pow.cpp
  tests/test_block.cpp
  tests/test_miner.cpp
  tests/test_params.cpp
  tests/test_chain.cpp
  # milestone 2
  tests/test_base58.cpp
  tests/test_ed25519.cpp
  tests/test_address.cpp
  tests/test_transaction.cpp
  tests/test_merkle.cpp
  tests/test_block_body.cpp
  tests/test_consensus.cpp
  # milestone 3
  tests/test_coins.cpp
  tests/test_validation.cpp
  tests/test_chainstate.cpp
  tests/test_wallet.cpp
  # milestone 4
  tests/test_arith_uint256.cpp
  tests/test_reorg.cpp
  # milestone 5
  tests/test_storage.cpp
  tests/test_persistence.cpp
  # milestone 6
  tests/test_protocol.cpp
  tests/test_addrman.cpp
  tests/test_p2p.cpp
  tests/test_net.cpp
  # milestone 7
  tests/test_mempool.cpp
  tests/test_wallet_hd.cpp
  tests/test_rpc.cpp
  # hardening
  tests/test_banman.cpp
  tests/test_headerssync.cpp
  # milestone 8
  tests/test_index.cpp
  tests/test_explorer.cpp
  tests/test_seeder.cpp
  # milestone 10
  tests/test_networks.cpp
  # milestone 11
  tests/test_asert.cpp
  # milestone 13
  tests/test_version.cpp
  # milestone 15
  tests/test_genesis.cpp
  # milestone 16
  tests/test_boundhash.cpp
  # milestone 17
  tests/test_mnemonic.cpp
)
target_link_libraries(bound_tests PRIVATE bound_core leveldb::leveldb GTest::gtest GTest::gtest_main)
# Milestone 11: test vectors read at run time (tests/data).
target_compile_definitions(bound_tests PRIVATE BOUND_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")

include(GoogleTest)
gtest_discover_tests(bound_tests DISCOVERY_MODE PRE_TEST)

# Milestone 13: every program answers --version with this version.
foreach(program boundd bound-cli bound-explorer bound-seeder)
  add_test(NAME version_${program} COMMAND ${program} --version)
  string(REPLACE "." "\\." version_pattern "${PROJECT_VERSION}")
  set_tests_properties(version_${program} PROPERTIES PASS_REGULAR_EXPRESSION "^${program} ${version_pattern}[ \n]")
endforeach()

# Milestone 12: the fuzz targets (fuzz/fuzz.h). -DBOUND_FUZZ=libfuzzer builds them for clang's
# libFuzzer (coverage-guided; CI runs each for a while); otherwise bound_fuzz mutates seeds itself,
# and ctest runs every target briefly.
add_executable(bound_fuzz fuzz/main.cpp fuzz/targets.cpp fuzz/target_peer.cpp)
target_include_directories(bound_fuzz PRIVATE .)
target_link_libraries(bound_fuzz PRIVATE bound_core)
if(BOUND_FUZZ STREQUAL "libfuzzer")
  target_compile_definitions(bound_fuzz PRIVATE BOUND_LIBFUZZER)
  target_link_options(bound_fuzz PRIVATE -fsanitize=fuzzer)
else()
  add_test(NAME fuzz_targets COMMAND bound_fuzz all --runs 300)
endif()

# Milestone 9: bound-qt, the desktop wallet. Everything but main() is in bound_gui, for its tests.
if(BOUND_GUI)
  find_package(Qt6 6.4 COMPONENTS Widgets)
  if(NOT Qt6_FOUND)
    message(WARNING "Qt 6 not found, so bound-qt won't be built. On Debian or Ubuntu: sudo apt install "
                    "qt6-base-dev. Or configure with -DBOUND_GUI=OFF to stop this warning.")
  endif()
endif()
if(BOUND_GUI AND Qt6_FOUND)
  add_library(bound_gui STATIC
    src/qt/rpc.cpp
    src/qt/nodehost.cpp
    src/qt/style.cpp
    src/qt/format.cpp
    src/qt/session.cpp
    src/qt/dialogs.cpp
    src/qt/overview.cpp
    src/qt/send.cpp
    src/qt/receive.cpp
    src/qt/transactions.cpp
    src/qt/mining.cpp
    src/qt/network.cpp
    src/qt/console.cpp
    src/qt/mainwindow.cpp
    src/qt/bound.qrc
    # milestone 18
    src/qt/contacts.cpp
    src/qt/contactspage.cpp
    src/qt/icons.cpp
    src/qt/qr.cpp
    src/qt/qrcodegen/qrcodegen.cpp
  )
  set_target_properties(bound_gui PROPERTIES AUTOMOC ON AUTORCC ON)
  target_link_libraries(bound_gui PUBLIC bound_core Qt6::Widgets)

  add_executable(bound-qt WIN32 src/qt/main.cpp)
  bound_windows_resources(bound-qt "BOUND wallet")
  target_link_libraries(bound-qt PRIVATE bound_gui)

  # The tests draw without a screen: Qt's "offscreen" platform where there is one (Linux), else
  # "minimal" (vcpkg's Qt on Windows has no offscreen plugin).
  if(TARGET Qt6::QOffscreenIntegrationPlugin OR NOT WIN32)
    set(bound_test_platform offscreen)
  else()
    set(bound_test_platform minimal)
  endif()
  add_executable(bound_qt_tests tests/test_qt.cpp)
  target_link_libraries(bound_qt_tests PRIVATE bound_gui GTest::gtest)
  target_compile_definitions(bound_qt_tests PRIVATE BOUND_TEST_PLATFORM="${bound_test_platform}")
  gtest_discover_tests(bound_qt_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=${bound_test_platform})

  # On Windows Qt looks for its platform plugin in platforms\ beside the program (vcpkg copies the
  # DLLs, not the plugins).
  if(WIN32)
    set(bound_qt_plugins Qt6::QWindowsIntegrationPlugin)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
      list(APPEND bound_qt_plugins Qt6::QOffscreenIntegrationPlugin)
    elseif(TARGET Qt6::QMinimalIntegrationPlugin)
      list(APPEND bound_qt_plugins Qt6::QMinimalIntegrationPlugin)
    endif()
    foreach(target bound-qt bound_qt_tests)
      foreach(plugin ${bound_qt_plugins})
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/platforms"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${plugin}>" "$<TARGET_FILE_DIR:${target}>/platforms/")
      endforeach()
    endforeach()
  endif()
endif()

# Milestone 13: `cmake --install` lays the programs out the way a release ships them (the scripts in
# contrib/release pack that up).
# - Windows: one folder, bound-qt.exe beside every DLL it needs, Qt's platform plugin and the Visual
#   C++ runtime, so it runs on a PC with nothing installed.
# - Linux: bin/ and share/. A release build links its libraries statically (vcpkg's x64-linux).
include(GNUInstallDirs)
set(bound_programs boundd bound-cli bound-explorer bound-seeder)
if(TARGET bound-qt)
  list(APPEND bound_programs bound-qt)
endif()
if(WIN32)
  set(bound_install_doc .)
  install(TARGETS ${bound_programs} RUNTIME_DEPENDENCY_SET bound_dlls RUNTIME DESTINATION .)
  # The DLLs come from vcpkg; Windows' own (System32, the api-ms-* sets) stay behind.
  install(RUNTIME_DEPENDENCY_SET bound_dlls DESTINATION .
    DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin"
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32/")
  if(TARGET bound-qt)
    install(FILES $<TARGET_FILE:Qt6::QWindowsIntegrationPlugin> DESTINATION platforms)
  endif()
  # Visual C++'s runtime (vcruntime140.dll, msvcp140.dll, ...), which Microsoft lets programs carry.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  include(InstallRequiredSystemLibraries)
else()
  set(bound_install_doc ${CMAKE_INSTALL_DOCDIR})
  install(TARGETS ${bound_programs} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY contrib/systemd DESTINATION ${CMAKE_INSTALL_DATADIR}/bound)
  # Milestone 14: sets a server up from the package (contrib/deploy/bound-server).
  install(PROGRAMS contrib/deploy/bound-server DESTINATION ${CMAKE_INSTALL_DATADIR}/bound)
endif()
install(FILES LICENSE DESTINATION ${bound_install_doc})
# The licenses of the libraries built in (vcpkg keeps each one's in share/<port>/copyright).
if(DEFINED VCPKG_INSTALLED_DIR)
  set(bound_shipped_ports libsodium leveldb snappy nlohmann-json)
  if(TARGET bound-qt)
    list(APPEND bound_shipped_ports qtbase double-conversion libpng pcre2 zlib)
  endif()
  foreach(port ${bound_shipped_ports})
    set(copyright "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright")
    if(EXISTS "${copyright}")
      install(FILES "${copyright}" DESTINATION ${bound_install_doc}/licenses RENAME ${port}.txt)
    endif()
  endforeach()
endif()
# Milestone 18: the QR code library built into the wallet app (src/qt/qrcodegen, MIT).
if(TARGET bound-qt)
  install(FILES src/qt/qrcodegen/LICENSE.txt DESTINATION ${bound_install_doc}/licenses RENAME qrcodegen.txt)
endif()
