execute_process(COMMAND "${NM}" -u "${OBJECT}" RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT code EQUAL 0)
	message(FATAL_ERROR "Callback symbol audit failed to run: ${error}")
endif()
string(REPLACE "\n" ";" lines "${output}")
foreach(line IN LISTS lines)
	string(STRIP "${line}" line)
	if(line STREQUAL "")
		continue()
	endif()
	string(REGEX REPLACE "^.*[ \t]" "" symbol "${line}")
	# ELF linker anchor for PIC data references, including ASan's runtime options.
	# The actual referenced symbols are still checked individually below.
	if(symbol STREQUAL "_GLOBAL_OFFSET_TABLE_")
		continue()
	endif()
	# Sanitizer instrumentation is diagnostic, not part of production callback work.
	if(NOT symbol MATCHES "^_*(memcpy|memset|chkstk(_ms)?|stack_chk_fail|security_check_cookie|security_cookie)$" AND
		NOT symbol MATCHES "^_+(asan|ubsan|tsan)_[A-Za-z0-9_]+$")
		message(FATAL_ERROR "Unexpected callback dependency: ${symbol}")
	endif()
endforeach()
message(STATUS "Callback object has no allocation, release, blocking or I/O dependencies")
