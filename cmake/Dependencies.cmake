include(FetchContent)
# Immutable upstream commits and SHA-256 of the exact codeload archives.
# SOURCE_SUBDIR prevents unreviewed upstream defaults from configuring targets.
FetchContent_Declare(teder_miniaudio_source
	URL https://codeload.github.com/mackron/miniaudio/tar.gz/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d
	URL_HASH SHA256=1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55
	DOWNLOAD_EXTRACT_TIMESTAMP FALSE SOURCE_SUBDIR teder-unused)
FetchContent_Declare(teder_flac_source
	URL https://codeload.github.com/xiph/flac/tar.gz/1507800de4b70e21be71f38caa0d9079d0bc6e45
	URL_HASH SHA256=d80ef5facdb21972efe91774da03d6b9abf216aa17093d740fcc411cd8afbb41
	DOWNLOAD_EXTRACT_TIMESTAMP FALSE SOURCE_SUBDIR teder-unused)
FetchContent_Declare(teder_kissfft_source
	URL https://codeload.github.com/mborgerding/kissfft/tar.gz/7bce4153c6bc8aba2db0e889e576f9d00505cbe1
	URL_HASH SHA256=7ad1124648a46977b16ddde03bf243bcd52fc452516c57016584ab4b4f2baadc
	DOWNLOAD_EXTRACT_TIMESTAMP FALSE SOURCE_SUBDIR teder-unused)
FetchContent_MakeAvailable(teder_miniaudio_source teder_flac_source teder_kissfft_source)

include(cmake/Miniaudio.cmake)
add_library(teder_miniaudio OBJECT "${teder_ma_patched}/miniaudio.c")
target_include_directories(teder_miniaudio PUBLIC "${teder_ma_patched}")
target_compile_definitions(teder_miniaudio PUBLIC MA_NO_DECODING MA_NO_ENCODING
	MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_ENABLE_ONLY_SPECIFIC_BACKENDS)
if(WIN32)
	target_compile_definitions(teder_miniaudio PUBLIC MA_ENABLE_WASAPI)
	set(teder_backend_mask "(UINT64_C(1) << TEDER_BACKEND_WASAPI)")
elseif(APPLE)
	target_compile_definitions(teder_miniaudio PUBLIC MA_ENABLE_COREAUDIO)
	set(teder_backend_mask "(UINT64_C(1) << TEDER_BACKEND_COREAUDIO)")
