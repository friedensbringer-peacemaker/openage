# Copyright 2026-2026 the openage authors. See copying.md for legal info.

# XR fork: write the git commit of SOURCE_DIR to OUTPUT (only if it changed).
# Called after the codegen; builds with OPENAGE_CODEGEN_DIR compare it with
# their own commit before they reuse the generated files.

execute_process(
	COMMAND git rev-parse HEAD
	WORKING_DIRECTORY "${SOURCE_DIR}"
	OUTPUT_VARIABLE commit
	RESULT_VARIABLE result
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
if(NOT result EQUAL 0)
	set(commit "unknown")
endif()

set(content "${commit}\n")
if(EXISTS "${OUTPUT}")
	file(READ "${OUTPUT}" old_content)
	if("${old_content}" STREQUAL "${content}")
		return()
	endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
