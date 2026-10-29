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
	# The renderer may call only these audited consumer operations. The PCM
	# object is separately checked without this allowance (no recursive escape).
	if(ALLOW_PCM AND symbol MATCHES "^_?td_pcm_(select|peek|consume|mark)$")
		continue()
	endif()
	if(ALLOW_ANALYSIS AND symbol MATCHES "^_?td_analysis_(begin|append|finish)$")
		continue()
	endif()
	if(ALLOW_RENDER AND symbol MATCHES "^_?td_audio_render$")
		continue()
	endif()
	# Audited AArch64 outline atomics use LSE or lock-free LL/SC instructions;
	# neither helper calls, allocates, locks or performs I/O.
	if(symbol STREQUAL "__aarch64_cas4_acq_rel" OR symbol STREQUAL "__aarch64_ldadd4_rel")
		continue()
	endif()
	# Stack guards are compiler canary data loads, not callback work. Darwin's
	# fortified memcpy/memset add a destination-size check to the same memory work.
	# Sanitizer instrumentation is diagnostic, not part of production callback work.
	if(NOT symbol MATCHES "^_*(mem(cpy|set)(_chk)?|chkstk(_ms)?|stack_chk_(fail|guard)|security_check_cookie|security_cookie)$" AND
		NOT symbol MATCHES "^_+(asan|ubsan|tsan)_[A-Za-z0-9_]+$")
		message(FATAL_ERROR "Unexpected callback dependency: ${symbol}")
	endif()
endforeach()
message(STATUS "Callback object has no allocation, release, blocking or I/O dependencies")
