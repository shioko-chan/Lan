%{
#include "lan/AST.h"
#include <cstdlib>
#include <string>
#include <vector>

int yylex(void);
void yyerror(lan::Program** out, const char* message);
extern int yylineno;
extern std::string lan_parse_error;
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
  std::vector<lan::Parameter>* params;
  std::vector<lan::RecordField>* fields;
}

%token IF ELSE TRUE FALSE MISSING
%token NEWLINE INDENT DEDENT
%token BAD_INDENT
%token <number> NUMBER
%token <text> TEXT IDENTIFIER FUNCTION_NAME
%token EQ NE LE GE AND OR NOT COALESCE

%type <expr> expression coalesce logical_or logical_and equality comparison additive
%type <expr> multiplicative unary postfix primary block suite function_body
%type <exprs> arguments argument_list
%type <params> parameters parameter_list
%type <fields> record_fields record_field_list
%type <parameter> parameter
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
  : FUNCTION_NAME '(' parameters ')' '=' function_body
    { $$ = new lan::Function($1, $3, $6); free($1); }
  ;

function_body
  : expression NEWLINE { $$ = $1; }
  | NEWLINE INDENT suite DEDENT { $$ = $3; }
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

suite
  : expression newlines { $$ = $1; }
  | postfix '=' expression NEWLINE suite
    {
      std::string bindingName;
      lan::Expr* value = $3;
      if (auto* variable = dynamic_cast<lan::VariableExpr*>($1)) {
        bindingName = variable->name;
      } else if (auto* field = dynamic_cast<lan::FieldExpr*>($1)) {
        auto* root = dynamic_cast<lan::VariableExpr*>(field->record.get());
        if (!root) {
          lan_parse_error = "第 " + std::to_string(yylineno) +
                            " 行：字段更新的左边必须是一个名称";
          delete $1;
          delete $3;
          delete $5;
          YYERROR;
        }
        bindingName = root->name;
        value = field->key
          ? static_cast<lan::Expr*>(new lan::RecordUpdateExpr(
                new lan::VariableExpr(bindingName), field->key.release(), $3))
          : static_cast<lan::Expr*>(new lan::RecordUpdateExpr(
                new lan::VariableExpr(bindingName), field->name, $3));
      } else {
        lan_parse_error = "第 " + std::to_string(yylineno) +
                          " 行：等号左边必须是名称或字段";
        delete $1;
        delete $3;
        delete $5;
        YYERROR;
      }
      delete $1;
      $$ = new lan::LetExpr(bindingName, value, $5);
    }
  ;

block
  : expression { $$ = $1; }
  | NEWLINE INDENT suite DEDENT { $$ = $3; }
  ;

expression
  : IF expression ':' block ELSE ':' block
    { $$ = new lan::IfExpr($2, $4, $7); }
  | coalesce { $$ = $1; }
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

postfix
  : postfix '?' { $$ = new lan::PropagateExpr($1); }
  | postfix '.' IDENTIFIER { $$ = new lan::FieldExpr($1, $3); free($3); }
  | postfix '.' '(' expression ')' { $$ = new lan::FieldExpr($1, $4); }
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
  | IDENTIFIER '(' arguments ')' { $$ = new lan::CallExpr($1, $3); free($1); }
  | '(' expression ')' { $$ = $2; }
  ;

record_fields
  : /* empty */ { $$ = new std::vector<lan::RecordField>(); }
  | record_field_list { $$ = $1; }
  ;

record_field_list
  : IDENTIFIER ':' expression
    {
      $$ = new std::vector<lan::RecordField>();
      $$->emplace_back($1, $3);
      free($1);
    }
  | record_field_list ',' IDENTIFIER ':' expression
    {
      $1->emplace_back($3, $5);
      free($3);
      $$ = $1;
    }
  ;

arguments
  : /* empty */ { $$ = new std::vector<lan::Expr*>(); }
  | argument_list { $$ = $1; }
  ;

argument_list
  : expression { $$ = new std::vector<lan::Expr*>(); $$->push_back($1); }
  | argument_list ',' expression { $1->push_back($3); $$ = $1; }
  ;

%%

void yyerror(lan::Program**, const char* message) {
  lan_parse_error = "第 " + std::to_string(yylineno) + " 行：无法理解这里的写法（" +
                    message + "）";
}
