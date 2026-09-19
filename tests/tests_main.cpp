/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */

// standard includes
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>

// platform includes
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// test includes
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

namespace {
  /// Sandbox root, recorded by the constructor so main() can remove it on exit.
  char sandbox_root[4096] = {0};

  /**
   * @brief MEOW-TOUCH(test-isolation): redirect the suite at a throwaway config directory.
   *
   * @details Tests reach the config directory through `platf::appdata()`, which otherwise
   * resolves to the developer's LIVE `~/.config/sunmeow`. That is not cosmetic: suite runs have
   * twice destroyed a real installation's paired-client list and blanked its `uniqueid`, leaving
   * the host unpairable until the state file was repaired by hand.
   *
   * This MUST run before static initialization, not at the top of main(). `src/config.cpp`
   * initializes namespace-scope objects (e.g. `APPS_JSON_PATH`) by calling `platf::appdata()`,
   * and `appdata()` memoizes its answer with `std::call_once`. Those dynamic initializers run
   * before main(), so a setenv() in main() is already too late -- the real config path is
   * cached by then. A previous attempt did exactly that and the guard test below caught it.
   *
   * `constructor(101)` places this in `.init_array.00101`, which the loader runs before the
   * unprioritized `.init_array` entries holding those dynamic initializers.
   *
   * Three variables matter, in `appdata()`'s own precedence order:
   *   - `CONFIGURATION_DIRECTORY` outranks XDG, so it is cleared or it would win.
   *   - `XDG_CONFIG_HOME` is then pointed at the sandbox.
   *   - `SUNSHINE_MIGRATE_CONFIG` is cleared because `=1` makes `appdata()` copy the old
   *     location in and then `fs::remove_all()` the original -- deleting the very config this
   *     is protecting.
   *
   * Only async-signal-safe / C-library calls are used here: this runs before the C++ runtime is
   * fully initialized, so no iostreams and no non-trivial statics.
   */
  __attribute__((constructor(101))) void isolate_config_dir() {
    ::unsetenv("CONFIGURATION_DIRECTORY");
    ::unsetenv("SUNSHINE_MIGRATE_CONFIG");

    const char *tmp = ::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0') {
      tmp = "/tmp";
    }

    std::snprintf(sandbox_root, sizeof(sandbox_root), "%s/sunmeow-tests-%ld", tmp, static_cast<long>(::getpid()));

    if (::mkdir(sandbox_root, 0700) != 0 && errno != EEXIST) {
      std::fprintf(stderr, "FATAL: could not create test config sandbox at %s: %s\n", sandbox_root, std::strerror(errno));
      std::_Exit(1);
    }

    if (::setenv("XDG_CONFIG_HOME", sandbox_root, 1) != 0) {
      std::fprintf(stderr, "FATAL: could not set XDG_CONFIG_HOME to %s\n", sandbox_root);
      std::_Exit(1);
    }
  }
}  // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  const int result = RUN_ALL_TESTS();

  if (sandbox_root[0] != '\0') {
    std::error_code ec;
    std::filesystem::remove_all(sandbox_root, ec);
  }

  return result;
}
