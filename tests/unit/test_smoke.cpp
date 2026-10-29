#include "test/test_framework.h"

PB_TEST(smoke_framework_registers) {
  PB_CHECK_EQ(1 + 1, 2);
}

PB_TEST(smoke_second_test_registered) {
  PB_CHECK(true);
}
