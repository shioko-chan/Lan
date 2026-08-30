execute_process(
  COMMAND "${LANC}" --run "${SOURCE}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "compact ABI program failed: ${run_error}")
endif()
if(NOT run_output MATCHES
   "calculated: \\[11, missing, 33\\].*flags: \\[true, missing, false, true\\].*labels: \\[\"A!\", missing, \"B!\", \"C!\", \"D!\"\\].*nested: \\[\\[1\\], missing\\].*numbers: \\[1, missing, 3\\].*paired_truths: \\[true, true, missing\\].*scaled: \\[11, missing, 13\\].*scaled_again: \\[12, missing, 16\\].*texts: \\[\"A\", missing, \"B\", \"C\", \"D\"\\].*truths: \\[true, missing, false, true\\]")
  message(FATAL_ERROR "unexpected compact ABI output: ${run_output}")
endif()

execute_process(
  COMMAND "${LANC}" "${SOURCE}" -o "${OUTPUT}"
  RESULT_VARIABLE compile_result
  ERROR_VARIABLE compile_error)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR "Lan compilation failed: ${compile_error}")
endif()

file(READ "${OUTPUT}" ir)
string(REGEX MATCH "define noundef i32 @main\\(\\).*attributes" main_ir "${ir}")
if(NOT main_ir)
  message(FATAL_ERROR "could not isolate generated main function")
endif()

# Number: double[3] plus one-byte presence[3].
if(NOT main_ir MATCHES "malloc\\(i64 24\\)" OR
   NOT main_ir MATCHES "malloc\\(i64 3\\)" OR
   NOT main_ir MATCHES "store i32 1,")
  message(FATAL_ERROR "Number list did not use compact double/presence storage")
endif()

# Boolean: one i2-compatible encoded byte per element.
if(NOT main_ir MATCHES "malloc\\(i64 4\\)" OR
   NOT main_ir MATCHES "store i32 2," OR
   NOT main_ir MATCHES "store i8 2,")
  message(FATAL_ERROR "Boolean list did not use compact three-state storage")
endif()

# Text and nested List values use pointer arrays with null as the missing niche.
if(NOT main_ir MATCHES "malloc\\(i64 40\\)" OR
   NOT main_ir MATCHES "store i32 3," OR
   NOT main_ir MATCHES "malloc\\(i64 16\\)" OR
   NOT main_ir MATCHES "store i32 4," OR
   NOT main_ir MATCHES "store ptr null,")
  message(FATAL_ERROR "pointer lists did not use compact null-niche storage")
endif()

# The old tagged element is { i32, double, ptr } and would require 24 bytes
# per Boolean or pointer element. Unique lengths above make those regressions
# observable as malloc(96), malloc(120), or malloc(48).
if(main_ir MATCHES "malloc\\(i64 (96|120|48)\\)")
  message(FATAL_ERROR "a homogeneous list fell back to tagged element storage")
endif()

# Flat Number/List pairs use the compact operation entry point. Clang/LLVM
# vectorizes its presence-aware arithmetic loop on the current target.
if(NOT main_ir MATCHES "call ptr @lan_number_list_binary\\(")
  message(FATAL_ERROR "flat Number operation used the dynamic recursive path")
endif()
if(NOT main_ir MATCHES "call ptr @lan_number_list_scalar_binary\\(")
  message(FATAL_ERROR "Number scalar broadcast used the dynamic flat path")
endif()
if(NOT main_ir MATCHES "call ptr @lan_boolean_list_binary\\(")
  message(FATAL_ERROR "Boolean column operation used the dynamic flat path")
endif()
if(NOT main_ir MATCHES "call ptr @lan_boolean_list_scalar_binary\\(")
  message(FATAL_ERROR "Boolean scalar broadcast used the dynamic flat path")
endif()
string(REGEX MATCH
       "define [^\n]*@lan_boolean_list_binary\\([^\n]*\\)[^{]*\\{.*\n\\}"
       boolean_loop_ir "${ir}")
if(NOT boolean_loop_ir MATCHES "load <[0-9]+ x i8>" OR
   NOT boolean_loop_ir MATCHES "store <[0-9]+ x i8>")
  message(FATAL_ERROR "Boolean column loop was not SIMD-vectorized")
endif()
string(REGEX MATCH
       "define [^\n]*@lan_number_list_scalar_binary\\([^\n]*\\)[^{]*\\{.*\n\\}"
       scalar_loop_ir "${ir}")
if(NOT scalar_loop_ir MATCHES "fadd <[0-9]+ x double>")
  message(FATAL_ERROR "Number scalar broadcast loop was not SIMD-vectorized")
endif()
string(REGEX MATCHALL "call ptr @lan_flat_list_binary\\(" flat_calls "${main_ir}")
list(LENGTH flat_calls flat_count)
if(flat_count LESS 1)
  message(FATAL_ERROR
          "expected Text scalar broadcast to use the flat compact path")
endif()
if(NOT ir MATCHES "load <[0-9]+ x double>" OR
   NOT ir MATCHES "llvm.loop.isvectorized")
  message(FATAL_ERROR "compact Number loop was not vectorized")
endif()
