cmake_minimum_required(VERSION 3.24)
project(LaplacePostgres VERSION 0.6.0 LANGUAGES C CXX ASM)

# Laplace-postgres: built with CMake against pg_config's paths, with Laplace-Native built as part of it and linked in.
if(DEFINED ENV{LAPLACE_NATIVE} AND NOT "$ENV{LAPLACE_NATIVE}" STREQUAL "")
  set(LAPLACE_NATIVE "$ENV{LAPLACE_NATIVE}" CACHE PATH "Laplace-Native sources")
else()
  set(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native" CACHE PATH "Laplace-Native sources")
endif()
add_subdirectory(${LAPLACE_NATIVE} native EXCLUDE_FROM_ALL)
include(${LAPLACE_NATIVE}/cmake/LaplacePaths.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceFlags.cmake)

set(PG_CONFIG "${LAPLACE_PG_DIR}/bin/pg_config" CACHE FILEPATH "pg_config of the target server")
foreach(v includedir-server pkglibdir sharedir)
  execute_process(COMMAND ${PG_CONFIG} --${v} OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "-" "_" name ${v})
  set(PG_${name} ${out})
endforeach()

add_library(laplace_pg MODULE src/laplace_pg.c)
set_target_properties(laplace_pg PROPERTIES OUTPUT_NAME laplace PREFIX "" C_VISIBILITY_PRESET hidden
                                            BUILD_RPATH "${LAPLACE_ICU_DIR}/lib" INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib")
target_include_directories(laplace_pg PRIVATE ${PG_includedir_server})
target_compile_options(laplace_pg PRIVATE ${LP_C_FLAGS} -Wno-unused-function -fwrapv -fno-strict-aliasing)   # PostgreSQL requires wrapping arithmetic
target_link_libraries(laplace_pg PRIVATE laplace_text laplace_static BLAKE3::blake3 m)
target_link_options(laplace_pg PRIVATE -Wl,--exclude-libs,ALL)

file(GLOB LAPLACE_EXTENSION_SQL sql/laplace--*.sql)
install(TARGETS laplace_pg LIBRARY DESTINATION ${PG_pkglibdir})
install(FILES laplace.control ${LAPLACE_EXTENSION_SQL} DESTINATION ${PG_sharedir}/extension)
