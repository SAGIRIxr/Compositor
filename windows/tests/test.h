// 极简测试框架：不依赖第三方库，CI 上直接运行。
#pragma once
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct TestCase { const char* name; std::function<void()> body; };
std::vector<TestCase>& testRegistry();
struct TestRegistrar { TestRegistrar(const char* name, std::function<void()> body) { testRegistry().push_back({name, std::move(body)}); } };
struct TestFailure { std::string message; };

// 让 uint8_t 之类按数字打印。
template <typename T> const T& testPrintable(const T& v) { return v; }
inline int testPrintable(unsigned char v) { return v; }
inline int testPrintable(signed char v) { return v; }

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name) \
    static void name(); \
    static TestRegistrar TEST_CONCAT(registrar_, name)(#name, name); \
    static void name()

#define CHECK(condition) do { if (!(condition)) { std::ostringstream s_; s_ << __FILE__ << ":" << __LINE__ << " 断言失败：" #condition; throw TestFailure{s_.str()}; } } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { std::ostringstream s_; s_ << __FILE__ << ":" << __LINE__ << " " #a " == " #b " 失败：" << testPrintable(va_) << " 与 " << testPrintable(vb_); throw TestFailure{s_.str()}; } } while (0)
#define CHECK_NEAR(a, b, tolerance) do { double va_ = (a), vb_ = (b); if (std::abs(va_ - vb_) > (tolerance)) { std::ostringstream s_; s_ << __FILE__ << ":" << __LINE__ << " " #a " ≈ " #b " 失败：" << va_ << " 与 " << vb_; throw TestFailure{s_.str()}; } } while (0)
#define CHECK_THROWS(statement) do { bool thrown_ = false; try { statement; } catch (...) { thrown_ = true; } if (!thrown_) { std::ostringstream s_; s_ << __FILE__ << ":" << __LINE__ << " 应抛出异常：" #statement; throw TestFailure{s_.str()}; } } while (0)