else()
	target_compile_definitions(teder_miniaudio PUBLIC MA_ENABLE_ALSA MA_ENABLE_PULSEAUDIO)
	set(teder_backend_mask "((UINT64_C(1) << TEDER_BACKEND_ALSA) | (UINT64_C(1) << TEDER_BACKEND_PULSE))")
	target_compile_definitions(teder_miniaudio PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
endif()
add_library(teder_kissfft OBJECT "${teder_kissfft_source_SOURCE_DIR}/kiss_fft.c"
	"${teder_kissfft_source_SOURCE_DIR}/kiss_fftr.c")
target_include_directories(teder_kissfft PUBLIC "${teder_kissfft_source_SOURCE_DIR}")
set_target_properties(teder_miniaudio teder_kissfft PROPERTIES C_VISIBILITY_PRESET hidden)

# Keep dependency options local; do not overwrite the caller's BUILD_TESTING or
# BUILD_SHARED_LIBS cache. No programs, Ogg decoder, C++ wrapper or codec workers.
function(teder_configure_flac)
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
	set(BUILD_SHARED_LIBS OFF)
	set(BUILD_TESTING OFF)
	foreach(option BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_DOCS
		WITH_OGG ENABLE_MULTITHREADING INSTALL_MANPAGES INSTALL_PKGCONFIG_MODULES
		INSTALL_CMAKE_CONFIG_MODULE)
		set(${option} OFF)
	endforeach()
	add_subdirectory("${teder_flac_source_SOURCE_DIR}" "${teder_flac_source_BINARY_DIR}" EXCLUDE_FROM_ALL)
	set_target_properties(FLAC PROPERTIES C_VISIBILITY_PRESET hidden DEBUG_POSTFIX "")
endfunction()
teder_configure_flac()
# All codec objects share one allocation ABI. Keep the adapter in the archive
# so installed static consumers need no additional target or public header.
add_library(teder_flac_heap OBJECT src/flac_heap.c)
target_include_directories(teder_flac_heap PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(teder_flac_heap PROPERTIES C_VISIBILITY_PRESET hidden)
target_sources(FLAC PRIVATE $<TARGET_OBJECTS:teder_flac_heap>)
target_include_directories(FLAC PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
if(MSVC)
	target_compile_options(FLAC PRIVATE "$<$<COMPILE_LANGUAGE:C>:/FI${PROJECT_SOURCE_DIR}/cmake/flac_alloc.h>")
	target_compile_options(teder_flac_heap PRIVATE /W4 $<$<BOOL:${TEDER_WERROR}>:/WX>)
else()
	target_compile_options(FLAC PRIVATE "$<$<COMPILE_LANGUAGE:C>:-include${PROJECT_SOURCE_DIR}/cmake/flac_alloc.h>")
	# Corrupt predictors overflow libFLAC's signed restore arithmetic by design
	# (upstream annotates these sites for fuzzing). Make the wrap defined in every
	# GCC/Clang build of this private copy; see docs/dependencies.md.
	target_compile_options(FLAC PRIVATE "$<$<COMPILE_LANGUAGE:C>:-fwrapv>")
	target_compile_options(teder_flac_heap PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${TEDER_WERROR}>:-Werror>)
endif()
install(FILES "${teder_miniaudio_source_SOURCE_DIR}/LICENSE"
	DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/teder RENAME miniaudio.txt)
install(FILES "${teder_flac_source_SOURCE_DIR}/COPYING.Xiph"
	DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/teder RENAME libFLAC.txt)
install(FILES "${teder_kissfft_source_SOURCE_DIR}/LICENSES/BSD-3-Clause"
	DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/teder RENAME KissFFT.txt)

if(BUILD_TESTING)
	FetchContent_Declare(teder_check_source
		URL https://codeload.github.com/libcheck/check/tar.gz/11970a7e112dfe243a2e68773f014687df2900e8
		URL_HASH SHA256=20e7360f5774844c09fde696e5bf71d061e9f53b3d763d886d3318d8893fdda4
		DOWNLOAD_EXTRACT_TIMESTAMP FALSE SOURCE_SUBDIR teder-unused)
	FetchContent_MakeAvailable(teder_check_source)
	function(teder_configure_check)
		set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
		set(BUILD_TESTING OFF)
		set(HAVE_SUBUNIT OFF)
		add_subdirectory("${teder_check_source_SOURCE_DIR}" "${teder_check_source_BINARY_DIR}" EXCLUDE_FROM_ALL SYSTEM)
	endfunction()
	teder_configure_check()
	target_link_libraries(check PUBLIC Threads::Threads)
	# Check 0.15.2 treats missing librt as missing clock_gettime. Probe the actual
	# native function, preserving the independent timer compatibility sources.
	if(MINGW)
		# Enable MinGW's gated alarm declaration for Check's timer shims.
		foreach(target check checkShared)
			target_compile_options(${target} PRIVATE -include "${PROJECT_SOURCE_DIR}/cmake/check_mingw.h")
		endforeach()
	endif()
	include(CheckCSourceCompiles)
	set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
	check_c_source_compiles("#define _POSIX_C_SOURCE 200809L\n#include <time.h>\nint main(void) { struct timespec t; return clock_gettime(CLOCK_REALTIME, &t); }" teder_native_clock_gettime)
	unset(CMAKE_REQUIRED_LIBRARIES)
	if(teder_native_clock_gettime)
		foreach(target check checkShared)
			get_target_property(sources ${target} SOURCES)
			list(FILTER sources EXCLUDE REGEX "/lib/clock_gettime\\.c$")
			set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
		endforeach()
	endif()
endif()
