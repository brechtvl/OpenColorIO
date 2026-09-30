# SPDX-License-Identifier: BSD-3-Clause
# Copyright Contributors to the OpenColorIO Project.

# rapidyaml v0.16.0 (amalgamated single source, modified to include ryml.hpp by relative path)
# https://github.com/biojppm/rapidyaml
add_library(rapidyaml INTERFACE IMPORTED GLOBAL)
set_target_properties(rapidyaml PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/include"
    INTERFACE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/src/ryml.cpp"
)
