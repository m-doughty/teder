set(TEDER_NOTCURSES_PREFIX "" CACHE PATH "Installed m-doughty/notcurses fork prefix")
set(TEDER_NOTCURSES_REVISION "" CACHE STRING "Source revision used for that installation")
set(teder_notcurses_pin "fd67f2bafec21365f72a418797799d2051376fbe")
if(NOT TEDER_NOTCURSES_PREFIX OR NOT TEDER_NOTCURSES_REVISION STREQUAL teder_notcurses_pin)
	message(FATAL_ERROR "TEDER_WITH_NOTCURSES requires an installed m-doughty/notcurses at ${teder_notcurses_pin}; set TEDER_NOTCURSES_PREFIX and TEDER_NOTCURSES_REVISION")
endif()
if(MSVC)
	message(FATAL_ERROR "The required notcurses fork uses MSYS2 UCRT64 on Windows, not MSVC")
endif()
find_path(teder_notcurses_include notcurses/notcurses.h
	PATHS "${TEDER_NOTCURSES_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
find_library(teder_notcurses_library NAMES notcurses-core
	PATHS "${TEDER_NOTCURSES_PREFIX}/lib" "${TEDER_NOTCURSES_PREFIX}/lib64" NO_DEFAULT_PATH REQUIRED)
if(teder_notcurses_library MATCHES "\\.a$" AND NOT teder_notcurses_library MATCHES "\\.dll\\.a$")
	message(FATAL_ERROR "The renderer requires a shared notcurses runtime/import library, not a private static copy")
endif()
add_library(teder::notcurses_runtime UNKNOWN IMPORTED)
set_target_properties(teder::notcurses_runtime PROPERTIES
	IMPORTED_LOCATION "${teder_notcurses_library}"
	INTERFACE_INCLUDE_DIRECTORIES "${teder_notcurses_include}")
