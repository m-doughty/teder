# Check 0.15.2 packs and unpacks messages through tables of typed functions cast to
# pfun/upfun, so every call goes through a mismatched function-pointer type, which
# is undefined behaviour. A generated copy of src/check_pack.c gives each table a
# CheckMsg wrapper that converts the pointer back to its message type. Packing
# callers cast a specific message object to CheckMsg *, and unpacking passes a real
# union, so the conversion is defined both ways and no union member is accessed.
# Never rewrite downloaded sources. The hash and exact anchor fail closed.
set(teder_check_pack_original "${teder_check_source_SOURCE_DIR}/src/check_pack.c")
file(SHA256 "${teder_check_pack_original}" teder_check_pack_hash)
if(NOT teder_check_pack_hash STREQUAL "2e7ad0fc78fcef5ebbb454134441c3a43703bc320cf4fdc17a6385cb21d4948c")
	message(FATAL_ERROR "Check pack-table fix requires review for this source hash: ${teder_check_pack_original} has SHA-256 ${teder_check_pack_hash}")
endif()
file(READ "${teder_check_pack_original}" teder_check_pack)
set(tables [=[static pfun pftab[] = {
    (pfun) pack_ctx,
    (pfun) pack_fail,
    (pfun) pack_loc,
    (pfun) pack_duration
};

static upfun upftab[] = {
    (upfun) upack_ctx,
    (upfun) upack_fail,
    (upfun) upack_loc,
    (upfun) upack_duration
};]=])
string(FIND "${teder_check_pack}" "${tables}" first)
string(FIND "${teder_check_pack}" "${tables}" last REVERSE)
if(first LESS 0 OR NOT first EQUAL last)
	message(FATAL_ERROR "Check pack-table patch anchor missing or repeated in ${teder_check_pack_original}")
endif()
# Table order follows enum ck_msg_type: CTX, FAIL, LOC, DURATION.
set(wrappers "")
set(pack_entries "")
set(upack_entries "")
set(kinds ctx fail loc duration)
set(types CtxMsg FailMsg LocMsg DurationMsg)
foreach(kind type IN ZIP_LISTS kinds types)
	string(APPEND wrappers "static size_t pack_${kind}_msg(char **buf, CheckMsg * msg)\n{\n"
		"    return pack_${kind}(buf, (${type} *) (void *) msg);\n}\n\n"
		"static void upack_${kind}_msg(char **buf, CheckMsg * msg)\n{\n"
		"    upack_${kind}(buf, (${type} *) (void *) msg);\n}\n\n")
	list(APPEND pack_entries "    pack_${kind}_msg")
	list(APPEND upack_entries "    upack_${kind}_msg")
endforeach()
list(JOIN pack_entries ",\n" pack_entries)
list(JOIN upack_entries ",\n" upack_entries)
string(REPLACE "${tables}" "${wrappers}static pfun pftab[] = {\n${pack_entries}\n};\n\nstatic upfun upftab[] = {\n${upack_entries}\n};"
	teder_check_pack "${teder_check_pack}")
set(teder_check_patched "${PROJECT_BINARY_DIR}/check-patched")
file(MAKE_DIRECTORY "${teder_check_patched}")
file(WRITE "${teder_check_patched}/check_pack.c" "${teder_check_pack}")

# Both Check targets list the original file by its source-relative name. Swap in
# the generated copy, and fail if that entry ever moves or disappears.
foreach(target check checkShared)
	get_target_property(sources ${target} SOURCES)
	list(FIND sources "check_pack.c" index)
	if(index LESS 0)
		message(FATAL_ERROR "Check target ${target} no longer lists check_pack.c; review cmake/CheckPack.cmake")
	endif()
	list(REMOVE_AT sources ${index})
	list(FIND sources "check_pack.c" repeated)
	if(NOT repeated LESS 0)
		message(FATAL_ERROR "Check target ${target} lists check_pack.c more than once; review cmake/CheckPack.cmake")
	endif()
	list(INSERT sources ${index} "${teder_check_patched}/check_pack.c")
	set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
	# The copy keeps upstream's quoted includes, which resolve beside the original.
	target_include_directories(${target} PRIVATE "${teder_check_source_SOURCE_DIR}/src")
endforeach()
