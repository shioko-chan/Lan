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

extern FILE* yyin;
extern int yylineno;
extern void yyrestart(FILE* input);
extern int yyparse(lan::Program** out);
std::string lan_parse_error;

namespace {

void usage(const char* executable) {
  std::cerr << "用法：\n"
            << "  " << executable << " [--emit-ir] <程序.lan> [-o 输出.ll]\n"
            << "  " << executable << " --build <程序.lan>\n"
            << "  " << executable << " --run <程序.lan>\n";
}

std::string shellQuote(const std::string& value) {
  std::string result = "'";
  for (char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
}

enum class Mode { EmitIR, Build, Run };

int commandExitCode(int status) {
  if (status == -1) return 1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Mode mode = Mode::EmitIR;
  bool explicitMode = false;
  std::string inputPath;
  std::string outputPath;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--run" || argument == "--build" || argument == "--emit-ir") {
      Mode requested = argument == "--run" ? Mode::Run
                     : argument == "--build" ? Mode::Build : Mode::EmitIR;
      if (explicitMode && requested != mode) {
        usage(argv[0]);
        return 2;
      }
      mode = requested;
      explicitMode = true;
    }
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
  if (mode != Mode::EmitIR && !outputPath.empty()) {
    std::cerr << "Lan：--build 和 --run 不能和 -o 同时使用\n";
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
  program->sourcePath = inputPath;

  lan::CodeGenerator generator;
  std::string error;
  if (!generator.generate(*program, error)) {
    std::cerr << "Lan：" << error << "\n";
    return 1;
  }

  if (mode == Mode::EmitIR) {
    if (outputPath.empty()) std::cout << generator.getIR();
    else if (!generator.writeIR(outputPath, error)) {
      std::cerr << "Lan：无法写入“" << outputPath << "”：" << error << "\n";
      return 1;
    }
    return 0;
  }

  std::filesystem::path source(inputPath);
  std::string programName = source.stem().string();
  if (programName.empty()) {
    std::cerr << "Lan：无法从输入文件名确定程序名称\n";
    return 1;
  }
  auto buildDirectory = std::filesystem::current_path() / ".lan" / programName;
  std::error_code directoryError;
  std::filesystem::create_directories(buildDirectory, directoryError);
  if (directoryError) {
    std::cerr << "Lan：无法创建构建目录“" << buildDirectory.string()
              << "”：" << directoryError.message() << "\n";
    return 1;
  }
  auto irPath = buildDirectory / (programName + ".ll");
  auto objectPath = buildDirectory / (programName + ".o");
  auto executablePath = buildDirectory / programName;
  if (!generator.writeIR(irPath.string(), error)) {
    std::cerr << "Lan：无法写入 LLVM IR“" << irPath.string() << "”：" << error << "\n";
    return 1;
  }

  std::string compileCommand = shellQuote(LAN_CLANG_PATH) + " -O2 -c " +
      shellQuote(irPath.string()) + " -o " + shellQuote(objectPath.string());
  int status = commandExitCode(std::system(compileCommand.c_str()));
  if (status != 0) {
    std::cerr << "Lan：生成目标文件失败\n";
    return status;
  }
  std::string linkCommand = shellQuote(LAN_CLANG_PATH) + " " +
      shellQuote(objectPath.string()) + " -o " + shellQuote(executablePath.string());
  status = commandExitCode(std::system(linkCommand.c_str()));
  if (status != 0) {
    std::cerr << "Lan：生成可执行文件失败\n";
    return status;
  }

  if (mode == Mode::Build) {
    std::cout << "Lan：已生成 " << executablePath.string() << "\n";
    return 0;
  }
  return commandExitCode(std::system(shellQuote(executablePath.string()).c_str()));
}
