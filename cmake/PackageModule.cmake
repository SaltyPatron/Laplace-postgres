# Package one linked module generation without replacing a loaded generation.
file(SHA256 "${MODULE}" digest)
string(SUBSTRING "${digest}" 0 20 digest)
set(name "laplace_${digest}")
get_filename_component(directory "${MODULE}" DIRECTORY)
get_filename_component(suffix "${MODULE}" LAST_EXT)
configure_file("${MODULE}" "${directory}/${name}${suffix}" COPYONLY)
file(READ "${CONTROL}" control)
if(OVERRIDE)
  # Consumers with a private dynamic_library_path select a module basename;
  # standalone PostgreSQL installations retain the usual $libdir location.
  string(REPLACE "@MODULE_NAME@" "${name}" path "${OVERRIDE}")
else()
  set(path "$libdir/${name}")
endif()
string(REGEX REPLACE "module_pathname = '[^']*'" "module_pathname = '${path}'" control "${control}")
file(WRITE "${OUTPUT}/laplace.control" "${control}")
file(WRITE "${OUTPUT}/laplace_module.txt" "${name}\n")
