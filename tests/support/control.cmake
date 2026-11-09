# Supervise intentional defects in a child; timeout kills the whole process.
# Never unwind/free memory concurrently with a stranded test worker.
if(CASE STREQUAL "failed-check")
	# A failed check while workers still write the test's frame must report and exit 1
	# on every run. Unwinding that frame into Check's result collection crashes most runs,
	# but not all: Check's own report has the same form. Only the harness's report adds
	# the marker line, so every run must print it.
	file(WRITE "${LOG}" "")
	foreach(run RANGE 1 10)
		execute_process(COMMAND "${PROGRAM}" "control-${CASE}" TIMEOUT 15
			RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
		set(text "${output}${errors}")
		file(APPEND "${LOG}" "run ${run}: result=${result}\n${text}")
		if(NOT result STREQUAL "1" OR NOT text MATCHES "deliberate failure with a live engine"
			OR NOT text MATCHES "teder test harness: failed check reported without unwinding"
			OR text MATCHES "Sanitizer|runtime error")
			message(FATAL_ERROR "A failed check with a live engine did not exit cleanly on run ${run}; see ${LOG}")
		endif()
	endforeach()
	return()
endif()
execute_process(COMMAND "${PROGRAM}" "control-${CASE}" TIMEOUT 3
	RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${LOG}" "result=${result}\n${output}${errors}")
if(CASE STREQUAL "missing-worker")
	if(NOT result MATCHES "[Tt]imeout" OR NOT output MATCHES "worker pending at checkpoint")
		message(FATAL_ERROR "Missing-worker control did not reach its expected timeout; see ${LOG}")
	endif()
else()
	if(CASE STREQUAL "leak")
		set(expected "leak site=control.leak bytes=8")
	elseif(CASE STREQUAL "wrong-free")
		set(expected "wrong free: unowned block")
	elseif(CASE STREQUAL "worker")
		set(expected "worker check failed: false")
	elseif(CASE STREQUAL "pcm")
		set(expected "pcm mismatch frame=1 channel=1")
	else()
		message(FATAL_ERROR "Unknown control: ${CASE}")
	endif()
	if(NOT result STREQUAL "42" OR NOT errors MATCHES "${expected}")
		message(FATAL_ERROR "${CASE} control did not detect the intended defect; see ${LOG}")
	endif()
endif()
