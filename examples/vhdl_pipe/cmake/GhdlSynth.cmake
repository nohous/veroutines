# GhdlSynth.cmake - VHDL to Verilog through ghdl's synth backend (no yosys).
#
#   ghdl_synth_verilog(<out> TOP <entity> SOURCES <vhd...>
#                      [STD 08] [FLAGS <ghdl flags>] [GENERICS NAME=VALUE ...])
#       Synthesizes now and sets <out> to the netlist path.
#
#   add_vhdl_verilated_test(<name> TOP <entity> VHDL <vhd...> TB <cpp>
#                           [STD] [FLAGS] [GENERICS] [VERILATOR_ARGS ...])
#       synth -> verilate (PREFIX Vdut, FST trace) -> link veroutines -> add_test.
#
# Synth runs at configure time: verilator's verilate() enumerates its outputs
# during configuration and so needs the netlist present then. The VHDL sources
# are registered as configure dependencies, so editing one and running
# `cmake --build` re-runs configure, re-synthesizes, and rebuilds. SOURCES must
# be in ghdl analysis order (packages first); GENERICS pass -gNAME=VALUE to ghdl.

if(NOT DEFINED GHDL)
    find_program(GHDL ghdl REQUIRED)
endif()

function(ghdl_synth_verilog out_var)
    cmake_parse_arguments(GSV "" "TOP;STD" "SOURCES;FLAGS;GENERICS" ${ARGN})
    if(NOT GSV_STD)
        set(GSV_STD 08)
    endif()
    if(NOT GSV_FLAGS)
        set(GSV_FLAGS -frelaxed -Wno-hide)
    endif()

    set(workdir "${CMAKE_CURRENT_BINARY_DIR}/ghdl/${GSV_TOP}")
    set(netlist "${CMAKE_CURRENT_BINARY_DIR}/gen/${GSV_TOP}.v")

    set(gflags "")
    foreach(g IN LISTS GSV_GENERICS)
        list(APPEND gflags "-g${g}")
    endforeach()

    # Re-run configure (and so re-synth) when any source changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GSV_SOURCES})

    set(_stale FALSE)
    if(NOT EXISTS "${netlist}")
        set(_stale TRUE)
    else()
        foreach(src IN LISTS GSV_SOURCES)
            if("${src}" IS_NEWER_THAN "${netlist}")
                set(_stale TRUE)
            endif()
        endforeach()
    endif()

    if(_stale)
        get_filename_component(_gendir "${netlist}" DIRECTORY)
        file(MAKE_DIRECTORY "${workdir}" "${_gendir}")
        message(STATUS "ghdl synth ${GSV_TOP} -> verilog (no yosys)")
        execute_process(
            COMMAND "${GHDL}" -a --std=${GSV_STD} ${GSV_FLAGS} --workdir=${workdir} ${GSV_SOURCES}
            RESULT_VARIABLE _rc)
        if(_rc)
            message(FATAL_ERROR "ghdl -a failed for ${GSV_TOP} (exit ${_rc})")
        endif()
        execute_process(
            COMMAND "${GHDL}" synth --std=${GSV_STD} ${GSV_FLAGS} --workdir=${workdir}
                    ${gflags} --out=verilog ${GSV_TOP}
            OUTPUT_FILE "${netlist}"
            RESULT_VARIABLE _rc)
        if(_rc)
            file(REMOVE "${netlist}")
            message(FATAL_ERROR "ghdl synth failed for ${GSV_TOP} (exit ${_rc})")
        endif()
    endif()

    set(${out_var} "${netlist}" PARENT_SCOPE)
endfunction()

function(add_vhdl_verilated_test name)
    cmake_parse_arguments(T "" "TOP;STD;TB" "VHDL;FLAGS;GENERICS;VERILATOR_ARGS" ${ARGN})

    ghdl_synth_verilog(netlist
        TOP ${T_TOP} STD "${T_STD}"
        SOURCES ${T_VHDL} FLAGS ${T_FLAGS} GENERICS ${T_GENERICS})

    add_executable(${name} "${T_TB}")
    target_link_libraries(${name} PRIVATE veroutines::veroutines)
    target_compile_features(${name} PRIVATE cxx_std_23)
    target_compile_options(${name} PRIVATE -O2 -g)

    verilate(${name}
        SOURCES "${netlist}"
        TOP_MODULE ${T_TOP}
        PREFIX Vdut
        TRACE_FST
        VERILATOR_ARGS -O2 --trace-structs -Wno-fatal ${T_VERILATOR_ARGS})

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
