# Reviewed fixes to pinned 0.11.25 only. Never rewrite downloaded sources.
# Exact anchors fail closed if a dependency update changes the affected code.
function(teder_ma_replace variable before after)
	string(FIND "${${variable}}" "${before}" position)
	if(position LESS 0)
		message(FATAL_ERROR "miniaudio patch anchor missing: ${before}")
	endif()
	string(REPLACE "${before}" "${after}" changed "${${variable}}")
	set(${variable} "${changed}" PARENT_SCOPE)
endfunction()
file(SHA256 "${teder_miniaudio_source_SOURCE_DIR}/miniaudio.h" teder_ma_hash)
if(NOT teder_ma_hash STREQUAL "ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89")
	message(FATAL_ERROR "miniaudio context fixes require review for this source hash")
endif()
file(READ "${teder_miniaudio_source_SOURCE_DIR}/miniaudio.h" teder_ma_header)
set(start "MA_API ma_result ma_context_init(const ma_backend backends[], ma_uint32 backendCount, const ma_context_config* pConfig, ma_context* pContext)\n{")
set(end "MA_API ma_result ma_context_uninit(ma_context* pContext)\n{")
string(FIND "${teder_ma_header}" "${start}" begin)
string(FIND "${teder_ma_header}" "${end}" finish)
if(begin LESS 0 OR finish LESS begin)
	message(FATAL_ERROR "miniaudio context patch boundaries changed")
endif()
math(EXPR length "${finish} - ${begin}")
string(SUBSTRING "${teder_ma_header}" ${begin} ${length} original)
set(body "${original}")
teder_ma_replace(body "pContext->pLog = NULL;  /* Logging is not available. */" "return result; /* No initialized log to release. */")
teder_ma_replace(body
	"result = ma_context_init_backend_apis(pContext);\n    if (result != MA_SUCCESS) {\n        return result;\n    }"
	"result = ma_context_init_backend_apis(pContext);\n    if (result != MA_SUCCESS) {\n        goto failed_apis;\n    }")
teder_ma_replace(body
	"if (result == MA_SUCCESS) {\n            result = ma_mutex_init(&pContext->deviceEnumLock);"
	"if (result == MA_SUCCESS) {\n            pContext->backend = backend;\n            result = ma_mutex_init(&pContext->deviceEnumLock);")
teder_ma_replace(body
	"ma_log_postf(ma_context_get_log(pContext), MA_LOG_LEVEL_WARNING, \"Failed to initialize mutex for device enumeration. ma_context_get_devices() is not thread safe.\\n\");"
	"goto failed_backend;")
teder_ma_replace(body
	"ma_log_postf(ma_context_get_log(pContext), MA_LOG_LEVEL_WARNING, \"Failed to initialize mutex for device info retrieval. ma_context_get_device_info() is not thread safe.\\n\");"
	"goto failed_enum;")
teder_ma_replace(body
	"/* If we get here it means an error occurred. */\n    MA_ZERO_OBJECT(pContext);  /* Safety. */\n    return MA_NO_BACKEND;"
	[=[result = MA_NO_BACKEND;
    goto failed_apis;
failed_enum:
    ma_mutex_uninit(&pContext->deviceEnumLock);
failed_backend:
    if (pContext->callbacks.onContextUninit != NULL) {
        pContext->callbacks.onContextUninit(pContext);
    }
failed_apis:
    ma_context_uninit_backend_apis(pContext);
    if (pContext->pLog == &pContext->log) {
        ma_log_uninit(&pContext->log);
    }
    MA_ZERO_OBJECT(pContext);
    return result;]=])
string(REPLACE "${original}" "${body}" teder_ma_header "${teder_ma_header}")
# Early Win32 DLL-load failures precede COM initialization. Zero is S_OK and
# would incorrectly authorize CoUninitialize on that failure path.
teder_ma_replace(teder_ma_header
	"static ma_result ma_context_init_backend_apis__win32(ma_context* pContext)\n{"
	"static ma_result ma_context_init_backend_apis__win32(ma_context* pContext)\n{\n    pContext->win32.CoInitializeResult = -1;")
# WASAPI has already loaded AVRT before command synchronization/thread setup.
set(start "        result = ma_mutex_init(&pContext->wasapi.commandLock);")
set(end "/******************************************************************************\n\nDirectSound Backend")
string(FIND "${teder_ma_header}" "${start}" begin)
string(FIND "${teder_ma_header}" "${end}" finish)
if(begin LESS 0 OR finish LESS begin)
	message(FATAL_ERROR "miniaudio WASAPI patch boundaries changed")
endif()
math(EXPR length "${finish} - ${begin}")
string(SUBSTRING "${teder_ma_header}" ${begin} ${length} original)
set(body "${original}")
teder_ma_replace(body "return result;" "goto failed_resources;")
teder_ma_replace(body "    return MA_SUCCESS;\n}" [=[    return MA_SUCCESS;
failed_resources:
    if (pContext->wasapi.hAvrt) {
        ma_dlclose(ma_context_get_log(pContext), pContext->wasapi.hAvrt);
    }
#if defined(MA_WIN32_UWP)
    if (pContext->wasapi.hMMDevapi) {
        ma_dlclose(ma_context_get_log(pContext), pContext->wasapi.hMMDevapi);
    }
#endif
    return result;
}]=])
string(REPLACE "${original}" "${body}" teder_ma_header "${teder_ma_header}")
# A backend that fails before publishing its destructors must release its DLL/SO.
teder_ma_replace(teder_ma_header
	"ma_log_postf(ma_context_get_log(pContext), MA_LOG_LEVEL_ERROR, \"[ALSA] WARNING: Failed to initialize mutex for internal device enumeration.\");\n        return result;"
	"ma_log_postf(ma_context_get_log(pContext), MA_LOG_LEVEL_ERROR, \"[ALSA] WARNING: Failed to initialize mutex for internal device enumeration.\");\n#ifndef MA_NO_RUNTIME_LINKING\n        ma_dlclose(ma_context_get_log(pContext), pContext->alsa.asoundSO);\n#endif\n        return result;")
foreach(field pApplicationName pServerName)
	if(field STREQUAL "pApplicationName")
		set(prefix "if (pContext->pulse.pApplicationName == NULL && pConfig->pulse.pApplicationName != NULL) {\n        ")
	else()
		set(prefix "if (pContext->pulse.pServerName == NULL && pConfig->pulse.pServerName != NULL) {\n        ma_free(pContext->pulse.pApplicationName, &pContext->allocationCallbacks);\n        ")
	endif()
	teder_ma_replace(teder_ma_header "${prefix}return MA_OUT_OF_MEMORY;"
		"${prefix}\n#ifndef MA_NO_RUNTIME_LINKING\n        ma_dlclose(ma_context_get_log(pContext), pContext->pulse.pulseSO);\n#endif\n        return MA_OUT_OF_MEMORY;")
endforeach()
set(teder_ma_patched "${PROJECT_BINARY_DIR}/miniaudio-patched")
file(MAKE_DIRECTORY "${teder_ma_patched}")
file(WRITE "${teder_ma_patched}/miniaudio.h" "${teder_ma_header}")
configure_file("${teder_miniaudio_source_SOURCE_DIR}/miniaudio.c"
	"${teder_ma_patched}/miniaudio.c" COPYONLY)
