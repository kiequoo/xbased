# hakuX native core (QEMU-free): FEXCore fork + reimplemented Xbox kernel.
#
# hakux_native is the core library; xboxrun runs a title from the command
# line on the device (adb shell) for the frontier loop. Cross-compiled with
# the NDK into build/native/core; the steps are in android/README.md.
cmake_minimum_required(VERSION 3.20)
project(hakux_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ThinLTO for Release builds of everything but FEX (its own ENABLE_LTO stays
# off, fex.cmake): the decoder and the renderer ~2% lighter (the RP5, DOA2).
# The cache keeps relinking after a change to the files changed.
option(HAKUX_LTO "ThinLTO in Release builds (FEX excepted)" ON)
if(HAKUX_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT nc_ipo LANGUAGES C CXX)
  if(nc_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    add_link_options("-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
  endif()
endif()

get_filename_component(NC_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# Linux on x86-64 (not Android): the PC edition's other platform, the same
# translator as Windows (cmake/linux.cmake)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(NC_LINUX_X64 ON)
endif()
# Linux on arm64 (not Android): FEX as the CPU backend, SDL3 desktop frontend.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(NC_LINUX_ARM64 ON)
endif()
if(NC_LINUX_X64)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux.cmake")
elseif(NC_LINUX_ARM64)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux_arm64.cmake")
  set(NC_XXHASH_DIR "${HAKUX_FEX_DIR}/External/xxhash")
elseif(WIN32)
  # Windows x64: the guest CPU is the core's own x86-32 -> x86-64 translator
  # (cpu/cpu_x86.cpp, on Zydis) instead of FEX; cmake/win.cmake.
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/win.cmake")
else()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fex.cmake")
  set(NC_XXHASH_DIR "${HAKUX_FEX_DIR}/External/xxhash")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hle.cmake")

# libslirp (NC_DEPS/libslirp, 4.7.0, unmodified): the NAT backend's TCP/IP
# stack (net/nat.c), on the GLib subset plus net/slirp/glib.h. Its public
# header is copied beside the generated version header, so the core's
# include path gets only those two.
set(NC_SLIRP "${NC_DEPS}/libslirp/src")
set(SLIRP_MAJOR_VERSION 4)
set(SLIRP_MINOR_VERSION 7)
set(SLIRP_MICRO_VERSION 0)
set(SLIRP_VERSION_STRING "\"4.7.0\"")
configure_file("${NC_SLIRP}/libslirp-version.h.in" "${CMAKE_BINARY_DIR}/slirp/libslirp-version.h" @ONLY)
if(WIN32)
  # linked statically: no dllimport/dllexport on its functions
  file(READ "${NC_SLIRP}/libslirp.h" nc_slirp_h)
  string(REPLACE "__declspec(dllexport)" "" nc_slirp_h "${nc_slirp_h}")
  string(REPLACE "__declspec(dllimport)" "" nc_slirp_h "${nc_slirp_h}")
  file(WRITE "${CMAKE_BINARY_DIR}/slirp/libslirp.h" "${nc_slirp_h}")
else()
  configure_file("${NC_SLIRP}/libslirp.h" "${CMAKE_BINARY_DIR}/slirp/libslirp.h" COPYONLY)
endif()
add_library(nc_slirp STATIC
  net/slirp/glib.c
  "${NC_SLIRP}/arp_table.c" "${NC_SLIRP}/bootp.c" "${NC_SLIRP}/cksum.c" "${NC_SLIRP}/dhcpv6.c"
  "${NC_SLIRP}/dnssearch.c" "${NC_SLIRP}/if.c" "${NC_SLIRP}/ip6_icmp.c" "${NC_SLIRP}/ip6_input.c"
  "${NC_SLIRP}/ip6_output.c" "${NC_SLIRP}/ip_icmp.c" "${NC_SLIRP}/ip_input.c" "${NC_SLIRP}/ip_output.c"
  "${NC_SLIRP}/mbuf.c" "${NC_SLIRP}/misc.c" "${NC_SLIRP}/ncsi.c" "${NC_SLIRP}/ndp_table.c"
  "${NC_SLIRP}/sbuf.c" "${NC_SLIRP}/slirp.c" "${NC_SLIRP}/socket.c" "${NC_SLIRP}/state.c"
  "${NC_SLIRP}/stream.c" "${NC_SLIRP}/tcp_input.c" "${NC_SLIRP}/tcp_output.c" "${NC_SLIRP}/tcp_subr.c"
  "${NC_SLIRP}/tcp_timer.c" "${NC_SLIRP}/tftp.c" "${NC_SLIRP}/udp.c" "${NC_SLIRP}/udp6.c"
  "${NC_SLIRP}/util.c" "${NC_SLIRP}/version.c" "${NC_SLIRP}/vmstate.c")
target_include_directories(nc_slirp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net/slirp" "${NC_REPO}")
target_include_directories(nc_slirp PUBLIC "${CMAKE_BINARY_DIR}/slirp")
target_link_libraries(nc_slirp PRIVATE nc_glib)
target_compile_options(nc_slirp PRIVATE -w)
if(NC_LINUX_X64 OR NC_LINUX_ARM64)
  target_compile_options(nc_slirp PRIVATE "SHELL:-include signal.h")   # misc.c's (Android's headers bring it)
endif()

if(NC_LINUX_X64)
  set(NC_PLATFORM_SOURCES core/vm.c core/fault_win.c core/jmp_x64.c cpu/cpu_x86.cpp cpu/x64emu.cpp
      compat/linux/seh.c compat/linux/linux.c)
  set(NC_CPU_LIB nc_zydis)
  set(NC_SYS_LIBS pthread dl m)
elseif(NC_LINUX_ARM64)
  # FEX as the CPU backend, desktop frontend (app_win.cmake): no SEH shim;
  # the core's faults (MMIO, dirty tracking, call-ret) go through a plain
  # SIGSEGV/SIGBUS handler (core/fault_linux_arm64.c) as on Android. linux.c
  # for the system-property shim and thread helpers NC_DESKTOP code expects.
  set(NC_PLATFORM_SOURCES core/jmp.c core/vm.c core/fault_linux_arm64.c cpu/cpu.cpp compat/linux/linux.c)
  set(NC_CPU_LIB nc_fex)
  set(NC_SYS_LIBS pthread dl m)
elseif(WIN32)
  set(NC_PLATFORM_SOURCES core/vm.c core/vm_win.cpp core/fault_win.c core/jmp_x64.c cpu/cpu_x86.cpp cpu/x64emu.cpp compat/win/win.c)
  set(NC_CPU_LIB nc_zydis)
  set(NC_SYS_LIBS ws2_32 iphlpapi winmm bcrypt dwmapi synchronization pthread)
else()
  set(NC_PLATFORM_SOURCES core/jmp.c core/vm.c cpu/cpu.cpp)
  set(NC_CPU_LIB nc_fex)
  set(NC_SYS_LIBS log)
endif()

add_library(hakux_native STATIC
  ${NC_PLATFORM_SOURCES}
  core/log.c
  core/xdvdfs.c
  core/xbe_load.c
  core/mods.c
  core/options.c
  core/cheats.c
  core/memscan.c
  core/core.c
  core/mmio.c
  core/irq.c
  dev/nv2a.c
  dev/ports.c
  dev/usb.c
  dev/usb_hub.c
  dev/xid.c
  dev/ohci.c
  dev/aci.c
  dev/nvnet.c
  net/net.c
  net/lan.c
  net/kai.c
  net/nat.c
  kernel/kernel.c
  kernel/pool.c
  kernel/ob.c
  kernel/ps.c
  kernel/ke.c
  kernel/mm.c
  kernel/paging.c
  kernel/ex.c
  kernel/rtl.c
  kernel/seh.c
  kernel/fixups.c
  kernel/x86mmio.c
  kernel/timer.c
  kernel/intr.c
  kernel/apc.c
  kernel/io.c
  kernel/xc.c
  kernel/xc_crypto.c
  kernel/kstate.c
  state/state.c
  state/vm_state.c
  state/save.c
  state/cache.c
  "${NC_REPO}/hw/xbox/hle/core/xbe.c"
)
target_include_directories(hakux_native PUBLIC "${NC_REPO}")
# xxhash (single header, FEX's copy): the savestates' blob store (state/cache.c)
target_include_directories(hakux_native PRIVATE "${NC_XXHASH_DIR}")
# The HLE layer's pure modules build without QEMU in this mode.
target_compile_definitions(hakux_native PRIVATE HLE_HOST_TEST)
target_link_libraries(hakux_native PUBLIC ${NC_CPU_LIB} hakux_hle hakux_apu nc_slirp ${NC_SYS_LIBS} z)
target_compile_options(hakux_native PRIVATE -Wall -Wno-unused-parameter)
target_sources(hakux_native PRIVATE core/chd_image.c)

# libchdr (NC_DEPS/libchdr, BSD-3, unmodified): CHD disc images
# (core/chd_image.c), with its own LZMA, Zstandard and FLAC decoders and the
# system's zlib. Without it, CHD files aren't read.
if(EXISTS "${NC_DEPS}/libchdr/CMakeLists.txt")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(WITH_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)
  set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
  set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${NC_DEPS}/libchdr" "${CMAKE_BINARY_DIR}/libchdr" EXCLUDE_FROM_ALL)
  target_compile_options(chdr-static PRIVATE -w)
  target_link_libraries(hakux_native PRIVATE chdr-static)
  target_compile_definitions(hakux_native PRIVATE NC_HAVE_CHD)
endif()

# xboxrun's custom Vulkan driver ("vkdriver="): the app's code for it
# (native/app/vkdriver.c, vklayer.c), through adrenotools when NC_DEPS has it
# (cmake/app.cmake).
if(WIN32 OR NC_LINUX_X64 OR NC_LINUX_ARM64)
  add_executable(xboxrun EXCLUDE_FROM_ALL run/xboxrun.c run/capture.c)
  target_link_libraries(xboxrun PRIVATE hakux_native hakux_hle z)
else()
  add_executable(xboxrun run/xboxrun.c run/capture.c app/vkdriver.c app/vklayer.c)
  target_link_libraries(xboxrun PRIVATE hakux_native hakux_hle z log)
endif()
target_link_libraries(hakux_hle PUBLIC hakux_native)
target_link_libraries(nc_slirp PRIVATE hakux_native)   # nc_log

# The hakuX app's native core library (native/app, cmake/app.cmake); on
# Windows and Linux the hakuX executable (native/app/win_main.c, cmake/app_win.cmake).
if(WIN32 OR NC_LINUX_X64 OR NC_LINUX_ARM64)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app_win.cmake")
else()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.cmake")
endif()

if(WIN32)
  nc_win_compat(hakux_native hakux_hle hakux_apu nc_glib nc_slirp xboxrun)
  if(TARGET hakux)
    nc_win_compat(hakux)
  endif()
elseif(NC_LINUX_X64 OR NC_LINUX_ARM64)
  nc_linux_compat(hakux_native hakux_hle hakux_apu nc_glib nc_slirp xboxrun hakux)
endif()
