cmake_minimum_required(VERSION 3.25)
if(CMAKE_HOST_APPLE)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS target")
endif()
project(teder VERSION 0.1.0 LANGUAGES C)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
	message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()
option(BUILD_SHARED_LIBS "Build shared teder libraries" ON)
option(BUILD_TESTING "Build teder tests (Check is a test-only dependency)" ${PROJECT_IS_TOP_LEVEL})
option(TEDER_WITH_NOTCURSES "Build the separate renderer library foundation" OFF)
option(TEDER_WERROR "Treat warnings in teder code as errors" OFF)
set(TEDER_SANITIZER "none" CACHE STRING "Instrumentation: none, address, thread")
set_property(CACHE TEDER_SANITIZER PROPERTY STRINGS none address thread)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
include(cmake/Platform.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(TEDER_WITH_NOTCURSES)
	include(cmake/Notcurses.cmake)
endif()
include(cmake/Dependencies.cmake)

function(teder_target target)
	set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET hidden
		VERSION ${PROJECT_VERSION} SOVERSION 1)
	target_include_directories(${target} PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics $<$<BOOL:${TEDER_WERROR}>:/WX>)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
			$<$<BOOL:${TEDER_WERROR}>:-Werror>)
	endif()
	if(NOT TEDER_SANITIZER STREQUAL "none")
		target_link_options(${target} PUBLIC ${teder_sanitizer_flags})
	endif()
endfunction()

configure_file(cmake/version.h.in generated/teder_version.h @ONLY)
add_library(teder_memory OBJECT src/memory.c)
teder_target(teder_memory)
add_library(teder_pcm_callback OBJECT src/pcm_queue.c)
teder_target(teder_pcm_callback)
add_library(teder_audio_callback OBJECT src/audio_callback.c)
teder_target(teder_audio_callback)
add_library(teder_backend OBJECT src/backend.c src/backend_heap.c)
teder_target(teder_backend)
target_link_libraries(teder_backend PRIVATE teder_miniaudio)
add_library(teder_pcm OBJECT src/pcm_pool.c src/retire.c)
teder_target(teder_pcm)
add_library(teder_lifetime OBJECT src/engine.c src/result.c src/control.c src/request.c src/decode_worker.c)
add_library(teder_source OBJECT src/source_flac.c src/source_file.c src/convert.c)
teder_target(teder_source)
target_link_libraries(teder_source PRIVATE teder_miniaudio FLAC)
teder_target(teder_lifetime)
target_link_libraries(teder_lifetime PRIVATE teder_miniaudio FLAC Threads::Threads)
if(BUILD_SHARED_LIBS)
	target_compile_definitions(teder_lifetime PRIVATE TEDER_BUILD)
else()
	target_compile_definitions(teder_lifetime PRIVATE TEDER_STATIC)
endif()
add_library(teder src/build.c $<TARGET_OBJECTS:teder_memory>
	$<TARGET_OBJECTS:teder_pcm> $<TARGET_OBJECTS:teder_pcm_callback>
	$<TARGET_OBJECTS:teder_audio_callback>
	$<TARGET_OBJECTS:teder_backend>
	$<TARGET_OBJECTS:teder_lifetime>
	$<TARGET_OBJECTS:teder_source>
	$<TARGET_OBJECTS:teder_miniaudio> $<TARGET_OBJECTS:teder_kissfft>)
add_library(teder::teder ALIAS teder)
teder_target(teder)
target_include_directories(teder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(teder PRIVATE TEDER_BUILD)
if(NOT BUILD_SHARED_LIBS)
	target_compile_definitions(teder PUBLIC TEDER_STATIC)
endif()
target_link_libraries(teder PRIVATE $<BUILD_INTERFACE:FLAC> $<INSTALL_INTERFACE:teder::flac>)
if(NOT WIN32)
	target_link_libraries(teder PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
endif()

install(TARGETS teder EXPORT tederTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
# Private static codec accompanies static consumers; no FLAC headers enter our ABI.
if(NOT BUILD_SHARED_LIBS)
	install(FILES $<TARGET_FILE:FLAC> DESTINATION ${CMAKE_INSTALL_LIBDIR}/teder)
endif()
if(TEDER_WITH_NOTCURSES)
	add_library(teder-notcurses src/notcurses_build.c)
	add_library(teder::notcurses ALIAS teder-notcurses)
	teder_target(teder-notcurses)
	set_target_properties(teder-notcurses PROPERTIES EXPORT_NAME notcurses)
	target_compile_definitions(teder-notcurses PRIVATE TEDER_NOTCURSES_BUILD _GNU_SOURCE)
	if(NOT BUILD_SHARED_LIBS)
		target_compile_definitions(teder-notcurses PUBLIC TEDER_NOTCURSES_STATIC)
	endif()
	target_link_libraries(teder-notcurses PUBLIC teder PRIVATE teder::notcurses_runtime)
	install(TARGETS teder-notcurses EXPORT tederTargets
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES docs/api-contract.md docs/build.md docs/dependencies.md docs/sources.md
	DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
set(teder_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/teder")
configure_package_config_file(cmake/tederConfig.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/tederConfig.cmake"
	INSTALL_DESTINATION "${teder_config_dir}" PATH_VARS CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/tederConfigVersion.cmake"
	VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(EXPORT tederTargets NAMESPACE teder:: DESTINATION "${teder_config_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tederConfig.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/tederConfigVersion.cmake" DESTINATION "${teder_config_dir}")
if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()
