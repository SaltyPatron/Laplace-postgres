# Resolve Native before loading its shared CMake helpers.
include("${CMAKE_CURRENT_LIST_DIR}/../../Laplace-Operations/toolchain/source-selection.cmake")
laplace_resolve_source_path(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native" "Laplace-Native sources")
