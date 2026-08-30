%{
#include "lan/AST.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int yylex(void);
void yyerror(lan::Program** out, const char* message);
extern int yylineno;
extern std::string lan_parse_error;

static constexpr const char* kCurrentElement = "$lan_current_element";

static lan::Expr* located(lan::Expr* expression, int line, int column) {
  expression->line = line;
  expression->column = column;
  return expression;
}

static void collectRecordPath(lan::Expr* target, std::string& rootName,
                              std::vector<lan::RecordPathPart>& path) {
  auto* field = dynamic_cast<lan::FieldExpr*>(target);
  if (!field) {
    auto* root = dynamic_cast<lan::VariableExpr*>(target);
    if (root) rootName = root->name;
    delete target;
    return;
  }
  lan::Expr* parent = field->record.release();
  if (field->key) path.emplace_back(field->key.release());
  else path.emplace_back(field->name);
  delete field;
  collectRecordPath(parent, rootName, path);
}

static lan::Expr* makeBinding(lan::Expr* target, lan::Expr* expression,
                              lan::Expr* body) {
  std::string bindingName;
  lan::Expr* value = expression;
  if (auto* variable = dynamic_cast<lan::VariableExpr*>(target)) {
    bindingName = variable->name;
  } else if (auto* field = dynamic_cast<lan::FieldExpr*>(target)) {
    std::vector<lan::RecordPathPart> reversed;
    collectRecordPath(target, bindingName, reversed);
    target = nullptr;
    if (bindingName.empty()) {
      lan_parse_error = "第 " + std::to_string(yylineno) +
                        " 行：字段更新必须以局部名称为根，不能更新临时值";
      delete expression;
      delete body;
      return nullptr;
    }
    if (auto* rhs = dynamic_cast<lan::VariableExpr*>(expression);
        rhs && rhs->name == bindingName) {
      lan_parse_error = "第 " + std::to_string(yylineno) +
                        " 行：不能把“" + bindingName +
                        "”自身写入它的字段";
      delete expression;
      delete body;
      return nullptr;
    }
    std::reverse(reversed.begin(), reversed.end());
    value = new lan::NestedRecordUpdateExpr(
        new lan::VariableExpr(bindingName), std::move(reversed), expression);
  } else {
    lan_parse_error = "第 " + std::to_string(yylineno) +
                      " 行：等号左边必须是名称或字段";
    delete target;
    delete expression;
    delete body;
    return nullptr;
  }
  delete target;
  auto* result = new lan::LetExpr(bindingName, value, body);
  result->line = value->line;
  result->column = value->column;
  return result;
}

static lan::Expr* makePipelineStage(lan::Expr* receiver, const char* name,
                                    std::vector<lan::CallArgument>* arguments) {
  auto operation = lan::CollectionOperation::Map;
  bool isCollection = true;
  if (std::strcmp(name, "map") == 0) operation = lan::CollectionOperation::Map;
  else if (std::strcmp(name, "filter") == 0) operation = lan::CollectionOperation::Filter;
  else if (std::strcmp(name, "select") == 0) operation = lan::CollectionOperation::Select;
  else if (std::strcmp(name, "add") == 0) operation = lan::CollectionOperation::Add;
  else if (std::strcmp(name, "replace") == 0) operation = lan::CollectionOperation::Replace;
  else if (std::strcmp(name, "find") == 0) operation = lan::CollectionOperation::Find;
  else if (std::strcmp(name, "any") == 0) operation = lan::CollectionOperation::Any;
  else if (std::strcmp(name, "all") == 0) operation = lan::CollectionOperation::All;
  else isCollection = false;

  if (!isCollection) {
    arguments->insert(arguments->begin(), lan::CallArgument(receiver));
    auto* result = new lan::CallExpr(name, arguments);
    result->line = receiver->line;
    result->column = receiver->column;
    return result;
  }
  if (arguments->size() != 1 || !arguments->front().name.empty()) {
    lan_parse_error = "第 " + std::to_string(yylineno) + " 行：集合操作“" +
                      name + "”需要一个元素表达式";
    delete receiver;
    delete arguments;
    return nullptr;
  }
  lan::Expr* body = arguments->front().value.release();
  std::string binding;
  if (auto* elementBinding = dynamic_cast<lan::ElementBindingExpr*>(body)) {
    binding = elementBinding->name;
    body = elementBinding->body.release();
    delete elementBinding;
  }
  delete arguments;
  auto* result = new lan::CollectionExpr(operation, receiver, body, binding);
  result->line = receiver->line;
  result->column = receiver->column;
  return result;
}

static lan::Expr* makeRecordWith(lan::Expr* record,
                                 std::vector<lan::RecordField>* fields) {
  lan::Expr* result = record;
  for (auto& field : *fields) {
    result = field.key
        ? static_cast<lan::Expr*>(new lan::RecordUpdateExpr(
              result, field.key.release(), field.value.release()))
        : static_cast<lan::Expr*>(new lan::RecordUpdateExpr(
              result, field.name, field.value.release()));
  }
  delete fields;
  return result;
}
%}

