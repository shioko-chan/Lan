%option noyywrap yylineno nounput noinput
%x INDENT_SCAN
%{
#include "lan/AST.h"
#include <vector>
#include "parser.hpp"
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#define YY_DECL int yylex_impl(void)
#define YY_USER_INIT BEGIN(INDENT_SCAN);

static int sourceLine = 1;
static int sourceColumn = 1;
#define YY_USER_ACTION do { \
  yylloc.first_line = sourceLine; \
  yylloc.first_column = sourceColumn; \
  for (int locationIndex = 0; locationIndex < yyleng; ++locationIndex) { \
    if (yytext[locationIndex] == '\n') { ++sourceLine; sourceColumn = 1; } \
    else { ++sourceColumn; } \
  } \
  yylloc.last_line = sourceLine; \
  yylloc.last_column = sourceColumn; \
} while (0);

static std::vector<int> indentation{0};
static int pendingDedents = 0;
static int braceDepth = 0;
static int rawIndentDepth = 0;
static std::vector<int> layoutDepths;
static int lastSourceToken = 0;
static int explicitDepth = 0;
static std::vector<int> pendingHeaders;

static bool endsExpression(int token) {
  return token == IDENTIFIER || token == NUMBER || token == INDEX || token == TEXT ||
         token == TRUE || token == FALSE || token == MISSING || token == ')' ||
         token == ']' || token == '}' || token == '?' || token == '.';
}

struct BufferedToken {
  int token;
  YYSTYPE value;
  YYLTYPE location;
};

