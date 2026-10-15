if(NOT DEFINED T1T OR NOT DEFINED CLANG OR NOT DEFINED INPUT OR
   NOT DEFINED RECURSION_INPUT OR NOT DEFINED WORK)
  message(FATAL_ERROR "T1T, CLANG, INPUT, RECURSION_INPUT and WORK are required")
endif()

file(MAKE_DIRECTORY "${WORK}")

function(llvm_case case_name input expected)
  set(IR "${WORK}/${case_name}.ll")
  if(WIN32)
    set(PROGRAM "${WORK}/${case_name}.exe")
  else()
    set(PROGRAM "${WORK}/${case_name}")
  endif()

  execute_process(
    COMMAND "${T1T}" transfer "${input}" --target=llvm "--asm-out=${IR}"
    RESULT_VARIABLE generate_result
    OUTPUT_VARIABLE generate_output
    ERROR_VARIABLE generate_error)
  if(NOT generate_result EQUAL 0)
    message(FATAL_ERROR "${case_name}: LLVM IR generation failed:\n${generate_output}\n${generate_error}")
  endif()

  execute_process(
    COMMAND "${CLANG}" "${IR}" -O2 -Wno-override-module -o "${PROGRAM}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error)
  if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "${case_name}: clang rejected generated IR:\n${compile_output}\n${compile_error}")
  endif()

  execute_process(
    COMMAND "${PROGRAM}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "${case_name}: generated LLVM program failed:\n${run_output}\n${run_error}")
  endif()

  string(REPLACE "\r\n" "\n" normalized_output "${run_output}")
  if(NOT normalized_output STREQUAL "${expected}")
    message(FATAL_ERROR "${case_name}: unexpected LLVM output: '${normalized_output}'")
  endif()
  message(STATUS "${case_name}: LLVM IR compiled and ran successfully")
endfunction()

llvm_case(arithmetic "${INPUT}" "7\n6\n")
llvm_case(recursion "${RECURSION_INPUT}" "120\n")