%parse-param { lan::Program** out }
%locations

%union {
  double number;
  char* text;
  lan::Expr* expr;
  lan::Function* function;
  lan::Parameter* parameter;
  lan::Program* program;
  std::vector<lan::Expr*>* exprs;
  std::vector<lan::CallArgument>* call_args;
  std::vector<lan::Parameter>* params;
  std::vector<lan::RecordField>* fields;
}

%token IF ELSE TRUE FALSE MISSING WITH PIPE_ARROW FAT_ARROW
%token NEWLINE INDENT DEDENT
%token VIRTUAL_SEMICOLON VIRTUAL_CLOSE
%token BAD_INDENT
%token <number> NUMBER INDEX
%token <text> TEXT IDENTIFIER CALL_IDENTIFIER FUNCTION_NAME
%token EQ NE LE GE AND OR NOT COALESCE
%nonassoc VIRTUAL_OPEN
%nonassoc '{'

%type <expr> expression coalesce logical_or logical_and equality comparison additive
%type <expr> multiplicative unary pipeline postfix primary block block_items
%type <exprs> arguments argument_list
%type <call_args> call_arguments call_argument_list
%type <params> parameters parameter_list
%type <fields> record_fields record_field_list
%type <parameter> parameter
%type <text> field_name
%type <function> function
%type <program> functions

%start program

%%

program
  : newlines functions expression newlines
    { $2->result.reset($3); *out = $2; }
  ;

newlines
  : /* empty */
  | newlines NEWLINE
  ;

functions
  : /* empty */ { $$ = new lan::Program(); }
  | functions function { $1->functions.emplace_back($2); $$ = $1; }
  ;

function
  : FUNCTION_NAME '(' parameters ')' '=' expression NEWLINE
    { $$ = new lan::Function($1, $3, $6); free($1); }
  ;

parameters
  : /* empty */ { $$ = new std::vector<lan::Parameter>(); }
  | parameter_list { $$ = $1; }
  ;

parameter_list
  : parameter
    { $$ = new std::vector<lan::Parameter>(); $$->emplace_back(std::move(*$1)); delete $1; }
  | parameter_list ',' parameter
    { $1->emplace_back(std::move(*$3)); delete $3; $$ = $1; }
  ;

parameter
  : IDENTIFIER { $$ = new lan::Parameter($1); free($1); }
  | IDENTIFIER '=' expression { $$ = new lan::Parameter($1, $3); free($1); }
  ;

block
  : '{' block_items '}' { $$ = $2; }
  | VIRTUAL_OPEN block_items VIRTUAL_CLOSE { $$ = $2; }
  ;

block_items
  : expression { $$ = $1; }
  | expression ';' { $$ = $1; }
  | postfix '=' expression ';' block_items
    {
      $$ = makeBinding($1, $3, $5);
      if (!$$) YYERROR;
    }
  | postfix '=' expression VIRTUAL_SEMICOLON block_items
    {
      $$ = makeBinding($1, $3, $5);
      if (!$$) YYERROR;
    }
  ;

