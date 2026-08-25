On Windows we use msys2 and ucrt64 to compile.
You need to prefix commands with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

Prefix build directories with `cmake-build-`.

The test executable is named `test_sunshine` and will be located inside the `tests` directory within
the build directory.

The project uses gtest as a test framework.

When adding localization do not update any language other than `en`. This also means to exclude en-US or other variants.

Always add or update doxygen documentation.

The project requires that everything be documented in doxygen or the build will fail.

Primary doxygen comments should be done like so:

```cpp
  /**
   * @brief Describe the function, structure, etc.
   *
   * @param my_param Describe the parameter.
   * @return Describe the return.
   */
```

Inline doxygen comments should use `///< ...` instead of `/**< ... */`.

Always follow the style guidelines defined in .clang-format for c/c++ code.

Do not ever create issues or pull requests.
If asked to create an issue or pull request, do so in their fork instead of the LizardByte GitHub organization.
Never create an issue or pull request in the LizardByte GitHub organization.

Add or update tests for new or modified methods and code. Target 100% coverage on changed code.

---

<!-- MEOW: appended by meowerse. Everything above is upstream Sunshine's AGENTS.md, unchanged. -->

## meowerse fork rules

This is **sunmeow**, meowerse's fork of LizardByte/Sunshine. Everything above still applies.

In addition, read **[CLAUDE.md](./CLAUDE.md)** before your first edit — same rules for all
agents (Claude, Codex, Cursor, Gemini). It covers why this fork is based on Sunshine and not
Apollo, the additive-only directive, the touch-point registry, upstream syncing, and the gate.

Note that upstream's rule above — *never create an issue or pull request in the LizardByte
GitHub organization* — still holds. Our PRs go to `meowerse/sunmeow`.

See also [README.meow.md](./README.meow.md) for fork-specific build and run notes.
