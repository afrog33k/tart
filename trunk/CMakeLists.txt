# CMake build file for Tart

cmake_minimum_required(VERSION 2.8.4)
project(TART)

set(TART_VERSION 0)
set(TART_MAJOR_REVISION 1)
set(TART_MINOR_REVISION 0)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules/")
set(PROFILE_TARTC 0)
set(PROFILE_TARTLN 0)
set(USE_CLANG 1 CACHE BOOL "Whether to use clang to compile C++ files.")

# Macros we'll need
include(CheckIncludeFile)
include(CheckIncludeFileCXX)
include(CheckFunctionExists)
include(CheckCXXSourceCompiles)
include(CheckTypeSize)
include(FindThreads)

# Check for the existence of include files.
check_include_file(assert.h HAVE_ASSERT_H)
check_include_file(stdio.h HAVE_STDIO_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(stddef.h HAVE_STDDEF_H)
check_include_file(stdlib.h HAVE_STDLIB_H)
check_include_file(string.h HAVE_STRING_H)
check_include_file(stdbool.h HAVE_STDBOOL_H)
check_include_file(stdarg.h HAVE_STDARG_H)
check_include_file(malloc.h HAVE_MALLOC_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(fcntl.h HAVE_FCNTL_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(errno.h HAVE_ERRNO_H)
check_include_file(execinfo.h HAVE_EXECINFO_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/time.h HAVE_SYS_TIME_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(sys/resource.h HAVE_SYS_RESOURCE_H)
check_include_file(libkern/OSAtomic.h HAVE_LIBKERN_OSATOMIC_H)
check_include_file_cxx(new HAVE_NEW)
check_include_file_cxx(ctime HAVE_CTIME)
check_include_file_cxx(cxxabi.h HAVE_CXXABI_H)
check_include_file_cxx(dlfcn.h HAVE_DLFCN_H)
check_include_file_cxx(unwind.h HAVE_UNWIND_H)
check_include_file_cxx(libunwind.h HAVE_LIBUNWIND_H)

if (CMAKE_USE_PTHREADS_INIT)
    set (HAVE_PTHREADS ON)
endif (CMAKE_USE_PTHREADS_INIT)

# Check for library functions we need.
set(CMAKE_REQUIRED_DEFINITIONS -D__USE_GNU)
check_function_exists(backtrace HAVE_BACKTRACE)
check_function_exists(dladdr HAVE_DLADDR)
check_function_exists(posix_memalign HAVE_POSIX_MEMALIGN)
check_function_exists(valloc HAVE_VALLOC)
check_function_exists(_aligned_malloc HAVE_ALIGNED_MALLOC)
check_function_exists(stat HAVE_STAT)

# Check for LLVM
find_package(LLVM REQUIRED)
find_program(LLVM_LLC llc PATHS ${LLVM_BIN_DIR})
find_program(LLVM_LD llvm-ld PATHS ${LLVM_BIN_DIR})
find_program(LLVM_OPT opt PATHS ${LLVM_BIN_DIR})
find_program(LLVM_AS llvm-as PATHS ${LLVM_BIN_DIR})
find_program(CLANG clang PATHS ${LLVM_BIN_DIR} NO_DEFAULT_PATH)
find_program(PYTHON3 python3)

# Use clang if it's available
if (USE_CLANG AND CLANG)
  set(CMAKE_CXX_COMPILER ${CLANG})
  set(CMAKE_COMPILER_IS_CLANG true)
  unset(CMAKE_COMPILER_IS_GNUCXX)
endif (USE_CLANG AND CLANG)

# Find libraries
find_library(LIB_DL dl)

# Check for GCC atomics
check_cxx_source_compiles(
    "int main() {
        int f;
        __sync_fetch_and_add(&f, 1);
        __sync_fetch_and_sub(&f, 1);
        __sync_add_and_fetch(&f, 1);
        __sync_sub_and_fetch(&f, 1);
        __sync_bool_compare_and_swap(&f, 1, 0);
    }"
    HAVE_GCC_ATOMICS)

# Check for GCC thread local support
check_cxx_source_compiles(
    "__thread int i; int main() { return 0; }"
    HAVE_GCC_THREAD_LOCAL)

if (MSVC)
  check_cxx_source_compiles(
      "__declspec(thread) int i; int main() { return 0; }"
      HAVE_MSVC_THREAD_LOCAL)
endif (MSVC)

# Check for type sizes
check_type_size(int SIZEOF_INT)
check_type_size(long SIZEOF_LONG)
check_type_size(size_t SIZEOF_SIZE_T)
check_type_size("long long" SIZEOF_LONG_LONG)
check_type_size("void*" SIZEOF_VOID_PTR)

# Check for dsymutil
find_program(DSYMUTIL dsymutil)

# Directory paths for installed libraries
file(TO_NATIVE_PATH ${CMAKE_INSTALL_PREFIX}/lib/tart/static       TART_INSTALL_DIR_LIB_STATIC)
file(TO_NATIVE_PATH ${CMAKE_INSTALL_PREFIX}/lib/tart/plugin       TART_INSTALL_DIR_LIB_PLUGIN)
file(TO_NATIVE_PATH ${CMAKE_INSTALL_PREFIX}/lib/tart/bc/libstd.bc TART_INSTALL_DIR_LIB_STD_BC)

# Generate the config.h file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.h)

# Generate the runtime config.h file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/runtime_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/runtime/include/runtime_config.h)

# Generate the config_paths.h file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config_paths.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config_paths.h
    ESCAPE_QUOTES)

# Include directory paths
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${TART_SOURCE_DIR}/compiler/include)
include_directories(${TART_SOURCE_DIR}/linker/include)
include_directories(${LLVM_INCLUDE_DIR})

# Definitions needed by LLVM headers
add_definitions(
    -D__STDC_LIMIT_MACROS
    -D__STDC_CONSTANT_MACROS
    -D_GNU_SOURCE)

# Extra flags for GCC (C++ only)
if (CMAKE_COMPILER_IS_GNUCXX)
  add_definitions(
      -pipe
      -Wall -Wextra -Werror -Wcast-align -Wpointer-arith
      -Wno-deprecated -Wno-unused
      -fmessage-length=0
  )
endif (CMAKE_COMPILER_IS_GNUCXX)

if (CMAKE_COMPILER_IS_CLANG)
  add_definitions(
      -Wall
      -Wextra
      -Werror
      -Wcast-align
      -Wpointer-arith
      -Wno-deprecated
      -Wno-unused-parameter
      -fno-rtti
      -fno-exceptions
      -fPIC
  )
  set(CMAKE_EXE_LINKER_FLAGS -lstdc++)
endif (CMAKE_COMPILER_IS_CLANG)

# Extra flags for MSVC (C only)
if (MSVC)
  add_definitions(
      -D_CRT_SECURE_NO_WARNINGS
      -DUNICODE -D_UNICODE
      -wd4146 -wd4503 -wd4996 -wd4800 -wd4244 -wd4624
      -wd4355 -wd4715 -wd4180 -wd4345 -wd4224
  )
endif (MSVC)

# Enable use of assembly language sources
if (NOT MSVC)
  enable_language(ASM-ATT OPTIONAL)
endif (NOT MSVC)

# Add the check-build target
add_custom_target(check)

# Global definitions used by tests.
set(GC_PLUGIN "${PROJECT_BINARY_DIR}/linker/libgc${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(REFLECTOR_PLUGIN "${PROJECT_BINARY_DIR}/linker/libreflector${CMAKE_SHARED_LIBRARY_SUFFIX}")

# Subdirs to compile
add_subdirectory(compiler)
add_subdirectory(linker)
add_subdirectory(tools)
add_subdirectory(runtime)
add_subdirectory(third-party)
add_subdirectory(test/unit)
add_subdirectory(lib)
add_subdirectory(test/compile)
add_subdirectory(test/simple)
#add_subdirectory(test/minimal)
add_subdirectory(test/lang)
add_subdirectory(test/stdlib)
add_subdirectory(test/libopts)
add_subdirectory(doc/api)
