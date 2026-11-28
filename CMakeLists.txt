cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)                        # runtime dependencies matched on normalized paths
endif()
project(LaplacePostgres VERSION 1.0.0 LANGUAGES C CXX ASM)
# Some configured ASM toolchains leave the global format Unknown despite the
# C compiler's verified ELF ABI. Ninja needs the actual format for RPATH edits.
if(CMAKE_EXECUTABLE_FORMAT STREQUAL "Unknown" AND CMAKE_C_COMPILER_ABI STREQUAL "ELF")
  set(CMAKE_EXECUTABLE_FORMAT ELF)
endif()
include(cmake/LaplacePgExecutor.cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Laplace-postgres: built with CMake against pg_config's paths, linked with the installed Laplace-Native package
# (CMAKE_PREFIX_PATH or LaplaceNative_DIR; a build that includes Native provides the same package). The package also
# gives the floating-point contract Native was built under (LP_C_FLAGS) and the laplace_path() locations.
find_package(LaplaceNative CONFIG REQUIRED)

if(LAPLACE_PG_CONFIG)
  set(PG_CONFIG "${LAPLACE_PG_CONFIG}")
elseif(NOT DEFINED PG_CONFIG)
  set(PG_CONFIG "${LAPLACE_PG_DIR}/bin/pg_config")
endif()
foreach(v includedir-server pkglibdir sharedir libdir bindir)
  execute_process(COMMAND "${PG_CONFIG}" --${v} OUTPUT_VARIABLE out
                  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
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
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/laplace_function_bindings.h" CONTENT "static const FunctionBinding bindings[]={\n${_bindings}};\n")
add_library(laplace_pg MODULE
  src/laplace_pg.c
  src/realize.c
  src/gin_path.c
  src/gin_mask.c
  src/couple.c
  src/gist4d.c
  src/projection.c
  src/provenance.c
  src/attribution.c
  src/frontier_index.c
  src/frontier_gin.c
  src/enrichment.c
  src/walk.c
  src/sequences.c
  src/read_ops.c
  src/lexical_ops.c
  src/function_bindings.c
  src/binding.c
)
target_compile_definitions(laplace_pg PRIVATE LP_PG_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} LP_PG_VERSION_MINOR=${PROJECT_VERSION_MINOR})
target_include_directories(laplace_pg PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(laplace_pg PROPERTIES OUTPUT_NAME laplace PREFIX "" C_VISIBILITY_PRESET hidden
                                            BUILD_RPATH "${LAPLACE_ICU_DIR}/lib" INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib")
target_link_libraries(laplace_pg PRIVATE Laplace::text Laplace::laplace_static BLAKE3::blake3 laplace_pg_executor)
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
# PostgreSQL caches loaded libraries for the life of each backend. A new module
# therefore needs a new filename, including when an unreleased version is rebuilt.
# Hash the actual linked bytes; the stable laplace alias remains for older installs
# and private-package tooling. The generated control binds new functions to the hash.
set(_module_package "${CMAKE_CURRENT_BINARY_DIR}/package-laplace-module.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/laplace.control" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageModule.cmake")
file(READ "${_sql_root}/operations.sql" _operations)
file(READ "${_sql_root}/api.sql" _api)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_root}/api.sql")
# The same declarations repair missing extension APIs without replaying table DDL.
string(REPLACE "@extschema:postgis@" "@postgis@" _api_c "${_api}")
string(REPLACE "\\" "\\\\" _api_c "${_api_c}")
string(REPLACE "\"" "\\\"" _api_c "${_api_c}")
string(REPLACE "\r" "" _api_c "${_api_c}")
string(REPLACE "\n" "\\n\"\n\"" _api_c "${_api_c}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/laplace_current_api.h" CONTENT "static const char current_api[]=\n\"${_api_c}\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_root}/operations.sql")
file(READ "${_sql_root}/path_set_match.sql" _path_set_match)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sql_root}/path_set_match.sql")
# Storage layout versioning: hash only the storage type and persistent partitioned table DDL
# (not function signatures or operators) so adding or fixing SQL/C functions never forces a
# storage-incompatible database recreate.
set(_input "${_sql_root}/laplace.sql.in")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input}")
file(READ "${_input}" _storage_template)
string(FIND "${_storage_template}" "-- Canonical retained content;" _storage_tbl_begin)
string(FIND "${_storage_template}" "-- @laplace_api@" _storage_tbl_end)
if(_storage_tbl_begin LESS 0 OR _storage_tbl_end LESS _storage_tbl_begin)
  message(FATAL_ERROR "Unable to locate canonical storage DDL section in ${_input}")
endif()
math(EXPR _storage_tbl_len "${_storage_tbl_end} - ${_storage_tbl_begin}")
string(SUBSTRING "${_storage_template}" ${_storage_tbl_begin} ${_storage_tbl_len} _storage_ddl)
string(REGEX MATCH "CREATE TYPE @extschema@\\.blake3 \\([^;]*\\);" _storage_b3_type "${_storage_template}")
string(SHA256 _storage_version "${_storage_b3_type}\n${_storage_ddl}")
string(SUBSTRING "${_storage_version}" 0 16 _storage_version)
set(_sql "${_storage_template}")
string(REPLACE "-- @laplace_path_set_match@" "${_path_set_match}" _sql "${_sql}")
string(REPLACE "-- @laplace_operations@" "${_operations}" _sql "${_sql}")
string(REPLACE "-- @laplace_api@" "${_api}" _sql "${_sql}")
string(APPEND _sql "\nCOMMENT ON TABLE @extschema@.entity IS 'laplace-storage:${_storage_version}';\n")
string(SHA256 _current_version "${_sql}")
string(SUBSTRING "${_current_version}" 0 16 _current_version)
set(_reconcile_sql
  "-- Reconcile replaceable Laplace objects without touching storage tables.\n${_path_set_match}\n${_api}\n${_operations}\nDO $stamp$\nDECLARE ext_ns text;\nBEGIN\n  SELECT n.nspname INTO ext_ns FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = 'laplace';\n  IF ext_ns IS NOT NULL AND (SELECT p.partstrat::text || ':' || (SELECT count(*) FROM pg_catalog.pg_inherits i WHERE i.inhparent = c.oid) FROM pg_catalog.pg_class c JOIN pg_catalog.pg_partitioned_table p ON p.partrelid = c.oid JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = ext_ns AND c.relname = 'entity') = 'r:9' THEN\n    EXECUTE pg_catalog.format('COMMENT ON TABLE %I.entity IS %L', ext_ns, 'laplace-storage:${_storage_version}');\n  END IF;\nEND $stamp$;\nSELECT @extschema@.laplace_restore_function_bindings();\n")
string(CONCAT _verify_storage_sql
  "SELECT CASE\n"
  "  WHEN ext.ns IS NULL THEN 'missing'\n"
  "  WHEN pg_catalog.obj_description(pg_catalog.to_regclass(ext.ns || '.entity'), 'pg_class') = 'laplace-storage:${_storage_version}' THEN 'ok'\n"
  "  WHEN pg_catalog.to_regclass(ext.ns || '.entity') IS NOT NULL\n"
  "    AND (pg_catalog.obj_description(pg_catalog.to_regclass(ext.ns || '.entity'), 'pg_class') IS NULL\n"
  "         OR pg_catalog.obj_description(pg_catalog.to_regclass(ext.ns || '.entity'), 'pg_class') LIKE 'laplace-storage:%')\n"
  "    AND EXISTS (SELECT 1 FROM pg_catalog.pg_type t WHERE t.typnamespace = ext.nsoid AND t.typname = 'blake3' AND t.typlen = 16)\n"
  "    AND (SELECT p.partstrat::text || ':' || (SELECT count(*) FROM pg_catalog.pg_inherits i WHERE i.inhparent = c.oid)\n"
  "         FROM pg_catalog.pg_class c JOIN pg_catalog.pg_partitioned_table p ON p.partrelid = c.oid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'entity') = 'r:9'\n"
  "    AND (SELECT pg_catalog.string_agg(a.attname, ',' ORDER BY a.attnum)\n"
  "         FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid = a.attrelid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'entity' AND a.attnum > 0 AND NOT a.attisdropped) = 'id,tier,coord,hilbert,mask'\n"
  "    AND (SELECT p.partstrat::text || ':' || (SELECT count(*) FROM pg_catalog.pg_inherits i WHERE i.inhparent = c.oid)\n"
  "         FROM pg_catalog.pg_class c JOIN pg_catalog.pg_partitioned_table p ON p.partrelid = c.oid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'physicality') = 'r:9'\n"
  "    AND (SELECT pg_catalog.string_agg(a.attname, ',' ORDER BY a.attnum)\n"
  "         FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid = a.attrelid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'physicality' AND a.attnum > 0 AND NOT a.attisdropped) = 'entity,tier,hilbert,path,mask'\n"
  "    AND (SELECT p.partstrat::text || ':' || (SELECT count(*) FROM pg_catalog.pg_inherits i WHERE i.inhparent = c.oid)\n"
  "         FROM pg_catalog.pg_class c JOIN pg_catalog.pg_partitioned_table p ON p.partrelid = c.oid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'attestation') = 'r:16'\n"
  "    AND (SELECT pg_catalog.string_agg(a.attname, ',' ORDER BY a.attnum)\n"
  "         FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid = a.attrelid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'attestation' AND a.attnum > 0 AND NOT a.attisdropped) = 'claim,witness,lineage,games,tokens,score,position,trust'\n"
  "    AND (SELECT p.partstrat::text || ':' || (SELECT count(*) FROM pg_catalog.pg_inherits i WHERE i.inhparent = c.oid)\n"
  "         FROM pg_catalog.pg_class c JOIN pg_catalog.pg_partitioned_table p ON p.partrelid = c.oid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'consensus') = 'r:16'\n"
  "    AND (SELECT pg_catalog.string_agg(a.attname, ',' ORDER BY a.attnum)\n"
  "         FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON c.oid = a.attrelid\n"
  "         WHERE c.relnamespace = ext.nsoid AND c.relname = 'consensus' AND a.attnum > 0 AND NOT a.attisdropped) = 'claim,rating,deviation,volatility,matches'\n"
  "  THEN 'ok'\n"
  "  ELSE 'incompatible'\n"
  "END\n"
  "FROM (VALUES (1)) AS _v(dummy)\n"
  "LEFT JOIN LATERAL (\n"
  "  SELECT e.extnamespace AS nsoid, n.nspname AS ns\n"
  "  FROM pg_catalog.pg_extension e\n"
  "  JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace\n"
  "  WHERE e.extname = 'laplace'\n"
  ") ext ON true;\n")
file(GENERATE OUTPUT "${_module_package}" CONTENT
  "set(MODULE [==[$<TARGET_FILE:laplace_pg>]==])\nset(CONTROL [==[${CMAKE_CURRENT_SOURCE_DIR}/laplace.control]==])\nset(VERSION [==[${_current_version}]==])\nset(OUTPUT [==[${_sql_output}]==])\nset(OVERRIDE [==[${LAPLACE_PG_MODULE_PATHNAME}]==])\ninclude([==[${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageModule.cmake]==])\n")
add_custom_command(TARGET laplace_pg POST_BUILD COMMAND ${CMAKE_COMMAND} -P "${_module_package}" VERBATIM)
function(_laplace_write_if_changed path content)
  set(_existing "")
  if(EXISTS "${path}")
    file(READ "${path}" _existing)
  endif()
  if(NOT EXISTS "${path}" OR NOT "${_existing}" STREQUAL "${content}")
    file(WRITE "${path}" "${content}")
  endif()
endfunction()
set(LAPLACE_EXTENSION_SQL "${_sql_output}/laplace--${_current_version}.sql")
set(LAPLACE_RECONCILE_SQL "${_sql_output}/laplace.reconcile.sql")
set(LAPLACE_VERIFY_STORAGE_SQL "${_sql_output}/laplace.verify-storage.sql")
_laplace_write_if_changed("${LAPLACE_EXTENSION_SQL}" "${_sql}")
_laplace_write_if_changed("${LAPLACE_RECONCILE_SQL}" "${_reconcile_sql}")
_laplace_write_if_changed("${LAPLACE_VERIFY_STORAGE_SQL}" "${_verify_storage_sql}")
if(EXISTS "${_sql_output}/laplace.control")
  file(READ "${_sql_output}/laplace.control" _existing_control)
  string(REGEX REPLACE "default_version = '[^']*'" "default_version = '${_current_version}'" _updated_control "${_existing_control}")
  _laplace_write_if_changed("${_sql_output}/laplace.control" "${_updated_control}")
endif()
file(GLOB _previous_sql "${_sql_output}/laplace--*.sql")
foreach(_previous IN LISTS _previous_sql)
  if(NOT _previous IN_LIST LAPLACE_EXTENSION_SQL)
    file(REMOVE "${_previous}")
  endif()
endforeach()
install(SCRIPT "${_module_package}")
install(CODE "set(MODULE_INDEX [==[${_sql_output}/laplace_module.txt]==])\nset(MODULE_BUILD [==[${CMAKE_CURRENT_BINARY_DIR}]==])\nset(MODULE_SUFFIX [==[${CMAKE_SHARED_MODULE_SUFFIX}]==])\nset(MODULE_LIBDIR [==[${LAPLACE_PG_INSTALL_LIBDIR}]==])\ninclude([==[${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallModule.cmake]==])\n")
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
install(FILES "${_sql_output}/laplace.control" ${LAPLACE_EXTENSION_SQL} "${LAPLACE_RECONCILE_SQL}" "${LAPLACE_VERIFY_STORAGE_SQL}" DESTINATION ${LAPLACE_PG_INSTALL_SHAREDIR})

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_test(NAME module_install COMMAND ${CMAKE_COMMAND}
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/module-install.cmake")
  add_test(NAME configure_incremental COMMAND ${CMAKE_COMMAND}
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/configure-incremental.cmake")
  set_tests_properties(configure_incremental PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
  # Database regressions run against the deployed extension: tests/deployed.sh.
endif()
