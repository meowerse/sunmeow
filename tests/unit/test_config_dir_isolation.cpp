/**
 * @file tests/unit/test_config_dir_isolation.cpp
 * @brief Guard that the suite never resolves to the developer's live config directory.
 */

#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <cstdlib>
#include <filesystem>

// local includes
#include <src/platform/common.h>

namespace fs = std::filesystem;

/**
 * @brief The suite must operate on a sandbox, never `~/.config/sunmeow`.
 *
 * @details Regression: tests reach the config directory via `platf::appdata()`. Without the
 * sandbox installed by tests/meow/config_sandbox.cpp that resolved to the real installation,
 * and suite runs twice wiped a live paired-client list and blanked its `uniqueid`, leaving the
 * host unpairable. This asserts the redirection is actually in effect, so the protection cannot
 * silently regress.
 */
TEST(ConfigDirIsolation, AppdataIsNotTheUsersLiveConfigDirectory) {
#if defined(_WIN32)
  GTEST_SKIP() << "appdata() resolves from the executable's location on Windows, never the user profile";
#endif
  // The sandbox records the real HOME before it (on macOS) redirects HOME itself.
  const char *home = std::getenv("SUNMEOW_TESTS_REAL_HOME");
  if (home == nullptr) {
    home = std::getenv("HOME");
  }
  ASSERT_NE(home, nullptr);

  const auto appdata = fs::weakly_canonical(platf::appdata());
  const auto live_config = fs::path {home} / ".config" / "sunmeow";

  EXPECT_NE(appdata, fs::weakly_canonical(live_config));

  // Also assert it is not anywhere beneath the user's real config tree.
  const auto user_config_root = fs::weakly_canonical(fs::path {home} / ".config");
  const auto mismatch = std::mismatch(user_config_root.begin(), user_config_root.end(), appdata.begin(), appdata.end());
  EXPECT_NE(mismatch.first, user_config_root.end())
    << "appdata() resolved inside the user's real config tree: " << appdata;
}
