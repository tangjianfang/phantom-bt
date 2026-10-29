#pragma once
// 零依赖测试框架：PB_TEST 注册 + PB_CHECK/PB_CHECK_EQ/PB_CHECK_THROWS 断言 + PB_TEST_MAIN 入口
#include <exception>
#include <cstdio>
#include <string>
#include <vector>

namespace pb {

struct TestCtx {
  int checks = 0;
  int failures = 0;
  const char* name = "";
  void fail(const char* what, const char* file, int line) {
    ++failures;
    std::printf("  FAIL [%s] %s (%s:%d)\n", name, what, file, line);
    std::fflush(stdout);
  }
};

using TestFn = void (*)(TestCtx&);

class TestRegistry {
 public:
  static TestRegistry& instance() {
    static TestRegistry r;
    return r;
  }
  void add(const char* name, TestFn fn) { tests_.push_back({name, fn}); }
  int run_all(const std::string& filter) {
    int failed_tests = 0, total_run = 0;
    for (auto& t : tests_) {
      if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
      ++total_run;
      TestCtx ctx;
      ctx.name = t.name;
      try {
        t.fn(ctx);
      } catch (const std::exception& e) {
        ctx.fail(e.what(), __FILE__, __LINE__);
      } catch (...) {
        ctx.fail("unknown exception", __FILE__, __LINE__);
      }
      if (ctx.failures) {
        ++failed_tests;
        std::printf("[%s] %d check(s) FAILED\n", t.name, ctx.failures);
      } else {
        std::printf("[%s] ok (%d checks)\n", t.name, ctx.checks);
      }
      std::fflush(stdout);
    }
    std::printf("== %d/%d tests passed ==\n", total_run - failed_tests, total_run);
    std::fflush(stdout);
    return failed_tests ? 1 : 0;
  }

 private:
  struct Entry {
    const char* name;
    TestFn fn;
  };
  std::vector<Entry> tests_;
};

}  // namespace pb

#define PB_TEST(name)                                                                     \
  static void pb_test_fn_##name(pb::TestCtx& pb_t);                                       \
  static bool pb_reg_##name = (pb::TestRegistry::instance().add(#name, &pb_test_fn_##name), true); \
  static void pb_test_fn_##name(pb::TestCtx& pb_t)

#define PB_CHECK(c) do { ++pb_t.checks; if (!(c)) pb_t.fail(#c, __FILE__, __LINE__); } while (0)

#define PB_CHECK_EQ(a, b)                                                                  \
  do {                                                                                     \
    ++pb_t.checks;                                                                         \
    if (!((a) == (b))) {                                                                   \
      std::string pb_msg_ = #a " != " #b;                                                  \
      pb_t.fail(pb_msg_.c_str(), __FILE__, __LINE__);                                      \
    }                                                                                      \
  } while (0)

#define PB_CHECK_THROWS(e)                                                                 \
  do {                                                                                     \
    ++pb_t.checks;                                                                         \
    bool threw_ = false;                                                                   \
    try { (void)(e); } catch (...) { threw_ = true; }                                      \
    if (!threw_) pb_t.fail(#e " throws", __FILE__, __LINE__);                              \
  } while (0)

#define PB_TEST_MAIN()                                                                     \
  int main(int argc, char** argv) {                                                        \
    return pb::TestRegistry::instance().run_all(argc > 1 ? argv[1] : "");                  \
  }
