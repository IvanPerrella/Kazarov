# Paths and tools the Kazarov rules run. Included by Kazarov.cmake, after KZ_ROOT is set.

file(RELATIVE_PATH KZ_ROOT_REL "${CMAKE_SOURCE_DIR}" "${KZ_ROOT}")
file(RELATIVE_PATH KZ_BUILD_REL "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
set(KZ_BIN "${KZ_ROOT}/bin")
set(KZ_INCLUDE_REL "${KZ_ROOT_REL}/include")
set(KZ_PYTHONPATH "${KZ_ROOT}/python")

find_program(KZ_PYTHON NAMES python3 python HINTS $ENV{VIRTUAL_ENV}/bin REQUIRED)

# The binutils that assemble and inspect the MIPS objects. Each can be overridden
# by setting the KZ_ variable before the include.
set(MIPS_TOOL_PREFIX mips-ps2-decompals-
    CACHE STRING "Prefix of the binutils-mips-ps2-decompals tools")
if(NOT DEFINED KZ_AS)
    set(KZ_AS ${MIPS_TOOL_PREFIX}as)
endif()
if(NOT DEFINED KZ_OBJCOPY)
    set(KZ_OBJCOPY ${MIPS_TOOL_PREFIX}objcopy)
endif()
if(NOT DEFINED KZ_OBJDUMP)
    set(KZ_OBJDUMP ${MIPS_TOOL_PREFIX}objdump)
endif()
if(NOT DEFINED KZ_READELF)
    set(KZ_READELF ${MIPS_TOOL_PREFIX}readelf)
endif()

# MWLD runs under wibo, as the MWCC compilers do.
set(KZ_WIBO wibo CACHE STRING "The wibo that runs the Metrowerks tools")
set(KZ_LD ${KZ_WIBO} ${KZ_MWLD})

if(NOT DEFINED KZ_MWCC_RUNNER)
    set(KZ_MWCC_RUNNER wibo)
endif()
if(NOT DEFINED KZ_AS_FLAGS)
    set(KZ_AS_FLAGS "")
endif()
if(NOT DEFINED KZ_LIB_INCLUDE_DIRS)
    set(KZ_LIB_INCLUDE_DIRS "")
endif()

# Every compile and assembly depends on these, so a changed tool or image rebuilds
# the objects. The image id file exists only inside the container image.
set(KZ_TOOL_DEPENDS ${KZ_BIN}/kz-mwcc ${KZ_BIN}/kz-mwcc-exec)
if(EXISTS /opt/kazarov/image-id)
    list(APPEND KZ_TOOL_DEPENDS /opt/kazarov/image-id)
endif()

# Path under CMAKE_SOURCE_DIR for a relative path, unchanged if absolute.
function(kz_absolute_path path out_var)
    if(IS_ABSOLUTE "${path}")
        set(${out_var} "${path}" PARENT_SCOPE)
    else()
        set(${out_var} "${CMAKE_SOURCE_DIR}/${path}" PARENT_SCOPE)
    endif()
endfunction()
