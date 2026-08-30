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
target_compile_definitions(lanc PRIVATE LAN_CLANG_PATH="${CLANG_EXECUTABLE}")
llvm_map_components_to_libnames(LLVM_LIBS Core Support Passes BitReader Linker)
target_link_libraries(lanc PRIVATE ${LLVM_LIBS})

enable_testing()
add_test(NAME collection_pipeline_semantics
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/collection_pipeline.lan)
set_tests_properties(collection_pipeline_semantics PROPERTIES
  PASS_REGULAR_EXPRESSION [=[all_positive: false.*filtered: \[1, 3\].*found: 3.*has_large: true.*mapped: \[2, 0, -4, 6\].*names: \["Alice", "Bob", "Carol"\]]=])
add_test(NAME boolean_three_value_logic
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/three_value_logic.lan)
set_tests_properties(boolean_three_value_logic PROPERTIES
  PASS_REGULAR_EXPRESSION [=[missing_and_false: false.*missing_or_true: true.*refined: 5]=])
foreach(error_case
    normal_function_broadcast
    element_binding_outside
    pipeline_parentheses
    old_stream_syntax
    table_shape_type)
  add_test(NAME reject_${error_case}
    COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/${error_case}_error.lan)
  set_tests_properties(reject_${error_case} PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME reject_require_missing
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/require_missing_error.lan)
set_tests_properties(reject_require_missing PROPERTIES WILL_FAIL TRUE)
add_test(NAME table_lazy_row_lowering
  COMMAND ${CMAKE_COMMAND}
          -DLANC=$<TARGET_FILE:lanc>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/table_lowering.lan
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/table_lowering.ll
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_table_lowering.cmake)
add_test(NAME table_shape_call_intersection
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_shape_intersection.lan)
set_tests_properties(table_shape_call_intersection PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\[\["Alice"\], \[missing\]\]]=])
add_test(NAME table_map_table_fusion
  COMMAND ${CMAKE_COMMAND}
          -DLANC=$<TARGET_FILE:lanc>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/table_map_table_fusion.lan
          -DFALLBACK_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/table_fusion_fallback.lan
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/table_map_table_fusion.ll
          -DFALLBACK_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/table_fusion_fallback.ll
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_table_fusion.cmake)
add_test(NAME compact_list_abi
  COMMAND ${CMAKE_COMMAND}
          -DLANC=$<TARGET_FILE:lanc>
          -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/compact_list_abi.lan
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/compact_list_abi.ll
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_compact_list_abi.cmake)
add_test(NAME confirmed_new_syntax
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/confirmed_syntax.lan)
set_tests_properties(confirmed_new_syntax PROPERTIES
  PASS_REGULAR_EXPRESSION [=[empty_first: missing.*last: 5.*updated_age: 21]=])
foreach(error_case
    named_after_named
    named_duplicate
    named_unknown
    named_missing
    stream_receiver_outside
    record_dynamic_key_type
    record_temporary_update
    record_self_update)
  add_test(NAME reject_${error_case}
    COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/${error_case}_error.lan)
  set_tests_properties(reject_${error_case} PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME reject_record_nested_intermediate_at_runtime
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_nested_intermediate_error.lan)
set_tests_properties(reject_record_nested_intermediate_at_runtime PROPERTIES WILL_FAIL TRUE)
add_test(NAME build_native_executable
  COMMAND lanc --build ${CMAKE_CURRENT_SOURCE_DIR}/tests/simple.lan)
set_tests_properties(build_native_executable PROPERTIES
  PASS_REGULAR_EXPRESSION [=[Lan：已生成 .*\.lan/simple/simple]=])
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

add_test(NAME list_aggregates
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_aggregates.lan)
set_tests_properties(list_aggregates PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{all_missing_min: -1, average: 15, count: 2, empty_average: -1, empty_count: 0, empty_sum: 0, max: 20, min: 10, sum: 30\}]=])
add_test(NAME reject_text_list_numeric_aggregate
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_numeric_aggregate_type_error.lan)
set_tests_properties(reject_text_list_numeric_aggregate PROPERTIES WILL_FAIL TRUE)
add_test(NAME stream_aggregate_syntax
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/stream_aggregates.lan)
set_tests_properties(stream_aggregate_syntax PROPERTIES PASS_REGULAR_EXPRESSION
  [=[aggregate_named_field: \[1, 2, 3\].*list_average: 15.*list_count: 2.*list_max: 20.*list_min: 10.*list_sum: 30.*table_average: 15.*table_count: 2.*table_max: 20.*table_min: 10.*table_sum: 30]=])