static std::deque<BufferedToken> bufferedTokens;

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
<INITIAL>"match"    return MATCH;
<INITIAL>"match"/[ ]*":" { yylval.text = strdup(yytext); return IDENTIFIER; }
<INITIAL>"else"     return ELSE;
<INITIAL>"true"     return TRUE;
<INITIAL>"false"    return FALSE;
<INITIAL>"missing"  return MISSING;
<INITIAL>"io"       return IO_KEYWORD;
<INITIAL>"do"       return DO_KEYWORD;
<INITIAL>"do"/[ ]*":" { yylval.text = strdup(yytext); return IDENTIFIER; }
<INITIAL>"<-"       return LEFT_ARROW;
<INITIAL>"with"     return WITH;
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
<INITIAL>"|>"       return PIPE_ARROW;
<INITIAL>"=>"       return FAT_ARROW;
<INITIAL>"if"/[ ]*"("       return IF;
<INITIAL>"match"/[ ]*"("    return MATCH;
<INITIAL>"else"/[ ]*"("     return ELSE;
<INITIAL>"true"/[ ]*"("     return TRUE;
<INITIAL>"false"/[ ]*"("    return FALSE;
<INITIAL>"missing"/[ ]*"("  return MISSING;
<INITIAL>"io"/[ ]*"("       return IO_KEYWORD;
<INITIAL>"do"/[ ]*"("       return DO_KEYWORD;
<INITIAL>"with"/[ ]*"("     return WITH;
<INITIAL>"and"/[ ]*"("      return AND;
<INITIAL>"or"/[ ]*"("       return OR;
<INITIAL>"not"/[ ]*"("      return NOT;
<INITIAL>{ID}/[ ]*"("[^\n]*")"[ ]*"="[^=>] {
  if (std::strcmp(yytext, "io") == 0) return IO_KEYWORD;
  if (std::strcmp(yytext, "do") == 0) return DO_KEYWORD;
  if (std::strcmp(yytext, "match") == 0) return MATCH;
  yylval.text = strdup(yytext);
  return FUNCTION_NAME;
}
<INITIAL>{ID}/[ ]*"(" {
  yylval.text = strdup(yytext);
  return CALL_IDENTIFIER;
}
<INITIAL>"."{DIGIT}+ {
  yylval.number = std::strtod(yytext + 1, nullptr);
  return INDEX;
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
<INITIAL>"{" { ++braceDepth; return '{'; }
<INITIAL>"}" { if (braceDepth > 0) --braceDepth; return '}'; }
<INITIAL>\n {
  if (braceDepth > 0) {
    BEGIN(INITIAL);
    continue;
  }
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
  auto nextRawToken = []() {
    if (pendingDedents > 0) {
      --pendingDedents;
      return DEDENT;
    }
    return yylex_impl();
  };

  auto bufferCurrent = [](int token) {
    BufferedToken buffered{};
    buffered.token = token;
    buffered.value = yylval;
    buffered.location = yylloc;
    bufferedTokens.push_back(buffered);
  };

  while (bufferedTokens.empty()) {
    int token = nextRawToken();

    if (token == NEWLINE) {
      bool headerLayout = !pendingHeaders.empty() && pendingHeaders.back() == explicitDepth;
      bool opensLayout = lastSourceToken == '=' || lastSourceToken == ELSE ||
                         lastSourceToken == IO_KEYWORD || lastSourceToken == DO_KEYWORD ||
                         lastSourceToken == LEFT_ARROW ||
                         lastSourceToken == FAT_ARROW ||
                         headerLayout;
      if (headerLayout) pendingHeaders.pop_back();

      int next = nextRawToken();
      while (next == INDENT || next == DEDENT) {
        rawIndentDepth += next == INDENT ? 1 : -1;
        next = nextRawToken();
      }
      if (next == BAD_INDENT) return BAD_INDENT;

      YYSTYPE nextValue = yylval;
      YYLTYPE nextLocation = yylloc;
      std::deque<BufferedToken> lineTokens;
      if (opensLayout) {
        int enclosing = layoutDepths.empty() ? 0 : layoutDepths.back();
        if (rawIndentDepth <= enclosing || next == 0) return BAD_INDENT;
        layoutDepths.push_back(rawIndentDepth);
        lineTokens.push_back({VIRTUAL_OPEN, {}, nextLocation});
      } else {
        while (!layoutDepths.empty() && rawIndentDepth < layoutDepths.back()) {
          layoutDepths.pop_back();
          lineTokens.push_back({VIRTUAL_CLOSE, {}, nextLocation});
        }
        if (next != ELSE) {
          if (!layoutDepths.empty() && rawIndentDepth == layoutDepths.back() &&
              next != 0)
            lineTokens.push_back({VIRTUAL_SEMICOLON, {}, nextLocation});
          else if (layoutDepths.empty())
            lineTokens.push_back({NEWLINE, {}, nextLocation});
        }
      }
      if (next != 0) lineTokens.push_back({next, nextValue, nextLocation});
      else {
        while (!layoutDepths.empty()) {
          layoutDepths.pop_back();
          lineTokens.push_back({VIRTUAL_CLOSE, {}, nextLocation});
        }
        lineTokens.push_back({0, {}, nextLocation});
      }
      bufferedTokens.insert(bufferedTokens.end(), lineTokens.begin(), lineTokens.end());
      continue;
    }

    if (token == INDENT || token == DEDENT) {
      rawIndentDepth += token == INDENT ? 1 : -1;
      while (!layoutDepths.empty() && rawIndentDepth < layoutDepths.back()) {
        layoutDepths.pop_back();
        bufferedTokens.push_back({VIRTUAL_CLOSE, {}, yylloc});
      }
      continue;
    }

    if (token == 0) {
      while (!layoutDepths.empty()) {
        layoutDepths.pop_back();
        bufferedTokens.push_back({VIRTUAL_CLOSE, {}, yylloc});
      }
      bufferedTokens.push_back({0, {}, yylloc});
      continue;
    }

    bufferCurrent(token);
  }

  BufferedToken buffered = bufferedTokens.front();
  bufferedTokens.pop_front();
  int token = buffered.token;
  yylval = buffered.value;
  yylloc = buffered.location;

  // The definition lookahead can also span a constructor call inside a
  // parameter default: f(value = ok(1)) = ... . Definitions are top-level.
  if (token == FUNCTION_NAME && explicitDepth > 0) token = CALL_IDENTIFIER;

  if (token == MATCH && lastSourceToken == '.') {
    token = IDENTIFIER;
    yylval.text = strdup("match");
  }
  if (token == DO_KEYWORD && lastSourceToken == '.') {
    token = IDENTIFIER;
    yylval.text = strdup("do");
  }
  if (token == IF || token == MATCH)
    pendingHeaders.push_back(explicitDepth);
  // A Record literal inside a header is part of its subject, not its body.
  // Nested if/match headers and parenthesized subjects keep their own depth.
  if (token == '{' && !pendingHeaders.empty() && pendingHeaders.back() == explicitDepth &&
      endsExpression(lastSourceToken))
    pendingHeaders.pop_back();
  if (token == '(' || token == '[' || token == '{') ++explicitDepth;
  if (token == ')' || token == ']' || token == '}') --explicitDepth;
  if (token != VIRTUAL_OPEN && token != VIRTUAL_SEMICOLON &&
      token != VIRTUAL_CLOSE && token != 0)
    lastSourceToken = token;
  return token;
}
