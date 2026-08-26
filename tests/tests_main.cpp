/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */
#include "meow/test_sandbox.h"  // MEOW-TOUCH(test-sandbox): must precede any platf::appdata() call
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

int main(int argc, char **argv) {
  // MEOW-TOUCH(test-sandbox): the include alone arms the redirect -- it runs from a
  // constructor(101), before any translation unit's static initialisation, because
  // config.cpp resolves platf::appdata() at namespace scope and main() is already too
  // late. @see tests/meow/test_sandbox.h
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