add_test(NAME stream_transformation_syntax
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/stream_transformations.lan)
set_tests_properties(stream_transformation_syntax PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{dropped_rows: 1, first_name: "C", first_score: 71, length: 3, rows: 1\}]=])
add_test(NAME contextual_stream_names
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/contextual_stream_names.lan)
set_tests_properties(contextual_stream_names PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{aggregate: 2, fields: 18, parameters: 21\}]=])
add_test(NAME layout_stream_continuation
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/layout_stream_continuation.lan)
set_tests_properties(layout_stream_continuation PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{adults: \[true, true\], names: \["C", "A"\], rows: 2\}]=])
add_test(NAME list_zip_uses_shorter_length
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_shorter_zip.lan)
set_tests_properties(list_zip_uses_shorter_length PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{add: \[11, 22\], comparison: \[false, true\], reversed: \[9, 18\]\}]=])
add_test(NAME safe_list_index
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_index.lan)
set_tests_properties(safe_list_index PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{dynamic: 20, fixed: 10, fractional: -1, negative: -1, nested: 3, past_end: -1, stored_missing: -1, table_text: "A"\}]=])
add_test(NAME reject_text_list_index
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_text_index_error.lan)
set_tests_properties(reject_text_list_index PROPERTIES WILL_FAIL TRUE)
add_test(NAME list_broadcast_operators
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_broadcast_operators.lan)
set_tests_properties(list_broadcast_operators PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{and_false: \[false, false, false\], and_true: \[true, false, missing\], default_column: \[1, 2, 3\], default_scalar: \[1, 0, 3\], negated: \[false, true, missing\], numbers: \[-1, missing, 2\], or_false: \[true, false, missing\], or_true: \[true, true, true\], text_columns: \["A1", "B2"\], text_scalar: \["A!", missing, "C!"\]\}]=])
add_test(NAME reject_mixed_text_column_concat
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/list_text_concat_type_error.lan)
set_tests_properties(reject_mixed_text_column_concat PROPERTIES WILL_FAIL TRUE)
add_test(NAME nested_list_broadcast
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/nested_list_broadcast.lan)
set_tests_properties(nested_list_broadcast PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{add: \[\[2, 4\], \[4, 6\]\], compare: \[\[true, true\], \[false, false\]\], defaults: \[\[1, 0\], \[0, 4\]\], logic: \[\[true, false\], \[false, false\]\], negative: \[\[-1, -2\], \[-3, -4\]\], ragged: \[\[11, 22\], \[34\]\], reverse_compare: \[\[true, true\], \[false, false\]\], text: \[\["A!", "B\?"\], \["C!", "D\?"\]\]\}]=])
add_test(NAME structural_same
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/structural_same.lan)
set_tests_properties(structural_same PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{different_shape: false, different_types: false, list_length: false, lists: true, missing_values: true, nested: true, numbers: true, one_missing: false, record_keys: false, records: true, table_record: true\}]=])

add_test(NAME table_column_operations
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/examples/table.lan)
set_tests_properties(table_column_operations PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{age_max: 20, age_min: 17, age_total: 37, rows: 3, score_average: 85, scored: 2\}]=])
add_test(NAME table_empty_aggregates
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_empty_aggregate.lan)
set_tests_properties(table_empty_aggregates PROPERTIES PASS_REGULAR_EXPRESSION
  [=[\{average: -1, count: 0, sum: 0\}]=])
add_test(NAME reject_table_unequal_columns
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_invalid_length.lan)
set_tests_properties(reject_table_unequal_columns PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_table_non_list_column
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_invalid_column.lan)
set_tests_properties(reject_table_non_list_column PROPERTIES WILL_FAIL TRUE)
add_test(NAME table_operation_requires_validation
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_requires_validation.lan)
set_tests_properties(table_operation_requires_validation PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_non_numeric_table_aggregate
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_numeric_column_error.lan)
set_tests_properties(reject_non_numeric_table_aggregate PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_missing_table_column
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_missing_column.lan)
set_tests_properties(reject_missing_table_column PROPERTIES WILL_FAIL TRUE)

add_test(NAME table_transformations
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_transformations.lan)
set_tests_properties(table_transformations PROPERTIES PASS_REGULAR_EXPRESSION
  [=[arithmetic: \[85, 0, 95, 75\].*comparison: \[true, false, true, false\].*first_two: \{age: \[20, 17\], name: \["B", "A"\], score: \[80, missing\]\}.*list_arithmetic: \[40, 34, 40, 60\].*scalar_left: \[20, missing, 10, 30\].*text_comparison: \[false, true, false, false\]]=])
add_test(NAME reject_table_column_wrong_length
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_column_length_error.lan)
set_tests_properties(reject_table_column_wrong_length PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_adding_existing_table_column
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_add_existing_error.lan)
set_tests_properties(reject_adding_existing_table_column PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_replacing_missing_table_column
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_replace_missing_error.lan)
set_tests_properties(reject_replacing_missing_table_column PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_non_boolean_where_mask
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_where_type_error.lan)
set_tests_properties(reject_non_boolean_where_mask PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_empty_column_selection
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_empty_selection_error.lan)
set_tests_properties(reject_empty_column_selection PROPERTIES WILL_FAIL TRUE)
add_test(NAME reject_fractional_table_take
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_fractional_take_error.lan)
set_tests_properties(reject_fractional_table_take PROPERTIES WILL_FAIL TRUE)

add_test(NAME brace_semicolon_blocks
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_semicolon.lan)
set_tests_properties(brace_semicolon_blocks PROPERTIES PASS_REGULAR_EXPRESSION "80")
add_test(NAME brace_inline_blocks
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_inline.lan)
set_tests_properties(brace_inline_blocks PROPERTIES PASS_REGULAR_EXPRESSION "12")
add_test(NAME brace_top_level_scope
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_top_level.lan)
set_tests_properties(brace_top_level_scope PROPERTIES
  PASS_REGULAR_EXPRESSION [=[\{result: 21\}]=])
add_test(NAME reject_missing_semicolon_in_brace
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/brace_missing_semicolon_error.lan)
set_tests_properties(reject_missing_semicolon_in_brace PROPERTIES WILL_FAIL TRUE)
add_test(NAME mix_indentation_and_braces
  COMMAND lanc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_block_styles.lan)
set_tests_properties(mix_indentation_and_braces PROPERTIES PASS_REGULAR_EXPRESSION "21")
add_test(NAME reject_colon_after_if_else
  COMMAND lanc ${CMAKE_CURRENT_SOURCE_DIR}/tests/colon_after_if_error.lan)
set_tests_properties(reject_colon_after_if_else PROPERTIES WILL_FAIL TRUE)
