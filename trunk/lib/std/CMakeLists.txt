# CMake build file for lib/std

set(CMAKE_VERBOSE_MAKEFILE ON)

file(GLOB TART_STDLIB_SRC
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    tart/annex/*.tart
    tart/atomic/*.tart
    tart/core/*.tart
    tart/collections/*.tart
    tart/config/*.tart
    tart/concurrent/ThreadLocal.tart
    tart/gc/*.tart
    tart/gc/heap/*.tart
#    tart/inject/*.tart
#    tart/inject/binder/*.tart
#    tart/inject/impl/*.tart
    tart/io/*.tart
    tart/reflect/*.tart
    tart/text/*.tart
    tart/text/encodings/*.tart
    tart/time/*.tart)

set(SRCDIR ${CMAKE_CURRENT_SOURCE_DIR}) # Source file root
set(MODPATH ${TART_SOURCE_DIR}/lib/std)  # Module search path
set(TART_OPTIONS
    -g
    -debug-errors
    -nostdlib
)

# Empty list of output files
set(STDLIB_BC_FILES)

include(${CMAKE_CURRENT_BINARY_DIR}/libstd.deps OPTIONAL)

# Create a command for each source file
foreach(SRC_FILE ${TART_STDLIB_SRC})
  string(REGEX REPLACE ".tart\$" ".bc" OUT_FILE "${SRC_FILE}")
  
  # Generate the deps variable name
  string(REGEX REPLACE ".tart\$" "" DEPS_NAME "${SRC_FILE}")
  string(TOUPPER "${DEPS_NAME}" DEPS_NAME)
  string(REGEX REPLACE "[^a-zA-Z0-9]" "_" DEPS_NAME "${DEPS_NAME}")

  add_custom_command(OUTPUT ${OUT_FILE}
      COMMAND tartc ${TART_OPTIONS} -sourcepath ${SRCDIR} -i${MODPATH} ${SRC_FILE}
      DEPENDS "${SRC_FILE}" tartc ${${DEPS_NAME}_DEPS}
      COMMENT "Compiling Tart source file ${SRC_FILE}")

  # Remember the list of output files.
  set(STDLIB_BC_FILES ${STDLIB_BC_FILES} "${OUT_FILE}")
endforeach(SRC_FILE)

# Generate library and dependencies
add_custom_command(
    OUTPUT libstd.bc libstd.deps
    COMMAND ${LLVM_LD} -disable-opt -link-as-library -o libstd.bc ${STDLIB_BC_FILES}
    COMMAND gendeps -o libstd.deps ${STDLIB_BC_FILES}
#    COMMAND tartln -filetype=bc -link-as-library -o libstd.bc ${STDLIB_BC_FILES}
    DEPENDS ${STDLIB_BC_FILES} gendeps
    COMMENT "Linking libstd.bc")
    
add_custom_target(libstd DEPENDS libstd.bc libstd.deps)

# Extract doc comments
add_custom_command(
  OUTPUT libstd.xml
  COMMAND docexport -o libstd.xml  -sourcepath ${SRCDIR} -i${MODPATH} ${TART_STDLIB_SRC}
  DEPENDS ${TART_STDLIB_SRC} docexport
  COMMENT "Extracting doc comments for libstd.xml")

add_custom_target(libstd.doc DEPENDS libstd.xml)
