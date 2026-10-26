# The original header hash is verified by Miniaudio.cmake. Reuse the normal
# resource destructor for failures BEFORE worker creation, without joining an
# uninitialized thread or passing through the public initialized-state guard.
function(teder_ma_section start end output)
	string(FIND "${teder_ma_header}" "${start}" begin)
	string(FIND "${teder_ma_header}" "${end}" finish)
	if(begin LESS 0 OR finish LESS begin)
		message(FATAL_ERROR "miniaudio device patch boundaries changed")
	endif()
	math(EXPR length "${finish} - ${begin}")
	string(SUBSTRING "${teder_ma_header}" ${begin} ${length} section)
	set(${output} "${section}" PARENT_SCOPE)
endfunction()
teder_ma_section("MA_API void ma_device_uninit(ma_device* pDevice)\n{"
	"MA_API ma_context* ma_device_get_context(ma_device* pDevice)\n{" original)
string(FIND "${original}" "    if (pDevice->pContext->callbacks.onDeviceUninit != NULL) {" start)
if(start LESS 0)
	message(FATAL_ERROR "miniaudio device cleanup anchor changed")
endif()
string(SUBSTRING "${original}" ${start} -1 cleanup)
string(SUBSTRING "${original}" 0 ${start} prefix)
set(helper "static void ma_device_uninit_resources__teder(ma_device* pDevice)\n{\n${cleanup}")
string(REPLACE "${original}" "${prefix}    ma_device_uninit_resources__teder(pDevice);\n}\n\n"
	teder_ma_header "${teder_ma_header}")
teder_ma_section("MA_API ma_result ma_device_init(ma_context* pContext, const ma_device_config* pConfig, ma_device* pDevice)\n{"
	"MA_API ma_result ma_device_init_ex(const ma_backend backends[], ma_uint32 backendCount, const ma_context_config* pContextConfig, const ma_device_config* pConfig, ma_device* pDevice)\n{" original)
set(body "${original}")
teder_ma_replace(body "ma_device_uninit(pDevice);" "ma_device_uninit_resources__teder(pDevice);")
teder_ma_replace(body
	"result = pContext->callbacks.onDeviceInit(pDevice, pConfig, &descriptorPlayback, &descriptorCapture);\n    if (result != MA_SUCCESS) {"
	"result = pContext->callbacks.onDeviceInit(pDevice, pConfig, &descriptorPlayback, &descriptorCapture);\n    if (result != MA_SUCCESS) {\n        ma_event_uninit(&pDevice->stopEvent);")
string(REPLACE "${original}" "${helper}${body}" teder_ma_header "${teder_ma_header}")
# CoreAudio owns a separate stop notification event. Track successful init so
# its constructor failure and normal destruction use the same safe unwind.
teder_ma_replace(teder_ma_header
	"ma_event stopEvent;\n            ma_uint32 originalPeriodSizeInFrames;"
	"ma_event stopEvent;\n            ma_bool32 stopEventInitialized;\n            ma_uint32 originalPeriodSizeInFrames;")
teder_ma_replace(teder_ma_header "    ma_event_init(&pDevice->coreaudio.stopEvent);"
	[=[    result = ma_event_init(&pDevice->coreaudio.stopEvent);
    if (result != MA_SUCCESS) {
        ma_device_uninit__coreaudio(pDevice);
        return result;
    }
    pDevice->coreaudio.stopEventInitialized = MA_TRUE;]=])
teder_ma_replace(teder_ma_header
	"    if (pDevice->coreaudio.pAudioBufferList) {\n        ma_free(pDevice->coreaudio.pAudioBufferList, &pDevice->pContext->allocationCallbacks);\n    }"
	[=[    if (pDevice->coreaudio.pAudioBufferList) {
        ma_free(pDevice->coreaudio.pAudioBufferList, &pDevice->pContext->allocationCallbacks);
    }
    if (pDevice->coreaudio.stopEventInitialized) {
        ma_event_uninit(&pDevice->coreaudio.stopEvent);
    }]=])
