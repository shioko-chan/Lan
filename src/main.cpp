#include "lan/AST.h"
#include "lan/Codegen.h"
#include "parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern FILE* yyin;
extern int yylineno;
extern void yyrestart(FILE* input);
extern int yyparse(lan::Program** out);
std::string lan_parse_error;

namespace {

void usage(const char* executable) {
  std::cerr << "用法：" << executable
            << " [--emit-ir | --run] <程序.lan> [-o 输出.ll]\n";
}

std::string shellQuote(const std::string& value) {
  std::string result = "'";
  for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
}

}  // namespace

int main(int argc, char** argv) {
  bool run = false;
  std::string inputPath;
  std::string outputPath;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--run") run = true;
    else if (argument == "--emit-ir") run = false;
    else if (argument == "-o" && i + 1 < argc) outputPath = argv[++i];
    else if (!argument.empty() && argument[0] == '-') {
      usage(argv[0]);
      return 2;
    } else if (inputPath.empty()) inputPath = argument;
    else {
      usage(argv[0]);
      return 2;
    }
  }
  if (inputPath.empty()) {
    usage(argv[0]);
    return 2;
  }

  FILE* file = std::fopen(inputPath.c_str(), "r");
  if (!file) {
    std::cerr << "Lan：无法打开文件“" << inputPath << "”\n";
    return 1;
  }
  yyin = file;
  yylineno = 1;
  yyrestart(file);
  lan::Program* parsed = nullptr;
  int parseResult = yyparse(&parsed);
  std::fclose(file);
  std::unique_ptr<lan::Program> program(parsed);
  if (parseResult != 0 || !program) {
    std::cerr << "Lan：" << lan_parse_error << "\n";
    return 1;
  }

  lan::CodeGenerator generator;
  std::string error;
  if (!generator.generate(*program, error)) {
    std::cerr << "Lan：" << error << "\n";
    return 1;
  }

  if (!run) {
    if (outputPath.empty()) std::cout << generator.getIR();
    else if (!generator.writeIR(outputPath, error)) {
      std::cerr << "Lan：无法写入“" << outputPath << "”：" << error << "\n";
      return 1;
    }
    return 0;
  }

  if (!outputPath.empty()) {
    std::cerr << "Lan：--run 不能和 -o 同时使用\n";
    return 2;
  }
  auto temporary = std::filesystem::temp_directory_path() /
                   ("lan-" + std::to_string(static_cast<long long>(getpid())) + ".ll");
  if (!generator.writeIR(temporary.string(), error)) {
    std::cerr << "Lan：无法创建临时 LLVM 文件：" << error << "\n";
    return 1;
  }
  std::string command = shellQuote(LAN_LLI_PATH) + " " + shellQuote(temporary.string());
  int status = std::system(command.c_str());
  std::error_code removeError;
  std::filesystem::remove(temporary, removeError);
  if (status == -1) return 1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
