%{
#include "lan/AST.h"
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

int yylex(void);
void yyerror(lan::Program** out, const char* message);
extern int yylineno;
extern std::string lan_parse_error;

static constexpr const char* kStreamReceiver = "$lan_stream_receiver";

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
  return new lan::LetExpr(bindingName, value, body);
}

static lan::Expr* makeStreamCall(lan::Expr* receiver, const char* name,
                                 lan::Expr* first = nullptr,
                                 lan::Expr* second = nullptr) {
  auto* arguments = new std::vector<lan::Expr*>();
  arguments->push_back(new lan::VariableExpr(kStreamReceiver));
  if (first) arguments->push_back(first);
  if (second) arguments->push_back(second);
  return new lan::LetExpr(kStreamReceiver, receiver,
                          new lan::CallExpr(name, arguments));
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

%token IF ELSE TRUE FALSE MISSING WITH
%token NEWLINE INDENT DEDENT
%token VIRTUAL_SEMICOLON VIRTUAL_CLOSE
%token BAD_INDENT
%token <number> NUMBER INDEX
%token <text> TEXT IDENTIFIER CALL_IDENTIFIER FUNCTION_NAME
%token EQ NE LE GE AND OR NOT COALESCE
%nonassoc VIRTUAL_OPEN
%nonassoc SUM AVERAGE MIN MAX COUNT
%nonassoc LENGTH ROW_COUNT FIRST LAST FILL_MISSING WHERE SELECT_COLUMNS
%nonassoc TAKE DROP SORT_BY
%nonassoc ADD REPLACE
%nonassoc STREAM_AGGREGATE_END
%nonassoc '{'

%type <expr> expression coalesce logical_or logical_and equality comparison additive
%type <expr> multiplicative unary stream postfix primary block block_items
%type <exprs> arguments argument_list
%type <call_args> call_arguments call_argument_list
%type <params> parameters parameter_list
%type <fields> record_fields record_field_list
%type <parameter> parameter
%type <text> aggregate_name zero_stream_name one_stream_name
%type <text> builtin_name field_name
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
    { $$ = new lan::IfExpr($2, $3, $5); }
  | stream { $$ = $1; }
  ;

coalesce
  : logical_or COALESCE coalesce { $$ = new lan::BinaryExpr("??", $1, $3); }
  | logical_or { $$ = $1; }
  ;

logical_or
  : logical_or OR logical_and { $$ = new lan::BinaryExpr("or", $1, $3); }
  | logical_and { $$ = $1; }
  ;

logical_and
  : logical_and AND equality { $$ = new lan::BinaryExpr("and", $1, $3); }
  | equality { $$ = $1; }
  ;

equality
  : equality EQ comparison { $$ = new lan::BinaryExpr("==", $1, $3); }
  | equality NE comparison { $$ = new lan::BinaryExpr("!=", $1, $3); }
  | comparison { $$ = $1; }
  ;

comparison
  : comparison '<' additive { $$ = new lan::BinaryExpr("<", $1, $3); }
  | comparison '>' additive { $$ = new lan::BinaryExpr(">", $1, $3); }
  | comparison LE additive { $$ = new lan::BinaryExpr("<=", $1, $3); }
  | comparison GE additive { $$ = new lan::BinaryExpr(">=", $1, $3); }
  | additive { $$ = $1; }
  ;

additive
  : additive '+' multiplicative { $$ = new lan::BinaryExpr("+", $1, $3); }
  | additive '-' multiplicative { $$ = new lan::BinaryExpr("-", $1, $3); }
  | multiplicative { $$ = $1; }
  ;

multiplicative
  : multiplicative '*' unary { $$ = new lan::BinaryExpr("*", $1, $3); }
  | multiplicative '/' unary { $$ = new lan::BinaryExpr("/", $1, $3); }
  | unary { $$ = $1; }
  ;

unary
  : '-' unary { $$ = new lan::UnaryExpr('-', $2); }
  | '!' unary { $$ = new lan::UnaryExpr('!', $2); }
  | NOT unary { $$ = new lan::UnaryExpr('!', $2); }
  | postfix { $$ = $1; }
  ;

stream
  : coalesce { $$ = $1; }
  | stream aggregate_name %prec STREAM_AGGREGATE_END
    { $$ = makeStreamCall($1, $2); free($2); }
  | stream aggregate_name coalesce
    { $$ = makeStreamCall($1, $2, $3); free($2); }
  | stream zero_stream_name
    { $$ = makeStreamCall($1, $2); free($2); }
  | stream one_stream_name coalesce
    { $$ = makeStreamCall($1, $2, $3); free($2); }
  | stream ADD field_name ':' coalesce
    {
      $$ = makeStreamCall($1, "add_column", new lan::TextExpr($3), $5);
      free($3);
    }
  | stream REPLACE field_name ':' coalesce
    {
      $$ = makeStreamCall($1, "replace_column", new lan::TextExpr($3), $5);
      free($3);
    }
  ;

postfix
  : postfix '?' { $$ = new lan::PropagateExpr($1); }
  | postfix '.' field_name { $$ = new lan::FieldExpr($1, $3); free($3); }
  | postfix INDEX { $$ = new lan::FieldExpr($1, new lan::NumberExpr($2)); }
  | postfix '.' '(' expression ')' { $$ = new lan::FieldExpr($1, $4); }
  | postfix WITH '{' record_fields '}' { $$ = makeRecordWith($1, $4); }
  | primary { $$ = $1; }
  ;

primary
  : NUMBER { $$ = new lan::NumberExpr($1); }
  | TEXT { $$ = new lan::TextExpr($1); free($1); }
  | MISSING { $$ = new lan::MissingExpr(); }
  | '[' arguments ']' { $$ = new lan::ListExpr($2); }
  | '{' record_fields '}' { $$ = new lan::RecordExpr($2); }
  | TRUE { $$ = new lan::BooleanExpr(true); }
  | FALSE { $$ = new lan::BooleanExpr(false); }
  | IDENTIFIER { $$ = new lan::VariableExpr($1); free($1); }
  | '.' field_name
    {
      $$ = new lan::FieldExpr(new lan::VariableExpr(kStreamReceiver), $2);
      free($2);
    }
  | CALL_IDENTIFIER '(' call_arguments ')'
    { $$ = new lan::CallExpr($1, $3); free($1); }
  | builtin_name '(' call_arguments ')'
    { $$ = new lan::CallExpr($1, $3); free($1); }
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
  | builtin_name { $$ = $1; }
  | ADD { $$ = strdup("add"); }
  | REPLACE { $$ = strdup("replace"); }
  | WITH { $$ = strdup("with"); }
  ;

builtin_name
  : aggregate_name { $$ = $1; }
  | zero_stream_name { $$ = $1; }
  | one_stream_name { $$ = $1; }
  ;

aggregate_name
  : SUM { $$ = strdup("sum"); }
  | AVERAGE { $$ = strdup("average"); }
  | MIN { $$ = strdup("min"); }
  | MAX { $$ = strdup("max"); }
  | COUNT { $$ = strdup("count"); }
  ;

zero_stream_name
  : LENGTH { $$ = strdup("length"); }
  | ROW_COUNT { $$ = strdup("row_count"); }
  | FIRST { $$ = strdup("first"); }
  | LAST { $$ = strdup("last"); }
  ;

one_stream_name
  : FILL_MISSING { $$ = strdup("fill_missing"); }
  | WHERE { $$ = strdup("where"); }
  | SELECT_COLUMNS { $$ = strdup("select_columns"); }
  | TAKE { $$ = strdup("take"); }
  | DROP { $$ = strdup("drop"); }
  | SORT_BY { $$ = strdup("sort_by"); }
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
  ;

%%

void yyerror(lan::Program**, const char* message) {
  lan_parse_error = "第 " + std::to_string(yylineno) + " 行：无法理解这里的写法（" +
                    message + "）";
}
