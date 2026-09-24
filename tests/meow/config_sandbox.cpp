/**
 * @file tests/meow/config_sandbox.cpp
 * @brief Point the whole test binary at a throwaway config directory before anything reads it.
 *
 * Tests reach the config directory through `platf::appdata()`, which otherwise resolves to the
 * developer's LIVE `~/.config/sunmeow`. That is not cosmetic: suite runs have twice destroyed a
 * real installation's paired-client list and blanked its `uniqueid`, leaving the host unpairable
 * until the state file was repaired by hand. The capture tests also read and rewrote the live
 * `portal_token`.
 *
 * The redirection MUST happen before static initialization, not in `main()`: `src/config.cpp`
 * initializes namespace-scope objects (e.g. `APPS_JSON_PATH`) by calling `platf::appdata()`, and
 * `appdata()` memoizes its answer with `std::call_once`. Those dynamic initializers run before
 * `main()`, so a `setenv()` there is already too late. `constructor(101)` places the setup in
 * `.init_array.00101`, which the loader runs before the unprioritized `.init_array` entries that
 * hold those initializers; the priority is link-wide, so this can live in its own file rather
 * than in upstream's `tests_main.cpp`.
 *
 * Linux and FreeBSD only: that is where `appdata()` honours `XDG_CONFIG_HOME`. Windows resolves
 * the config directory from the executable's location and macOS from `$HOME/.config`, so the
 * redirection would have no effect there; `ConfigDirIsolation` skips on those platforms.
 */
#if !defined(_WIN32) && !defined(__APPLE__)

  // standard includes
  #include <cerrno>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <system_error>

  // platform includes
  #include <unistd.h>

namespace {
  /// Sandbox root created by `isolate_config_dir()`; empty until then.
  char sandbox_root[4096] = {0};

  /// Process that created the sandbox; only it may remove the directory.
  pid_t sandbox_owner = 0;

  /**
   * @brief Redirect the config directory at a fresh, private sandbox.
   *
   * @details Three variables matter, in `appdata()`'s own precedence order:
   *   - `CONFIGURATION_DIRECTORY` outranks XDG, so it is cleared or it would win.
   *   - `XDG_CONFIG_HOME` is then pointed at the sandbox.
   *   - `SUNSHINE_MIGRATE_CONFIG` is cleared because `=1` makes `appdata()` copy the old
   *     location in and then `fs::remove_all()` the original -- deleting the very config this
   *     exists to protect.
   *
   * `mkdtemp()` guarantees a new 0700 directory with an unpredictable name, so a leftover from a
   * crashed run is never reused and a pre-planted path or symlink cannot be followed. Any failure
   * is fatal: running the suite unsandboxed is exactly the outcome this prevents.
   *
   * C library only -- no iostreams, no C++ statics -- because this runs before the C++ dynamic
   * initializers of the test binary.
   */
  __attribute__((constructor(101))) void isolate_config_dir() {
    ::unsetenv("CONFIGURATION_DIRECTORY");
    ::unsetenv("SUNSHINE_MIGRATE_CONFIG");

    const char *tmp = ::getenv("TMPDIR");
    if (tmp == nullptr || tmp[0] == '\0') {
      tmp = "/tmp";
    }

    const int written = std::snprintf(sandbox_root, sizeof(sandbox_root), "%s/sunmeow-tests-XXXXXX", tmp);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(sandbox_root) || ::mkdtemp(sandbox_root) == nullptr) {
      std::fprintf(stderr, "FATAL: could not create the test config sandbox under %s: %s\n", tmp, std::strerror(errno));
      std::_Exit(1);
    }
    sandbox_owner = ::getpid();

    if (::setenv("XDG_CONFIG_HOME", sandbox_root, 1) != 0) {
      std::fprintf(stderr, "FATAL: could not set XDG_CONFIG_HOME to %s\n", sandbox_root);
      std::_Exit(1);
    }
  }

  /**
   * @brief Remove the sandbox once everything that might write into it has finished.
   *
   * @details `destructor(101)` runs after the unprioritized destructors, i.e. after the static
   * objects that could still flush into the config directory. Forked children (death tests,
   * helpers) inherit the path but not ownership, so they never delete the parent's sandbox.
   */
  __attribute__((destructor(101))) void remove_config_dir() {
    if (sandbox_root[0] == '\0' || sandbox_owner != ::getpid()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(sandbox_root, ec);
  }
}  // namespace

#endif
