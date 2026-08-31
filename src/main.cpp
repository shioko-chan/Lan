#include "lan/AST.h"
#include "lan/Codegen.h"
#include "lan/RuntimeAssets.h"
#include "parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

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
            << "  " << executable
            << " --run <程序.lan> [--random-seed 整数] [-- 参数...]\n";
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
  std::vector<std::string> programArguments;
  std::string randomSeed;
  bool hasRandomSeed = false;
  bool hasArgumentSeparator = false;
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--") {
      hasArgumentSeparator = true;
      for (++i; i < argc; ++i) programArguments.emplace_back(argv[i]);
      break;
    }
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
    else if (argument == "--random-seed" && i + 1 < argc) {
      randomSeed = argv[++i];
      hasRandomSeed = true;
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
  if (hasArgumentSeparator && mode != Mode::Run) {
    std::cerr << "Lan：程序参数只能用于 --run，并放在 -- 后面\n";
    return 2;
  }
  if (hasRandomSeed && mode != Mode::Run) {
    std::cerr << "Lan：--random-seed 只能用于 --run\n";
    return 2;
  }
  if (hasRandomSeed) {
    uint64_t value = 0;
    if (randomSeed.empty()) {
      std::cerr << "Lan：随机种子必须是 0 到 9007199254740991 之间的整数\n";
      return 2;
    }
    for (char digit : randomSeed) {
      unsigned next = digit >= '0' && digit <= '9' ? (unsigned)(digit - '0') : 10;
      if (next > 9 || value > (UINT64_C(9007199254740991) - next) / 10) {
        std::cerr << "Lan：随机种子必须是 0 到 9007199254740991 之间的整数\n";
        return 2;
      }
      value = value * 10 + next;
    }
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
  std::string nativeRuntime;
  bool regex = generator.needsRegexRuntime();
  if (regex) {
    auto archivePath = buildDirectory / "lan_regex_runtime.a";
    std::ofstream archive(archivePath, std::ios::binary | std::ios::trunc);
    archive.write(reinterpret_cast<const char*>(lan_regex_bytes),
        reinterpret_cast<uintptr_t>(lan_regex_end) -
        reinterpret_cast<uintptr_t>(lan_regex_bytes));
    archive.close();
    if (!archive) {
      std::cerr << "Lan：无法写入正则运行时静态库\n";
      return 1;
    }
    std::ofstream notices(buildDirectory / "REGEX_LICENSES.txt", std::ios::binary);
    notices.write(reinterpret_cast<const char*>(lan_regex_licenses),
        reinterpret_cast<uintptr_t>(lan_regex_licenses_end) -
        reinterpret_cast<uintptr_t>(lan_regex_licenses));
    notices.close();
    if (!notices) {
      std::cerr << "Lan：无法写入正则运行时许可证\n";
      return 1;
    }
    nativeRuntime = " " + shellQuote(archivePath.string()) + " -pthread";
#ifdef __APPLE__
    // Abseil's time/logging implementation depends on this system framework.
    nativeRuntime += " -framework CoreFoundation -Wl,-dead_strip";
#elif defined(__linux__)
    nativeRuntime += " -ldl -lrt -Wl,--gc-sections";
#endif
  }
  std::string linkCommand = shellQuote(regex ? LAN_CLANGXX_PATH : LAN_CLANG_PATH) + " " +
      shellQuote(objectPath.string()) + nativeRuntime + " -lm -o " + shellQuote(executablePath.string());
  status = commandExitCode(std::system(linkCommand.c_str()));
  if (status != 0) {
    std::cerr << "Lan：生成可执行文件失败\n";
    return status;
  }

  if (mode == Mode::Build) {
    std::cout << "Lan：已生成 " << executablePath.string() << "\n";
    return 0;
  }
  std::string executable = executablePath.string();
  if (hasRandomSeed) {
    if (setenv("LAN_INTERNAL_RANDOM_SEED", randomSeed.c_str(), 1) != 0) {
      std::cerr << "Lan：无法配置随机种子：" << std::strerror(errno) << "\n";
      return 1;
    }
  } else {
    unsetenv("LAN_INTERNAL_RANDOM_SEED");
  }
  std::vector<char*> runArguments{executable.data()};
  for (auto& argument : programArguments) runArguments.push_back(argument.data());
  runArguments.push_back(nullptr);
  execv(executable.c_str(), runArguments.data());
  std::cerr << "Lan：执行程序失败：" << std::strerror(errno) << "\n";
  return 1;
}
