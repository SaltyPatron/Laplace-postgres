# Exercise the built candidate in a fresh, private PostgreSQL instance. The
# installed cluster, extension files, and application databases are untouched.
foreach(required PG_REGRESS PG_BINDIR MODULE SOURCE BUILD TIER0)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing regression setting ${required}")
  endif()
endforeach()
file(REAL_PATH "${BUILD}" build_root)
set(package "${build_root}/regress-package")
set(output "${build_root}/regress-output")
set(instance "${build_root}/regress-instance")
file(MAKE_DIRECTORY "${package}/extension" "${output}")
file(GLOB scripts "${SOURCE}/sql/laplace--*.sql")
file(COPY ${scripts} DESTINATION "${package}/extension")
file(READ "${SOURCE}/laplace.control" control)
file(TO_CMAKE_PATH "${MODULE}" module_path)
string(REGEX REPLACE "\\.(dll|so|dylib)$" "" module_path "${module_path}")
string(REPLACE "'" "''" module_path "${module_path}")
string(REGEX REPLACE "module_pathname = '[^']*'" "module_pathname = '${module_path}'" control "${control}")
file(WRITE "${package}/extension/laplace.control" "${control}")
if(WIN32)
  set(separator ";")
else()
  set(separator ":")
endif()
file(TO_CMAKE_PATH "${TIER0}" tier0)
string(REPLACE "'" "''" tier0 "${tier0}")
set(config "${build_root}/regress-postgresql.conf")
file(WRITE "${config}"
  "extension_control_path = '${package}${separator}$system'\n"
  "laplace.tier0 = '${tier0}'\n"
  "max_prepared_transactions = 0\n")
# pg_regress owns only this instance path, all of which is inside this build.
# Its temporary-instance mode stops the server and cleans its cluster on success.
if(WIN32)
  find_program(pwsh NAMES pwsh REQUIRED)
  execute_process(COMMAND "${pwsh}" -NoLogo -NoProfile -File "${SOURCE}/tests/run-regress.ps1"
    -PgRegress "${PG_REGRESS}" -PgBin "${PG_BINDIR}" -InputRoot "${SOURCE}/tests"
    -BuildRoot "${build_root}" -Config "${config}"
    WORKING_DIRECTORY "${build_root}" RESULT_VARIABLE result)
else()
execute_process(COMMAND "${PG_REGRESS}" "--bindir=${PG_BINDIR}"
  "--inputdir=${SOURCE}/tests" "--outputdir=${output}"
  "--temp-instance=${instance}" "--temp-config=${config}"
  --no-locale --encoding=UTF8 --load-extension=postgis --load-extension=laplace
  mask_upgrade mask geometry gin_path gist4d text_id attestation stage
  WORKING_DIRECTORY "${build_root}" RESULT_VARIABLE result)
endif()
if(NOT result EQUAL 0)
  foreach(log "${output}/regression.diffs" "${output}/log/initdb.log" "${output}/log/postmaster.log")
    if(EXISTS "${log}")
      file(READ "${log}" details)
      message("${log}:\n${details}")
    endif()
  endforeach()
  message(FATAL_ERROR "PostgreSQL regressions failed (${result})")
endif()
