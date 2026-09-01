#pragma once

#include "lan/AST.h"
#include <set>

namespace lan {

// Coverage is structural, independent of call-site constant values. A missing
// arm on T is permitted, allowing the same function to handle T and Option<T>.
struct MatchCoverage {
  bool missing = false;
  bool present = false;
  bool trueValue = false;
  bool falseValue = false;
  bool ok = false;
  bool err = false;
  std::set<double> numbers;
  std::set<std::string> texts;

  // False means this arm is subsumed by preceding arms.
  bool add(const MatchPattern& pattern) {
    using Kind = MatchPattern::Kind;
    switch (pattern.kind) {
      case Kind::Ok:
      case Kind::Err: {
        bool& seen = pattern.kind == Kind::Ok ? ok : err;
        if (present || seen) return false;
        seen = true;
        present = ok && err;
        return true;
      }
      case Kind::Missing:
        if (missing) return false;
        missing = true;
        return true;
      case Kind::Present:
        if (present) return false;
        present = true;
        return true;
      case Kind::Wildcard:
      case Kind::Binding:
        if (present && missing) return false;
        present = missing = true;
        return true;
      case Kind::Literal:
        if (present) return false;
        if (auto* number = dynamic_cast<const NumberExpr*>(pattern.literal.get()))
          return numbers.insert(number->value).second;
        if (auto* text = dynamic_cast<const TextExpr*>(pattern.literal.get()))
          return texts.insert(text->value).second;
        if (auto* boolean = dynamic_cast<const BooleanExpr*>(pattern.literal.get())) {
          bool& seen = boolean->value ? trueValue : falseValue;
          if (seen) return false;
          seen = true;
          present = trueValue && falseValue;
        } else if (dynamic_cast<const UnitExpr*>(pattern.literal.get())) {
          present = true;
        }
        return true;
    }
    return false;
  }
};

inline bool bindsName(const MatchPattern& pattern) {
  return (pattern.kind == MatchPattern::Kind::Binding ||
          pattern.kind == MatchPattern::Kind::Present ||
          pattern.kind == MatchPattern::Kind::Ok ||
          pattern.kind == MatchPattern::Kind::Err) && pattern.name != "_";
}

inline bool resultPattern(const MatchPattern& pattern) {
  return pattern.kind == MatchPattern::Kind::Ok ||
         pattern.kind == MatchPattern::Kind::Err;
}

}  // namespace lan
