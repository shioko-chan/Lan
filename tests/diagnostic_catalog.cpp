#include "lan/Diagnostic.h"

#include <cassert>

int main() {
  using namespace lan;
  DiagnosticLanguage language;
  assert(parseDiagnosticLanguage("zh-CN", language));
  assert(language == DiagnosticLanguage::Chinese);
  assert(parseDiagnosticLanguage("zh", language));
  assert(parseDiagnosticLanguage("en", language));
  assert(language == DiagnosticLanguage::English);
  assert(!parseDiagnosticLanguage("fr", language));
  diagnosticLanguage() = DiagnosticLanguage::Chinese;
  assert(std::string(diagnosticPrefix()) == "Lan：");
  assert(diagnostic(DiagnosticKey::UnknownName, {"answer"}) ==
         "找不到名称“answer”，请检查拼写或先给它一个值");
  diagnosticLanguage() = DiagnosticLanguage::English;
  assert(std::string(diagnosticPrefix()) == "Lan: ");
  assert(compilerContext("if 后面的条件") == "if condition");
  assert(compilerContext("函数“sum”的结果") == "result of function sum");
  assert(compilerContext("传给函数“sum”的参数“numbers”") ==
         "argument numbers passed to function sum");
  assert(compilerContext("传给函数“size”的参数“items”的列表元素") ==
         "argument items passed to function size List element");
  assert(compilerContext("sort 的结果") == "sort result");
  assert(requiredArguments("length", RequirementKey::OneListOrText) ==
         "length requires one List or Text argument");
  assert(diagnostic(DiagnosticKey::UnknownName, {"answer"}) ==
         "Unknown name answer; check its spelling or define it first");
  assert(diagnostic(DiagnosticKey::SourceLocation, {"sample.lan", "3", "8"}) ==
         "\nLocation: sample.lan:3:8");
  assert(diagnostic(DiagnosticKey::SourceLocation, {"{1}.lan", "3", "8"}) ==
         "\nLocation: {1}.lan:3:8");
}