expression
  : IF expression block ELSE block
    { $$ = located(new lan::IfExpr($2, $3, $5), @1.first_line, @1.first_column); }
  | pipeline { $$ = $1; }
  ;

coalesce
  : logical_or COALESCE coalesce { $$ = located(new lan::BinaryExpr("??", $1, $3), @2.first_line, @2.first_column); }
  | logical_or { $$ = $1; }
  ;

logical_or
  : logical_or OR logical_and { $$ = located(new lan::BinaryExpr("or", $1, $3), @2.first_line, @2.first_column); }
  | logical_and { $$ = $1; }
  ;

logical_and
  : logical_and AND equality { $$ = located(new lan::BinaryExpr("and", $1, $3), @2.first_line, @2.first_column); }
  | equality { $$ = $1; }
  ;

equality
  : equality EQ comparison { $$ = located(new lan::BinaryExpr("==", $1, $3), @2.first_line, @2.first_column); }
  | equality NE comparison { $$ = located(new lan::BinaryExpr("!=", $1, $3), @2.first_line, @2.first_column); }
  | comparison { $$ = $1; }
  ;

comparison
  : comparison '<' additive { $$ = located(new lan::BinaryExpr("<", $1, $3), @2.first_line, @2.first_column); }
  | comparison '>' additive { $$ = located(new lan::BinaryExpr(">", $1, $3), @2.first_line, @2.first_column); }
  | comparison LE additive { $$ = located(new lan::BinaryExpr("<=", $1, $3), @2.first_line, @2.first_column); }
  | comparison GE additive { $$ = located(new lan::BinaryExpr(">=", $1, $3), @2.first_line, @2.first_column); }
  | additive { $$ = $1; }
  ;

additive
  : additive '+' multiplicative { $$ = located(new lan::BinaryExpr("+", $1, $3), @2.first_line, @2.first_column); }
  | additive '-' multiplicative { $$ = located(new lan::BinaryExpr("-", $1, $3), @2.first_line, @2.first_column); }
  | multiplicative { $$ = $1; }
  ;

multiplicative
  : multiplicative '*' unary { $$ = located(new lan::BinaryExpr("*", $1, $3), @2.first_line, @2.first_column); }
  | multiplicative '/' unary { $$ = located(new lan::BinaryExpr("/", $1, $3), @2.first_line, @2.first_column); }
  | multiplicative '%' unary { $$ = located(new lan::BinaryExpr("%", $1, $3), @2.first_line, @2.first_column); }
  | unary { $$ = $1; }
  ;

unary
  : '-' unary { $$ = located(new lan::UnaryExpr('-', $2), @1.first_line, @1.first_column); }
  | '!' unary { $$ = located(new lan::UnaryExpr('!', $2), @1.first_line, @1.first_column); }
  | NOT unary { $$ = located(new lan::UnaryExpr('!', $2), @1.first_line, @1.first_column); }
  | postfix { $$ = $1; }
  ;

pipeline
  : coalesce { $$ = $1; }
  | pipeline PIPE_ARROW CALL_IDENTIFIER '(' call_arguments ')'
    {
      $$ = makePipelineStage($1, $3, $5);
      free($3);
      if (!$$) YYERROR;
    }
  ;

postfix
  : postfix '?' { $$ = located(new lan::PropagateExpr($1), @2.first_line, @2.first_column); }
  | postfix '.' field_name { $$ = located(new lan::FieldExpr($1, $3), @2.first_line, @2.first_column); free($3); }
  | postfix INDEX { $$ = located(new lan::FieldExpr($1, located(new lan::NumberExpr($2), @2.first_line, @2.first_column)), @2.first_line, @2.first_column); }
  | postfix '.' '(' expression ')' { $$ = located(new lan::FieldExpr($1, $4), @2.first_line, @2.first_column); }
  | postfix WITH '{' record_fields '}' { $$ = makeRecordWith($1, $4); }
  | primary { $$ = $1; }
  ;

