#ifndef LAN_RUNTIME_OWNERSHIP_HPP
#define LAN_RUNTIME_OWNERSHIP_HPP

#include "value.h"
#include <cstdlib>
#include <memory>
#include <utility>

// One owned reference. No implicit copies, conversions, or extra allocation.
class OwnedValue {
 public:
  static OwnedValue adopt(LanValue value) noexcept { return OwnedValue(value); }
  static OwnedValue share(LanValue value) noexcept {
    lan_value_retain(value.tag, value.number, value.pointer);
    return OwnedValue(value);
  }
  OwnedValue(const OwnedValue&) = delete;
  OwnedValue& operator=(const OwnedValue&) = delete;
  OwnedValue(OwnedValue&& other) noexcept : value_(other.take()) {}
  OwnedValue& operator=(OwnedValue&& other) noexcept {
    if (this != &other) {
      OwnedValue previous(std::move(*this));
      value_ = other.take();
    }
    return *this;
  }
  ~OwnedValue() { lan_value_release(value_.tag, value_.number, value_.pointer); }
  LanValue borrow() const noexcept { return value_; }
  template<class T = void> T* get() const noexcept {
    return static_cast<T*>(value_.pointer);
  }
  [[nodiscard]] LanValue take() noexcept {
    return std::exchange(value_, LanValue{LAN_MISSING, 0, nullptr});
  }
  template<class T = void> [[nodiscard]] T* takePointer() noexcept {
    return static_cast<T*>(take().pointer);
  }
 private:
  explicit OwnedValue(LanValue value) noexcept : value_(value) {}
  LanValue value_;
};

struct FreeMemory {
  void operator()(void* pointer) const noexcept { std::free(pointer); }
};
template<class T> using OwnedMemory = std::unique_ptr<T, FreeMemory>;

#endif
