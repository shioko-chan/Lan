#pragma once

#include "lan/AST.h"
#include <memory>
#include <string>

namespace llvm { class LLVMContext; class Module; }

namespace lan {

class CodeGenerator {
 public:
  CodeGenerator();
  ~CodeGenerator();
  bool generate(const Program& program, std::string& error);
  bool writeIR(const std::string& path, std::string& error) const;
  std::string getIR() const;
  bool needsRegexRuntime() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lan
