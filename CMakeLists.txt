cmake_minimum_required(VERSION 3.20)
project(lan LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(LLVM REQUIRED CONFIG)
find_program(CLANG_EXECUTABLE clang HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

set(LAN_RECORD_BITCODE ${CMAKE_CURRENT_BINARY_DIR}/lan_record_runtime.bc)
add_custom_command(
  OUTPUT ${LAN_RECORD_BITCODE}
  COMMAND ${CLANG_EXECUTABLE} -std=c17 -O2 -emit-llvm -c
          ${CMAKE_CURRENT_SOURCE_DIR}/runtime/record.c -o ${LAN_RECORD_BITCODE}
  DEPENDS runtime/record.c
  VERBATIM)
add_custom_target(lan_record_runtime DEPENDS ${LAN_RECORD_BITCODE})

BISON_TARGET(LanParser src/parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
             DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.hpp)
FLEX_TARGET(LanLexer src/lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)
ADD_FLEX_BISON_DEPENDENCY(LanLexer LanParser)

add_executable(lanc
  src/main.cpp
  src/Codegen.cpp
  ${BISON_LanParser_OUTPUTS}
  ${FLEX_LanLexer_OUTPUTS}
)
add_dependencies(lanc lan_record_runtime)

target_include_directories(lanc PRIVATE include ${CMAKE_CURRENT_BINARY_DIR} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(lanc PRIVATE ${LLVM_DEFINITIONS})
target_compile_definitions(lanc PRIVATE LAN_RECORD_BITCODE_PATH="${LAN_RECORD_BITCODE}")
llvm_map_components_to_libnames(LLVM_LIBS Core Support Passes BitReader Linker)
target_link_libraries(lanc PRIVATE ${LLVM_LIBS})

find_program(LLI_EXECUTABLE lli HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
target_compile_definitions(lanc PRIVATE LAN_LLI_PATH="${LLI_EXECUTABLE}")

enable_testing()
add_test(NAME reject_direct_recursion
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_recursion_error.lan)
set_tests_properties(reject_direct_recursion PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_indirect_recursion
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/indirect_recursion_error.lan)
set_tests_properties(reject_indirect_recursion PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_recursion_in_default
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_recursion_error.lan)
set_tests_properties(reject_recursion_in_default PROPERTIES WILL_FAIL TRUE)
add_test(NAME run_types
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/examples/types.lan)
set_tests_properties(run_types PROPERTIES PASS_REGULAR_EXPRESSION "你好，小明，你已经成年")
add_test(NAME reject_type_error
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/type_error.lan)
set_tests_properties(reject_type_error PROPERTIES WILL_FAIL TRUE)
add_test(NAME enforce_block_scope
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/scope_error.lan)
set_tests_properties(enforce_block_scope PROPERTIES WILL_FAIL TRUE)
add_test(NAME short_circuit_is_lazy
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/short_circuit.lan)
set_tests_properties(short_circuit_is_lazy PROPERTIES PASS_REGULAR_EXPRESSION "false")
add_test(NAME missing_propagates_left_to_right
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_left_to_right.lan)
set_tests_properties(missing_propagates_left_to_right
  PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{number_missing: missing, number_value: 19, text_missing: missing, text_value: true\}]=])
add_test(NAME reject_unhandled_missing_at_runtime
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_runtime_error.lan)
set_tests_properties(reject_unhandled_missing_at_runtime PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_unhandled_final_missing
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_final_error.lan)
set_tests_properties(reject_unhandled_final_missing PROPERTIES WILL_FAIL TRUE)
add_test(NAME question_is_expression_local
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/question_is_expression_local.lan)
set_tests_properties(question_is_expression_local PROPERTIES PASS_REGULAR_EXPRESSION "7")
add_test(NAME reject_old_print_effect
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/side_effect_error.lan)
set_tests_properties(reject_old_print_effect PROPERTIES WILL_FAIL TRUE)
add_test(NAME unicode_names
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/unicode.lan)
set_tests_properties(unicode_names PROPERTIES PASS_REGULAR_EXPRESSION "42")
add_test(NAME infer_types
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/inference.lan)
set_tests_properties(infer_types PROPERTIES PASS_REGULAR_EXPRESSION "42")
add_test(NAME simple_formula
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/simple.lan)
set_tests_properties(simple_formula PROPERTIES PASS_REGULAR_EXPRESSION "7")
add_test(NAME reject_bad_indentation
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/indent_error.lan)
set_tests_properties(reject_bad_indentation PROPERTIES WILL_FAIL TRUE)
add_test(NAME lexical_shadowing
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/shadowing.lan)
set_tests_properties(lexical_shadowing PROPERTIES PASS_REGULAR_EXPRESSION "20")
add_test(NAME homogeneous_list
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/examples/list.lan)
set_tests_properties(homogeneous_list PROPERTIES PASS_REGULAR_EXPRESSION "\\[1, 2, 3\\]")
add_test(NAME homogeneous_list_length
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_length.lan)
set_tests_properties(homogeneous_list_length PROPERTIES PASS_REGULAR_EXPRESSION "5")
add_test(NAME reject_length_of_non_list
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_type_error.lan)
set_tests_properties(reject_length_of_non_list PROPERTIES WILL_FAIL TRUE)
add_test(NAME option_number
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_number.lan)
set_tests_properties(option_number PROPERTIES PASS_REGULAR_EXPRESSION "0")
add_test(NAME option_text_null_niche
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_text.lan)
set_tests_properties(option_text_null_niche PROPERTIES PASS_REGULAR_EXPRESSION "fallback")
add_test(NAME option_boolean_i2_niche
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_boolean.lan)
set_tests_properties(option_boolean_i2_niche PROPERTIES PASS_REGULAR_EXPRESSION "false")
add_test(NAME option_list_null_niche
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_list.lan)
set_tests_properties(option_list_null_niche PROPERTIES PASS_REGULAR_EXPRESSION "0")
add_test(NAME option_list_element_missing
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_list_element.lan)
set_tests_properties(option_list_element_missing PROPERTIES
  PASS_REGULAR_EXPRESSION "\\[1, missing, 3\\]")
add_test(NAME option_missing_result
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_result.lan)
set_tests_properties(option_missing_result PROPERTIES PASS_REGULAR_EXPRESSION "missing")
add_test(NAME option_function_parameter
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/missing_parameter.lan)
set_tests_properties(option_function_parameter PROPERTIES PASS_REGULAR_EXPRESSION "missing")
add_test(NAME default_parameters
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/examples/default_parameters.lan)
set_tests_properties(default_parameters PROPERTIES PASS_REGULAR_EXPRESSION "小明")
add_test(NAME default_references_earlier_parameter
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_reference.lan)
set_tests_properties(default_references_earlier_parameter PROPERTIES PASS_REGULAR_EXPRESSION "15")
add_test(NAME default_can_be_missing
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_missing.lan)
set_tests_properties(default_can_be_missing PROPERTIES PASS_REGULAR_EXPRESSION "7, 7, 3, 10, missing")
add_test(NAME reject_required_after_default
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_order_error.lan)
set_tests_properties(reject_required_after_default PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_missing_required_argument
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_arity_error.lan)
set_tests_properties(reject_missing_required_argument PROPERTIES WILL_FAIL TRUE)

add_test(NAME record_literal_is_sorted
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_literal.lan)
set_tests_properties(record_literal_is_sorted PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\{active: true, age: 20, name: "小明"\}]=])
add_test(NAME record_static_dynamic_access
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_access.lan)
set_tests_properties(record_static_dynamic_access PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\{absent: "没有", age: 21, name: "小明"\}]=])
add_test(NAME record_persistent_update
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_update.lan)
set_tests_properties(record_persistent_update PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\[\{age: 20, name: "小明"\}, \{age: 21, city: "悉尼", name: "小兰"\}\]]=])
add_test(NAME reject_record_field_type_mismatch
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_type_error.lan)
set_tests_properties(reject_record_field_type_mismatch PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_missing_record_field
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_missing_error.lan)
set_tests_properties(reject_missing_record_field PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_duplicate_record_field
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_duplicate_error.lan)
set_tests_properties(reject_duplicate_record_field PROPERTIES WILL_FAIL TRUE)
add_test(NAME record_nested_and_empty
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_nested.lan)
set_tests_properties(record_nested_and_empty PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\{fallback: \{\}, value: 3\}]=])
add_test(NAME reject_missing_dynamic_record_key
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_dynamic_key_missing.lan)
set_tests_properties(reject_missing_dynamic_record_key PROPERTIES WILL_FAIL TRUE)

add_test(NAME reject_mixed_list
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_list_error.lan)
set_tests_properties(reject_mixed_list PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_nested_mixed_list
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/nested_list_type_error.lan)
set_tests_properties(reject_nested_mixed_list PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_runtime_dynamic_mixed_list
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/dynamic_list_type_error.lan)
set_tests_properties(reject_runtime_dynamic_mixed_list PROPERTIES WILL_FAIL TRUE)
add_test(NAME infer_empty_list_element
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_empty_inference.lan)
set_tests_properties(infer_empty_list_element PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\["text"\]]=])
add_test(NAME reject_function_list_element_mismatch
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_function_type_error.lan)
set_tests_properties(reject_function_list_element_mismatch PROPERTIES WILL_FAIL TRUE)
