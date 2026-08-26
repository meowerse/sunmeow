/**
 * @file tests/meow/test_sandbox.h
 * @brief MEOW-TOUCH(test-sandbox): keep the test suite out of the user's real config directory.
 *
 * ## The defect
 *
 * Running `./build/tests/test_sunshine` **destroys the developer's live Sunshine
 * configuration**. `tests/unit/test_http_pairing.cpp` drives the genuine pairing code, which
 * persists to `config::nvhttp.file_state`; `tests/unit/test_httpcommon.cpp` and
 * `tests/unit/test_file_handler.cpp` build their scratch directories as
 * `platf::appdata().string() + "/tests/"`. All three resolve against the *real* appdata
 * directory, so a test run overwrites `sunshine_state.json` with a fixture whose only entry is
 * a device named `"test"`, and leaves a stray `tests/` directory behind.
 *
 * That costs the user every paired client. A running Sunshine keeps the real list in memory
 * and rewrites it on its next save, so the damage is latent rather than immediate — it becomes
 * real the moment the process restarts. Reproduced on an unmodified baseline build; this is
 * inherited, not something our changes introduced.
 *
 * ## Why this runs before `main()`, not inside it
 *
 * `platf::appdata()` memoises into a function-local `static fs::path` behind
 * `std::call_once`, so the **first** call in the process wins for its lifetime.
 *
 * That first call happens during **static initialisation**, before `main()` is entered at all:
 * `src/config.cpp:62` declares `const std::string APPS_JSON_PATH = platf::appdata().string() +
 * "/apps.json";` at namespace scope, and the `config::sunshine` aggregate a few hundred lines
 * below it does the same for the config and log paths. Their dynamic initialisers call
 * `appdata()` while the process is still starting up.
 *
 * This was learned the hard way. A first version of this file put the redirect as the first
 * statement of `main()`. It printed its banner, created its temp directory, and **did not
 * work** — a full test run still rewrote the real `sunshine_state.json`, and the sandbox never
 * received a `sunshine/` subdirectory because `appdata()` had already been resolved and frozen.
 *
 * So the redirect is a `constructor(101)` function instead. GCC and Clang run prioritised
 * constructors before the unprioritised ones that carry a translation unit's dynamic
 * initialisation, and 101 is the lowest priority available to user code — meaning it runs
 * first. Setting the variables from a shell before launching the binary works for the same
 * reason, and is the fallback if this ever regresses.
 *
 * ## The three environment variables, and why all three matter
 *
 * `appdata()` resolves in this order: `CONFIGURATION_DIRECTORY`, then `XDG_CONFIG_HOME`, then
 * `$HOME/.config/sunshine`.
 *
 *  1. `XDG_CONFIG_HOME` is set to a fresh temp directory. Note `appdata()` appends `/sunshine`
 *     to it, so the sandbox root and the resulting config path are not the same directory.
 *  2. `CONFIGURATION_DIRECTORY` is **unset**, because it takes precedence — setting only
 *     `XDG_CONFIG_HOME` is silently insufficient under a systemd unit that sets it.
 *  3. `SUNSHINE_MIGRATE_CONFIG` is **unset**, and this one is not tidiness. Setting
 *     `XDG_CONFIG_HOME` makes `appdata()`'s `found` and `migrate_config` both true; if that
 *     variable is `"1"`, `appdata()` then copies `$HOME/.config/sunshine` into the new location
 *     **and calls `fs::remove_all()` on the original**. Redirecting without clearing it would
 *     turn "the tests overwrite your state file" into "the tests delete your entire config
 *     directory".
 *
 * The directory is deliberately **not** cleaned up: a failing test's leftovers are worth more
 * for post-mortem than the few kilobytes they cost, and the OS reclaims `/tmp` anyway.
 */
#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace meow::test_sandbox {

  /**
   * @brief Point the whole test process at a throwaway appdata directory.
   *
   * Runs automatically before `main()` via `constructor(101)` — see the note above on why
   * `main()` itself is too late. Not meant to be called by hand.
   *
   * @return The sandbox root that `XDG_CONFIG_HOME` was set to, for reporting.
   */
  inline std::filesystem::path redirect_appdata() {
    // Higher precedence than XDG_CONFIG_HOME -- must go, or the redirect is a no-op.
    ::unsetenv("CONFIGURATION_DIRECTORY");

    // Would make appdata() copy the real config here and DELETE the original. See above.
    ::unsetenv("SUNSHINE_MIGRATE_CONFIG");

    std::error_code ec;
    auto root = std::filesystem::temp_directory_path(ec) /
                ("sunshine-tests-" + std::to_string(::getpid()));
    if (ec) {
      // No temp dir means we cannot sandbox. Say so loudly rather than silently writing to the
      // user's real config -- Boost logging is not up yet, so this goes to stderr.
      std::cerr << "meow test sandbox: no temp directory (" << ec.message()
                << "); REFUSING to run against the real config directory" << std::endl;
      std::abort();
    }

    std::filesystem::create_directories(root, ec);
    if (ec) {
      std::cerr << "meow test sandbox: could not create " << root << " (" << ec.message()
                << "); REFUSING to run against the real config directory" << std::endl;
      std::abort();
    }

    if (::setenv("XDG_CONFIG_HOME", root.c_str(), 1) != 0) {
      std::cerr << "meow test sandbox: setenv(XDG_CONFIG_HOME) failed;"
                << " REFUSING to run against the real config directory" << std::endl;
      std::abort();
    }

    // Deliberately reports the sandbox ROOT, not root/<product>. appdata() appends the product
    // directory itself, and hardcoding that name here would silently go stale the next time it
    // changes -- it already did once, when the rebrand moved it from `sunshine` to `sunmeow`.
    std::cout << "meow test sandbox: XDG_CONFIG_HOME -> " << root << std::endl;
    return root;
  }

  /**
   * @brief Run the redirect before any translation unit's dynamic initialisation.
   *
   * Priority 101 is the lowest a user may specify; prioritised constructors run before the
   * unprioritised ones that perform static initialisation, so this beats `config.cpp`'s
   * `APPS_JSON_PATH` to the first `platf::appdata()` call.
   */
  [[maybe_unused]] __attribute__((constructor(101))) static void meow_test_sandbox_init() {
    redirect_appdata();
  }

}  // namespace meow::test_sandbox
