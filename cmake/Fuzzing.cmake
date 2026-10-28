if(TEDER_BUILD_FUZZERS)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
		OR BUILD_SHARED_LIBS OR BUILD_TESTING OR TEDER_BUILD_EXAMPLES
		OR NOT TEDER_SANITIZER STREQUAL "address")
		message(FATAL_ERROR "Fuzzers require Linux Clang, static libraries, TEDER_SANITIZER=address, BUILD_TESTING=OFF and TEDER_BUILD_EXAMPLES=OFF")
	endif()
	# Apply before dependencies: codec and converter paths need feedback too.
	# Source coverage supplies independently inspectable reachability evidence.
	add_compile_options(-fsanitize=fuzzer-no-link -fprofile-instr-generate -fcoverage-mapping)
	add_link_options(-fprofile-instr-generate)
endif()
