cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)                        # runtime dependencies matched on normalized paths
endif()
project(LaplacePostgres VERSION 1.46.0 LANGUAGES C CXX ASM)
include(cmake/LaplacePgExecutor.cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Laplace-postgres: built with CMake against pg_config's paths, with Laplace-Native built as part of it and linked in.
if(DEFINED ENV{LAPLACE_NATIVE} AND NOT "$ENV{LAPLACE_NATIVE}" STREQUAL "")
  set(LAPLACE_NATIVE "$ENV{LAPLACE_NATIVE}" CACHE PATH "Laplace-Native sources")
else()
  set(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native" CACHE PATH "Laplace-Native sources")
endif()
if(NOT TARGET laplace_static)
  add_subdirectory(${LAPLACE_NATIVE} native EXCLUDE_FROM_ALL)
endif()
include(${LAPLACE_NATIVE}/cmake/LaplacePaths.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceFlags.cmake)

if(LAPLACE_PG_CONFIG)
  set(PG_CONFIG "${LAPLACE_PG_CONFIG}")
else()
  set(PG_CONFIG "${LAPLACE_PG_DIR}/bin/pg_config" CACHE FILEPATH "pg_config of the target server")
endif()
foreach(v includedir-server pkglibdir sharedir libdir bindir)
  execute_process(COMMAND ${PG_CONFIG} --${v} OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "-" "_" name ${v})
  set(PG_${name} ${out})
endforeach()

# An embedding product installs this same module and SQL into its own declared
# PostgreSQL package. Standalone installation retains pg_config's destinations.
if(NOT DEFINED LAPLACE_PG_INSTALL_LIBDIR)
  set(LAPLACE_PG_INSTALL_LIBDIR "${PG_pkglibdir}")
endif()
if(NOT DEFINED LAPLACE_PG_INSTALL_SHAREDIR)
  set(LAPLACE_PG_INSTALL_SHAREDIR "${PG_sharedir}/extension")
endif()
if(NOT DEFINED LAPLACE_PG_INSTALL_BINDIR)
  set(LAPLACE_PG_INSTALL_BINDIR "${PG_bindir}")
endif()
if(NOT DEFINED LAPLACE_PG_MODULE_PATHNAME)
  set(LAPLACE_PG_MODULE_PATHNAME "$libdir/laplace")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/manifest/function_bindings.tsv")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/manifest/function_bindings.tsv" _binding_rows)
set(_bindings "")
foreach(_row IN LISTS _binding_rows)
  if(_row MATCHES "^([A-Za-z_][A-Za-z_0-9]*)[\t ]+(true|false)$")
    string(APPEND _bindings "{\"${CMAKE_MATCH_1}\",${CMAKE_MATCH_2}},\n")
  elseif(NOT _row MATCHES "^#|^[\t ]*$")
    message(FATAL_ERROR "Invalid Native function binding: ${_row}")
  endif()
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/laplace_function_bindings.h" CONTENT "${_bindings}")
add_library(laplace_pg MODULE src/laplace_pg.c src/gist4d.c src/projection.c src/provenance.c src/enrichment.c src/walk.c src/sequences.c src/function_bindings.c src/binding.c)
target_compile_definitions(laplace_pg PRIVATE LP_PG_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} LP_PG_VERSION_MINOR=${PROJECT_VERSION_MINOR})
target_include_directories(laplace_pg PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(laplace_pg PROPERTIES OUTPUT_NAME laplace PREFIX "" C_VISIBILITY_PRESET hidden
                                            BUILD_RPATH "${LAPLACE_ICU_DIR}/lib" INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib")
target_link_libraries(laplace_pg PRIVATE laplace_text laplace_static BLAKE3::blake3 laplace_pg_executor)
if(WIN32)
  # PostgreSQL's Windows port headers come first, as in its own build; a module takes the server's symbols from the
  # import library postgres.lib, and exports only what PG_FUNCTION_INFO_V1 and PG_MODULE_MAGIC mark PGDLLEXPORT.
  target_include_directories(laplace_pg SYSTEM PRIVATE ${PG_includedir_server}/port/win32_msvc ${PG_includedir_server}/port/win32
                                                ${PG_includedir_server})
  target_compile_definitions(laplace_pg PRIVATE WIN32)
  target_compile_options(laplace_pg PRIVATE ${LP_C_FLAGS} -Wno-unused-function /clang:-fwrapv /clang:-fno-strict-aliasing)
  target_link_libraries(laplace_pg PRIVATE "${PG_libdir}/postgres.lib")
else()
  target_include_directories(laplace_pg SYSTEM PRIVATE ${PG_includedir_server})
  target_compile_options(laplace_pg PRIVATE ${LP_C_FLAGS} -Wno-unused-function -fwrapv -fno-strict-aliasing)   # PostgreSQL requires wrapping arithmetic
  target_link_libraries(laplace_pg PRIVATE m)
  target_link_options(laplace_pg PRIVATE -Wl,--exclude-libs,ALL)
endif()

# One current object definition, shared by cold install and the actual
# populated-development transition. Never replay unreleased migration history.
set(_sql_root "${CMAKE_CURRENT_SOURCE_DIR}/sql")
set(_sql_output "${CMAKE_CURRENT_BINARY_DIR}/extension")
file(MAKE_DIRECTORY "${_sql_output}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/laplace.control" _control)
string(REGEX REPLACE "module_pathname = '[^']*'" "module_pathname = '${LAPLACE_PG_MODULE_PATHNAME}'" _control "${_control}")
file(WRITE "${_sql_output}/laplace.control" "${_control}")
file(READ "${_sql_root}/operations.sql" _operations)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_root}/operations.sql")
file(READ "${_sql_root}/path_set_match.sql" _path_set_match)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_root}/path_set_match.sql")
set(LAPLACE_EXTENSION_SQL "")
foreach(_version "1.46" "1.38--1.46" "1.39--1.46" "1.40--1.46" "1.41--1.46" "1.42--1.46" "1.43--1.46" "1.44--1.46" "1.45--1.46")
  if(_version STREQUAL "1.46")
    set(_input "${_sql_root}/laplace.sql.in")
  else()
    set(_input "${_sql_root}/laplace_update.sql.in")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input}")
  file(READ "${_input}" _sql)
  string(REPLACE "-- @laplace_path_set_match@" "${_path_set_match}" _sql "${_sql}")
  string(REPLACE "-- @laplace_operations@" "${_operations}" _sql "${_sql}")
  set(_output "${_sql_output}/laplace--${_version}.sql")
  file(WRITE "${_output}" "${_sql}")
  list(APPEND LAPLACE_EXTENSION_SQL "${_output}")
