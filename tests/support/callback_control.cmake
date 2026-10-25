execute_process(COMMAND "${CMAKE_COMMAND}" "-DNM=${NM}" "-DOBJECT=${OBJECT}"
	-P "${CMAKE_CURRENT_LIST_DIR}/callback_symbols.cmake"
	RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(code EQUAL 0 OR NOT error MATCHES "Unexpected callback dependency: [^\n]*free")
	message(FATAL_ERROR "Callback audit did not reject the known free call: ${output} ${error}")
endif()
message(STATUS "Callback audit rejected the deliberate free dependency")
