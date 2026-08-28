%option noyywrap yylineno nounput noinput
%x INDENT_SCAN
%{
#include "lan/AST.h"
#include <vector>
#include "parser.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define YY_DECL int yylex_impl(void)
#define YY_USER_INIT BEGIN(INDENT_SCAN);

static std::vector<int> indentation{0};
static int pendingDedents = 0;

static int applyIndentation(int width) {
  if (width > indentation.back()) {
    indentation.push_back(width);
    return INDENT;
  }
  if (width == indentation.back()) return 0;
  int count = 0;
  while (indentation.size() > 1 && width < indentation.back()) {
    indentation.pop_back();
    ++count;
  }
  if (width != indentation.back()) return BAD_INDENT;
  pendingDedents = count - 1;
  return DEDENT;
}

static char* decodeText(const char* source, int length) {
  std::string result;
  for (int i = 1; i < length - 1; ++i) {
    if (source[i] != '\\') {
      result += source[i];
      continue;
    }
    if (++i >= length - 1) break;
    switch (source[i]) {
      case 'n': result += '\n'; break;
      case 't': result += '\t'; break;
      case 'r': result += '\r'; break;
      case '\\': result += '\\'; break;
      case '"': result += '"'; break;
      default: result += source[i]; break;
    }
  }
  return strdup(result.c_str());
}
%}

DIGIT [0-9]
ID [A-Za-z_\200-\377][A-Za-z0-9_\200-\377]*

%%
<INDENT_SCAN>[ ]*("#"|"//")[^\n]*\r?\n  ;
<INDENT_SCAN>[ ]*\r?\n                    ;
<INDENT_SCAN>[ ]+ {
  int token = applyIndentation(yyleng);
  BEGIN(INITIAL);
  if (token) return token;
}
<INDENT_SCAN>[^ \r\n] {
  yyless(0);
  int token = applyIndentation(0);
  BEGIN(INITIAL);
  if (token) return token;
}

<INITIAL>"if"       return IF;
<INITIAL>"else"     return ELSE;
<INITIAL>"true"     return TRUE;
<INITIAL>"false"    return FALSE;
<INITIAL>"missing"  return MISSING;
<INITIAL>"and"      return AND;
<INITIAL>"or"       return OR;
<INITIAL>"not"      return NOT;
<INITIAL>"=="       return EQ;
<INITIAL>"!="       return NE;
<INITIAL>"<="       return LE;
<INITIAL>">="       return GE;
<INITIAL>"&&"       return AND;
<INITIAL>"||"       return OR;
<INITIAL>"??"       return COALESCE;
<INITIAL>{ID}/[ ]*"("[^\n]*")"[ ]*"="[^=] {
  yylval.text = strdup(yytext);
  return FUNCTION_NAME;
}
<INITIAL>{DIGIT}+("."{DIGIT}+)? {
  yylval.number = std::strtod(yytext, nullptr);
  return NUMBER;
}
<INITIAL>"\""([^"\\\n]|\\.)*"\"" {
  yylval.text = decodeText(yytext, yyleng);
  return TEXT;
}
<INITIAL>{ID} {
  yylval.text = strdup(yytext);
  return IDENTIFIER;
}
<INITIAL>("#"|"//")[^\n]* ;
<INITIAL>[ \t\r]+           ;
<INITIAL>\n {
  BEGIN(INDENT_SCAN);
  return NEWLINE;
}
<INITIAL>. return yytext[0];

<INDENT_SCAN,INITIAL><<EOF>> {
  if (indentation.size() > 1) {
    indentation.pop_back();
    return DEDENT;
  }
  return 0;
}
%%

int yylex(void) {
  if (pendingDedents > 0) {
    --pendingDedents;
    return DEDENT;
  }
  return yylex_impl();
}
