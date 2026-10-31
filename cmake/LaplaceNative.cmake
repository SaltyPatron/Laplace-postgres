# Resolve Native before loading its shared CMake helpers. Environment and sibling
# defaults must not become persistent cache overrides. Explicit cache and parent
# selections remain authoritative, including existing caches until cleared with -U.
if(NOT DEFINED LAPLACE_NATIVE)
  if(DEFINED ENV{LAPLACE_NATIVE} AND NOT "$ENV{LAPLACE_NATIVE}" STREQUAL "")
    set(LAPLACE_NATIVE "$ENV{LAPLACE_NATIVE}")
  else()
    set(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native")
  endif()
endif()
if(DEFINED CACHE{LAPLACE_NATIVE})
  set(LAPLACE_NATIVE "${LAPLACE_NATIVE}" CACHE PATH "Laplace-Native sources")
endif()
file(TO_CMAKE_PATH "${LAPLACE_NATIVE}" LAPLACE_NATIVE)
