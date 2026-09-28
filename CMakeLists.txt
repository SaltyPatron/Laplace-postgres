cmake_minimum_required(VERSION 3.24)
project(LaplacePostgres VERSION 0.1.0 LANGUAGES C CXX ASM)

# Laplace-postgres: built with CMake against pg_config's paths, with Laplace-Native linked in statically.
set(PG_CONFIG "/usr/local/pgsql/bin/pg_config" CACHE FILEPATH "pg_config of the target server")
set(LAPLACE_NATIVE_DIR "/repos/src/Laplace-Native" CACHE PATH "Laplace-Native sources")
foreach(v includedir-server pkglibdir sharedir)
  execute_process(COMMAND ${PG_CONFIG} --${v} OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "-" "_" name ${v})
  set(PG_${name} ${out})
endforeach()

add_subdirectory(${LAPLACE_NATIVE_DIR} native EXCLUDE_FROM_ALL)

add_library(laplace_pg MODULE src/laplace_pg.c)
set_target_properties(laplace_pg PROPERTIES OUTPUT_NAME laplace PREFIX "" C_VISIBILITY_PRESET hidden)
target_include_directories(laplace_pg PRIVATE ${PG_includedir_server})
target_compile_options(laplace_pg PRIVATE ${LP_C_FLAGS} -fwrapv -fno-strict-aliasing)   # PostgreSQL requires wrapping arithmetic
target_link_libraries(laplace_pg PRIVATE laplace_static BLAKE3::blake3 m)
target_link_options(laplace_pg PRIVATE -Wl,--exclude-libs,ALL)

install(TARGETS laplace_pg LIBRARY DESTINATION ${PG_pkglibdir})
install(FILES laplace.control sql/laplace--0.1.sql DESTINATION ${PG_sharedir}/extension)