primary
  : NUMBER { $$ = located(new lan::NumberExpr($1), @1.first_line, @1.first_column); }
  | TEXT { $$ = located(new lan::TextExpr($1), @1.first_line, @1.first_column); free($1); }
  | MISSING { $$ = located(new lan::MissingExpr(), @1.first_line, @1.first_column); }
  | '[' arguments ']' { $$ = located(new lan::ListExpr($2), @1.first_line, @1.first_column); }
  | '{' record_fields '}' { $$ = located(new lan::RecordExpr($2), @1.first_line, @1.first_column); }
  | TRUE { $$ = located(new lan::BooleanExpr(true), @1.first_line, @1.first_column); }
  | FALSE { $$ = located(new lan::BooleanExpr(false), @1.first_line, @1.first_column); }
  | IDENTIFIER { $$ = located(new lan::VariableExpr($1), @1.first_line, @1.first_column); free($1); }
  | '.' { $$ = located(new lan::VariableExpr(kCurrentElement), @1.first_line, @1.first_column); }
  | '.' field_name
    {
      $$ = located(new lan::FieldExpr(
          located(new lan::VariableExpr(kCurrentElement), @1.first_line, @1.first_column),
          $2), @1.first_line, @1.first_column);
      free($2);
    }
  | '.' '(' expression ')'
    { $$ = located(new lan::FieldExpr(
          located(new lan::VariableExpr(kCurrentElement), @1.first_line, @1.first_column),
          $3), @1.first_line, @1.first_column); }
  | CALL_IDENTIFIER '(' call_arguments ')'
    { $$ = located(new lan::CallExpr($1, $3), @1.first_line, @1.first_column); free($1); }
  | '(' expression ')' { $$ = $2; }
  | block { $$ = $1; }
  ;

record_fields
  : /* empty */ { $$ = new std::vector<lan::RecordField>(); }
  | record_field_list { $$ = $1; }
  ;

record_field_list
  : field_name ':' expression
    {
      $$ = new std::vector<lan::RecordField>();
      $$->emplace_back($1, $3);
      free($1);
    }
  | record_field_list ',' field_name ':' expression
    {
      $1->emplace_back($3, $5);
      free($3);
      $$ = $1;
    }
  | '(' expression ')' ':' expression
    {
      $$ = new std::vector<lan::RecordField>();
      $$->emplace_back($2, $5);
    }
  | record_field_list ',' '(' expression ')' ':' expression
    {
      $1->emplace_back($4, $7);
      $$ = $1;
    }
  ;

field_name
  : IDENTIFIER { $$ = $1; }
  ;

arguments
  : /* empty */ { $$ = new std::vector<lan::Expr*>(); }
  | argument_list { $$ = $1; }
  ;

argument_list
  : expression { $$ = new std::vector<lan::Expr*>(); $$->push_back($1); }
  | argument_list ',' expression { $1->push_back($3); $$ = $1; }
  ;

call_arguments
  : /* empty */ { $$ = new std::vector<lan::CallArgument>(); }
  | call_argument_list { $$ = $1; }
  ;

call_argument_list
  : expression { $$ = new std::vector<lan::CallArgument>(); $$->emplace_back($1); }
  | call_argument_list ',' expression { $1->emplace_back($3); $$ = $1; }
  | IDENTIFIER '=' expression
    {
      $$ = new std::vector<lan::CallArgument>();
      $$->emplace_back($3, $1);
      free($1);
    }
  | call_argument_list ',' IDENTIFIER '=' expression
    {
      $1->emplace_back($5, $3);
      free($3);
      $$ = $1;
    }
  | IDENTIFIER FAT_ARROW expression
    {
      $$ = new std::vector<lan::CallArgument>();
      $$->emplace_back(new lan::ElementBindingExpr($1, $3));
      free($1);
    }
  ;

%%

void yyerror(lan::Program**, const char* message) {
  lan_parse_error = "第 " + std::to_string(yylineno) + " 行：无法理解这里的写法（" +
                    message + "）";
}
