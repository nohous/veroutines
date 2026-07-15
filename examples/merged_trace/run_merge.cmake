# Merge the two VCDs produced by merged_trace and check the result carries both
# an RTL internal and a C++ signal. Invoked by CTest via cmake -P.

execute_process(
    COMMAND ${PYTHON} ${MERGE} ${DIR}/merged.vcd ${DIR}/merged_trace.rtl.vcd ${DIR}/merged_trace.cpp.vcd
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "merge_vcd.py failed")
endif()

file(READ ${DIR}/merged.vcd content)
string(FIND "${content}" "shifter" p_internal)
string(FIND "${content}" "seen" p_cpp)
if(p_internal EQUAL -1 OR p_cpp EQUAL -1)
    message(FATAL_ERROR "merged.vcd is missing the RTL internal or the C++ signal")
endif()

message(STATUS "merged.vcd carries an RTL internal (shifter) and a C++ signal (seen)")
