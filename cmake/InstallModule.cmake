# Same path rules as install(TARGETS), including an embedding product's relative
# libdir, cmake --install --prefix, and Linux DESTDIR packaging.
file(READ "${MODULE_INDEX}" name)
string(STRIP "${name}" name)
if(NOT name MATCHES "^laplace_[0-9a-f]+$")
  message(FATAL_ERROR "Invalid Native module generation: ${name}")
endif()
set(destination "${MODULE_LIBDIR}")
if(NOT IS_ABSOLUTE "${destination}")
  set(destination "${CMAKE_INSTALL_PREFIX}/${destination}")
endif()
file(INSTALL "${MODULE_BUILD}/${name}${MODULE_SUFFIX}"
  DESTINATION "${destination}" TYPE FILE)
