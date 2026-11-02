#include "test/test_framework.h"

extern "C" int pb_arch_contracts_anchor();

int main(int argc, char** argv) {
  (void)pb_arch_contracts_anchor();
  return pb::TestRegistry::instance().run_all(argc > 1 ? argv[1] : "");
}