endforeach()
file(GLOB _previous_sql "${_sql_output}/laplace--*.sql")
foreach(_previous IN LISTS _previous_sql)
  if(NOT _previous IN_LIST LAPLACE_EXTENSION_SQL)
    file(REMOVE "${_previous}")
  endif()
endforeach()
if(WIN32)
  # LoadLibrary does not search PATH: the DLLs laplace.dll loads (ICU's, and libmmd from the compiler) go next to
  # postgres.exe, as PostGIS's do. The list comes from the built module's imports at install time; the system's and
  # the UCRT's stay where they are.
  # oneAPI's bin holds symlinks into compiler/<version>/bin; the real directory, so the DLL itself is copied.
  file(REAL_PATH "${CMAKE_C_COMPILER}" LP_COMPILER)
  get_filename_component(LP_COMPILER_BIN "${LP_COMPILER}" DIRECTORY)
  install(TARGETS laplace_pg
          RUNTIME_DEPENDENCIES DIRECTORIES "${LAPLACE_ICU_DIR}/bin64" "${LAPLACE_ICU_DIR}/bin" "${LP_COMPILER_BIN}"
                               PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" POST_EXCLUDE_REGEXES "[Ss]ystem32"
          LIBRARY DESTINATION ${LAPLACE_PG_INSTALL_LIBDIR} RUNTIME DESTINATION ${LAPLACE_PG_INSTALL_BINDIR})
else()
  install(TARGETS laplace_pg LIBRARY DESTINATION ${LAPLACE_PG_INSTALL_LIBDIR})
endif()
install(FILES "${_sql_output}/laplace.control" ${LAPLACE_EXTENSION_SQL} DESTINATION ${LAPLACE_PG_INSTALL_SHAREDIR})

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  find_program(LAPLACE_PG_REGRESS NAMES pg_regress
               HINTS "${LAPLACE_PG_DIR}/lib/pgxs/src/test/regress" "${LAPLACE_PG_DIR}/bin")
  if(NOT LAPLACE_PG_REGRESS)
    message(FATAL_ERROR "PostgreSQL regression driver not found under LAPLACE_PG_DIR")
  endif()
  add_test(NAME postgres_regress COMMAND ${CMAKE_COMMAND}
    "-DPG_REGRESS=${LAPLACE_PG_REGRESS}" "-DPG_BINDIR=${PG_bindir}"
    "-DMODULE=$<TARGET_FILE:laplace_pg>" "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}" "-DTIER0=${LAPLACE_TIER0}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-regress.cmake")
  set_tests_properties(postgres_regress PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  if(WIN32)
    find_program(LAPLACE_REGRESS_DIFF NAMES diff HINTS "C:/Program Files/Git/usr/bin" REQUIRED)
    get_filename_component(_regress_diff_directory "${LAPLACE_REGRESS_DIFF}" DIRECTORY)
    set_tests_properties(postgres_regress PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_regress_diff_directory}")
  endif()
endif()
