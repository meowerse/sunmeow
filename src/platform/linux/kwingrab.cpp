/**
 * @file src/platform/linux/kwingrab.cpp
 * @brief KWin direct ScreenCast capture via zkde_screencast_unstable_v1 Wayland protocol.
 *
 * Bypasses xdg-desktop-portal entirely. Sunshine connects directly to KWin's
 * Wayland protocol to obtain a PipeWire node_id, then streams frames via PipeWire.
 *
 * Chain: KWin -> Wayland kde_screencast -> PipeWire -> Sunshine
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <pwd.h>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

// lib includes
#include <lizardbyte/common/env.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// generated protocol header
#include <kde-output-order-v1.h>
#include <xdg-output-unstable-v1.h>
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "pipewire.cpp"
#include "src/meow/display_union.h"  // MEOW-TOUCH(unified-desktop-capture): pure union-of-outputs geometry
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace kwin {
  /**
   * KWin Wayland ScreenCast permissions
   *
   * To have access to zkde_screencast_unstable_v1 KWin checks for a .desktop file with
   * X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1 and the current executable name
   * in the Exec= parameter.
   */
  class screencast_permission_helper_t {
  public:
    /**
     * @brief Check whether permission system deactivated.
     *
     * @return True when KWin reports that the permission system is disabled.
     */
    static bool is_permission_system_deactivated() {
      return lizardbyte::common::get_env("KWIN_WAYLAND_NO_PERMISSION_CHECKS") == "1";
    }

    /**
     * @brief Configure the KWin screencast session.
     */
    static void setup() {
      if (initialized) {
        return;
      }
      auto filenameprefix = std::format("{}.kwin", PROJECT_FQDN);
      auto executablepath = get_executable_full_path();

      // System: Check system XDG applications for permission (usually installed with Sunshine)
      if (check_kwin_system_permissions(filenameprefix, executablepath)) {
        create_file = false;
        initialized = true;
        return;
      }

      // If we do not have a system permission, check if we need a temporary permission via user's application directory
      if (is_permission_system_deactivated()) {
        BOOST_LOG(info) << "[kwingrab] No permission desktop file necessary. KWin permission system deactivated.";
        create_file = false;
        initialized = true;
        return;
      }

      // User: Check and (if necessary) update user's XDG applications for permission
      auto user_applications = get_xdg_user_applications_path();
      if (user_applications.empty()) {
        BOOST_LOG(error) << "[kwingrab] Failed to determine user application directory. Cannot continue with permission setup.";
        return;
      }
      // Create non-existing application directory so we can write into it
      if (!std::filesystem::exists(user_applications) && !std::filesystem::create_directories(user_applications)) {
        // In case of failure log and return
        BOOST_LOG(error) << "[kwingrab] Failed to create application directory. Cannot continue with permission setup.";
        create_file = false;
        initialized = true;
        return;
      }
      auto user_filepathprefix = (std::filesystem::path(user_applications) / filenameprefix).string();
      for (const auto &path : std::filesystem::directory_iterator(user_applications)) {
        // List existing files for prefix and check if they contain this executable or remove them
        const auto entry = path.path().string();
        if (entry.starts_with(user_filepathprefix)) {
          auto entry_executablepath = get_executable_from_desktop_file(entry);
          if (!entry_executablepath.empty() && entry_executablepath == executablepath) {
            // This entry is exactly the one we need
            BOOST_LOG(debug) << "[kwingrab] Ignoring current temporary KWin wayland permission file: "sv << entry;
            create_file = false;
            continue;
          }
          if (!entry_executablepath.empty() && std::filesystem::exists(entry_executablepath)) {
            // This entry is for another sunshine executable that still exists
            BOOST_LOG(debug) << "[kwingrab] Ignoring other valid temporary KWin wayland permission file: "sv << entry;
            continue;
          }
          if (std::filesystem::remove(path)) {
            BOOST_LOG(info) << "[kwingrab] Removed stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          } else {
            BOOST_LOG(warning) << "[kwingrab] Failed to remove stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          }
        }
      }
      if (create_file) {
        // Generate a unique file identifier based on current unixtime
        auto user_filepathidentifier = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        auto user_filepath = std::format("{}{}.desktop", user_filepathprefix, user_filepathidentifier);
        // Write new file if necessary
        std::ofstream filestream(user_filepath);
        if (filestream.is_open()) {
          filestream << "[Desktop Entry]" << std::endl
                     << "Exec=" << executablepath << std::endl
                     << "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1" << std::endl
                     << "Type=Application" << std::endl
                     << "Name="sv << PROJECT_FQDN << "-kwin-wayland-permission" << std::endl
                     << "Comment=Sunshine KWin screencast permission" << std::endl
                     << "NoDisplay=true" << std::endl;
          filestream.close();
          // Give KWin time to catch up to the new desktop file
          BOOST_LOG(info) << "[kwingrab] Created temporary KWin wayland permission file: "sv << user_filepath << " - Waiting 3 seconds for KDE to pick up new file.";
          std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        } else {
          BOOST_LOG(warning) << "[kwingrab] Failed to open temporary KWin wayland permission file: "sv << user_filepath;
        }
      }

      initialized = true;
    }

    /**
     * @brief Check whether newly initialized.
     *
     * @return True when KWin was initialized during this check.
     */
    static bool is_newly_initialized() {
      return create_file;
    }

  private:
    static inline bool initialized = false;
    static inline bool create_file = true;

    static std::filesystem::path get_home_dir() {
      // Check HOME environment variable
      if (std::string homedir = lizardbyte::common::get_env("HOME"); !homedir.empty()) {
        return homedir;
      }
      // Fall back to home directory from NSS passwd
      // Note: This should be thread-safe as we're always accessing the same entry for Sunshine
      return getpwuid(geteuid())->pw_dir;
    }

    static std::filesystem::path get_xdg_user_applications_path() {
      // Follow the XDG base directory specification for user data home:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      std::filesystem::path xdg_data_home;
      if (std::string dir = lizardbyte::common::get_env("XDG_DATA_HOME"); !dir.empty()) {
        xdg_data_home = std::filesystem::path(dir);
      } else {
        const auto homedir = get_home_dir();
        if (homedir.empty()) {
          return "";
        }
        xdg_data_home = std::filesystem::path(homedir) / ".local"sv / "share"sv;
      }
      return xdg_data_home / "applications";
    }

    static std::string get_executable_full_path() {
      // Adapted from https://linuxvox.com/blog/how-do-i-find-the-location-of-the-executable-in-c/
      constexpr auto path_len = PATH_MAX;  // PATH_MAX is defined in limits.h (e.g., 4096 on Linux)
      auto path_exe = std::make_unique<char[]>(path_len);
      // Read the symlink /proc/self/exe into path_exe
      const ssize_t len = readlink("/proc/self/exe", &path_exe[0], path_len - 1);
      if (len == -1) {
        return "";
      }
      // Return path_exe as a proper std::string with len returned by readlink
      return std::string(path_exe.get(), len);
    }

    static std::string get_executable_from_desktop_file(const std::string &path) {
      if (std::ifstream file(path); file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
          if (line.starts_with("Exec=") && line.length() > 5) {
            return line.substr(5);
          }
        }
      }
      return "";
    }

    static bool check_kwin_system_permissions(const std::string_view &filenameprefix, const std::string_view &executablepath) {
      // Find data dirs to check from XDG_DATA_DIRS
      std::vector<std::string> xdg_data_dirs;
      if (const std::string e = lizardbyte::common::get_env("XDG_DATA_DIRS"); !e.empty()) {
        std::stringstream ss(e);
        std::string item;

        while (getline(ss, item, ':')) {  // : is likely valid for all OSes supported, if a constant is available it should be used instead
          xdg_data_dirs.push_back(item);
        }
      }
      // Use defaults from https://specifications.freedesktop.org/basedir/latest/ if ENV var was empty
      if (xdg_data_dirs.empty()) {
        xdg_data_dirs.emplace_back("/usr/local/share/");
        xdg_data_dirs.emplace_back("/usr/share/");
      }
      // Check for ${filenameprefix}.desktop in each directory
      for (auto const &dir : xdg_data_dirs) {
        std::string filename = std::format("{0}{1}applications{1}{2}.desktop", dir, boost::filesystem::path::preferred_separator, filenameprefix);
        if (std::filesystem::exists(filename)) {
          auto file_executablepath = get_executable_from_desktop_file(filename);
          if (file_executablepath == executablepath) {
            BOOST_LOG(info) << "[kwingrab] Found matching system KWin desktop permission file: "sv << filename;
            return true;
          }
        }
      }
      return false;
    }
  };

  // MEOW-TOUCH(unified-desktop-capture): `meow::display_union::output_transform_t` mirrors
  // `enum wl_output_transform` so the pure geometry header needs no Wayland dependency and
  // stays unit testable without a compositor. Pin the two together here — the one place that
  // converts between them — so any drift is a compile error rather than a silent misrotation.
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::normal) == WL_OUTPUT_TRANSFORM_NORMAL);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::rotate_90) == WL_OUTPUT_TRANSFORM_90);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::rotate_180) == WL_OUTPUT_TRANSFORM_180);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::rotate_270) == WL_OUTPUT_TRANSFORM_270);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::flipped) == WL_OUTPUT_TRANSFORM_FLIPPED);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::flipped_90) == WL_OUTPUT_TRANSFORM_FLIPPED_90);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::flipped_180) == WL_OUTPUT_TRANSFORM_FLIPPED_180);
  static_assert(static_cast<int32_t>(meow::display_union::output_transform_t::flipped_270) == WL_OUTPUT_TRANSFORM_FLIPPED_270);

  // Output parameters
  /**
   * @brief KWin screencast output name and geometry.
   */
  struct output_parameter_t {
    std::string name;  ///< KWin output name.
    int width = 0;  ///< Output width in pixels.
    int height = 0;  ///< Output height in pixels.
    int pos_x = 0;  ///< Output X position in the compositor layout.
    int pos_y = 0;  ///< Output Y position in the compositor layout.
    // MEOW-TOUCH(unified-desktop-capture): logical geometry + scale + refresh, needed to
    // build a stream_region request. `width`/`height` above are device pixels (wl_output
    // mode); these are logical (xdg_output) and are the coordinate space stream_region uses.
    int logical_width = 0;  ///< Logical width from xdg_output, 0 when unknown.
    int logical_height = 0;  ///< Logical height from xdg_output, 0 when unknown.
    // xdg_output's logical position is kept in its OWN fields and never written back over
    // `pos_x`/`pos_y` above. Those become display_t::offset_x/offset_y, which pipewire.cpp
    // matches for equality against wl::monitors(); mixing two sources there would silently
    // break single-output capture. Only the union path reads these.
    int xdg_logical_x = 0;  ///< Logical X from xdg_output.
    int xdg_logical_y = 0;  ///< Logical Y from xdg_output.
    bool has_xdg_position = false;  ///< True once xdg_output reported a logical position.
    int transform = 0;  ///< wl_output transform; wl_output::mode is reported pre-transform.
    int wl_scale = 1;  ///< Integer wl_output scale, fallback only when xdg_output is absent.
    int refresh_mhz = 0;  ///< Current mode refresh rate in mHz.
    bool is_union = false;  ///< True for the synthetic whole-desktop pseudo-output.
    // order is needed to get a sorted output list and should be updated before sorting to have current values
    /**
     * @brief Order.
     */
    size_t order = SIZE_MAX;  // Use high number to keep monitors with uninitialized order value to the back
  };

  /**
   * Wayland KDE ScreenCast session
   *
   * Owns its own wl_display connection. Binds zkde_screencast_unstable_v1
   * and wl_output from the registry, then calls stream_output() to start
   * a ScreenCast. Waits for the created(node_id) event from KWin.
   */
  class screencast_t {
  public:
    screencast_t &operator=(screencast_t &&) = delete;  // Do not allow to copying

    ~screencast_t() {
      // Release KDE screencast wayland extensions and reset pointers
      if (kde_screencast_stream_v1_) {
        zkde_screencast_stream_unstable_v1_close(kde_screencast_stream_v1_);
        kde_screencast_stream_v1_ = nullptr;
      }
      if (kde_screencast_v1_) {
        zkde_screencast_unstable_v1_destroy(kde_screencast_v1_);
        kde_screencast_v1_ = nullptr;
      }
      if (kde_output_order) {
        kde_output_order_v1_destroy(kde_output_order);
        kde_output_order = nullptr;
      }
      // MEOW-TOUCH(unified-desktop-capture): release the xdg-output objects we created.
      for (auto *xdg_output : xdg_outputs | std::views::keys) {
        zxdg_output_v1_destroy(xdg_output);
      }
      xdg_outputs.clear();
      if (xdg_output_manager) {
        zxdg_output_manager_v1_destroy(xdg_output_manager);
        xdg_output_manager = nullptr;
      }

      // Clear output order list
      output_order.clear();
      // Clear current output parameters
      out_params.reset();
      out_params = nullptr;

      // wl_output is owned by the registry, released on disconnect
      // also cleanup associated output parameters and clear output list when done
      for (auto &[output, params] : outputs) {
        wl_output_destroy(output);
        params.reset();
      }
      outputs.clear();

      // Release wayland registry, display and reset pointers
      if (wl_registry) {
        wl_registry_destroy(wl_registry);
        wl_registry = nullptr;
      }
      if (wl_display) {
        wl_display_disconnect(wl_display);
        wl_display = nullptr;
      }
    }

    /**
     * @brief Connect to KWin wayland, enumerate outputs.
     * @param setup_permissions - Try to setup KWin permissions (default: true)
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int init(const bool setup_permissions = true) {
      if (setup_permissions) {
        // Try to set up permissions for zkde_screencast_unstable_v1
        screencast_permission_helper_t::setup();
      }

      std::string wl_name;
      if (!lizardbyte::common::get_env("WAYLAND_DISPLAY", wl_name)) {
        BOOST_LOG(error) << "[kwingrab] WAYLAND_DISPLAY not set"sv;
        return -1;
      }

      wl_display = wl_display_connect(wl_name.c_str());
      if (!wl_display) {
        BOOST_LOG(error) << "[kwingrab] cannot connect to Wayland display: "sv << wl_name;
        return -1;
      }

      wl_registry = wl_display_get_registry(wl_display);
      wl_registry_add_listener(wl_registry, &registry_listener, this);
      wl_display_roundtrip(wl_display);

      // MEOW-TOUCH(unified-desktop-capture): xdg_output is the only source of logical
      // geometry that stays correct under fractional scaling. The manager and the outputs
      // arrive in the same registry burst, so the objects can only be created afterwards.
      if (xdg_output_manager) {
        for (auto *output : outputs | std::views::keys) {
          auto *xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, output);
          xdg_outputs.emplace(xdg_output, outputs.at(output));
          zxdg_output_v1_add_listener(xdg_output, &xdg_output_listener, this);
        }
      }

      // We need a second roundtrip after binding outputs to get wl_output events
      wl_display_roundtrip(wl_display);

      // MEOW-TOUCH(unified-desktop-capture): a third roundtrip settles the xdg_output events
      // requested above. Only needed when xdg-output actually exists.
      if (xdg_output_manager) {
        wl_display_roundtrip(wl_display);
      }

      return 0;
    }

    /**
     * @brief Describe every known output for the pure union-geometry helper.
     * @return One entry per advertised wl_output.
     */
    std::vector<meow::display_union::output_geometry_t> collect_output_geometry() const {
      std::vector<meow::display_union::output_geometry_t> geometry;
      geometry.reserve(outputs.size());
      for (const auto &params : outputs | std::views::values) {
        meow::display_union::output_report_t report;
        report.name = params->name;
        report.wl_x = params->pos_x;
        report.wl_y = params->pos_y;
        report.mode_width = params->width;
        report.mode_height = params->height;
        report.transform = static_cast<meow::display_union::output_transform_t>(params->transform);
        report.wl_scale = params->wl_scale;
        report.refresh_mhz = params->refresh_mhz;
        report.has_xdg_logical_position = params->has_xdg_position;
        report.xdg_logical_x = params->xdg_logical_x;
        report.xdg_logical_y = params->xdg_logical_y;
        report.has_xdg_logical_size = params->logical_width > 0 && params->logical_height > 0;
        report.xdg_logical_width = params->logical_width;
        report.xdg_logical_height = params->logical_height;
        geometry.emplace_back(meow::display_union::describe_output(report));
      }
      return geometry;
    }

    /**
     * @brief Check if kwin screencasting is currently available
     * @return true if screencast can be started, false otherwise
     */
    bool is_kwin_screencasting_available() const {
      return kde_screencast_v1_ != nullptr;
    }

    /**
     * @brief Generate a sorted list of known output names.
     * @return List of strings with output names to pass to start()
     */
    std::vector<std::string> get_output_names() {
      std::vector<std::shared_ptr<output_parameter_t>> sorted_outputs;
      for (const auto &output_parameter : outputs | std::views::values) {
        output_parameter->order = get_order_for_output_name(output_parameter->name);
        sorted_outputs.emplace_back(output_parameter);
      }
      std::ranges::sort(sorted_outputs, [](const auto &a, const auto &b) {
        return a->order < b->order || a->pos_x < b->pos_x || a->pos_y < b->pos_y;
      });
      std::vector<std::string> output_names;
      for (const auto &output_parameter : sorted_outputs) {
        BOOST_LOG(info) << "[kwingrab] Found output: "sv << output_parameter->name << " order: "sv << output_parameter->order << " position: "sv << output_parameter->pos_x << "x"sv << output_parameter->pos_y << " resolution: "sv << output_parameter->width << "x"sv << output_parameter->height;
        output_names.emplace_back(output_parameter->name);
      }
      // MEOW-TOUCH(unified-desktop-capture): advertise the whole-desktop pseudo-display, but
      // last, so it can never become the implicit index-0 default. It is reachable two ways:
      // by setting `output_name` in the config, and — like every other entry in this list —
      // by a connected client pressing the display-switch hotkey (Ctrl+Alt+Shift+F1..F13),
      // which `input.cpp` turns into a `mail::switch_display` index. `video.cpp` clamps that
      // index into `[0, display_names.size() - 1]`, so the highest F-key selects whichever
      // entry is last, which is this one. That is bounded and intended, not a way for a
      // client to name an arbitrary output; it just means whole-desktop capture can be
      // toggled from the couch. Skipped when there is nothing to unify, or when the
      // compositor is too old for stream_region — in the latter case `start()` would only
      // fall back to a single output anyway, so advertising it would be a lie.
      if (!outputs.empty() && screencast_version_ >= meow::display_union::min_stream_region_version && std::ranges::none_of(output_names, [](const std::string &name) {
            return meow::display_union::is_union_output_name(name);
          })) {
        output_names.emplace_back(meow::display_union::union_output_name);
        BOOST_LOG(info) << "[kwingrab] Advertising whole-desktop pseudo-output: "sv << meow::display_union::union_output_name;
      }
      return output_names;
    }

    /**
     * @brief Check if KWin is available for potential screencasting
     * @return True if KWin is detected
     */
    bool kwin_available() const {
      // Detect KWin using kde_output_order_v1 extension
      if (kde_output_order) {
        return true;
      }
      return false;
    }

    /**
     * @brief Request a screencast stream.
     * @param output_name Which wl_output to capture.
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int start(const std::string_view &output_name) {
      // Try find correct output by name
      if (outputs.empty()) {
        BOOST_LOG(error) << "[kwingrab] no wl_output found"sv;
        return -1;
      }
      struct wl_output *output = nullptr;
      if (!output_name.empty()) {
        for (auto const &[output_, params_] : outputs) {
          if (params_->name == output_name) {
            output = output_;
            out_params = params_;
          }
        }
      }
      // MEOW-TOUCH(unified-desktop-capture): the reserved name streams every output as one
      // region. Real outputs are resolved above first, so a real connector could never be
      // shadowed by the reserved name. An unusable region falls through to single-output.
      meow::display_union::union_region_t region;
      if (!output && meow::display_union::is_union_output_name(output_name)) {
        region = prepare_union_region();
      }
      // Fall back to first element from the map in case of error
      if (!region.valid && (!output || !out_params)) {
        const auto output_ = outputs.begin();
        output = output_->first;
        out_params = output_->second;
      }

      // Request a stream for the chosen output with embedded cursor
      if (kde_screencast_v1_) {
        if (region.valid) {
          // MEOW-TOUCH(unified-desktop-capture): stream_region takes LOGICAL coordinates.
          kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_region(kde_screencast_v1_, region.x, region.y, static_cast<uint32_t>(region.width), static_cast<uint32_t>(region.height), wl_fixed_from_double(region.scale), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
        } else {
          kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_output(kde_screencast_v1_, output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
        }
        zkde_screencast_stream_unstable_v1_add_listener(kde_screencast_stream_v1_, &stream_listener, this);
      } else {
        // No screencast protocol found. Output an error based on newly initialized permission file.
        if (screencast_permission_helper_t::is_newly_initialized()) {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. "sv
                              "A new permission desktop file was automatically created but might now have been recognized yet. "sv
                              "Try restarting sunshine or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        } else {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. Check permission desktop file "sv
                              "for sunshine binary or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        }
        return -1;
      }

      if (wait_for_stream() < 0) {
        return -1;
      }

      if (stream_failed) {
        BOOST_LOG(error) << "[kwingrab] stream_output failed: "sv << stream_error_msg;
        return -1;
      }
      // Check for valid node_id and/or object serial values here, stream_ready is just an internal flag
      if (out_node_id == PW_ID_ANY && (out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(error) << "[kwingrab] timeout waiting for created event"sv;
        return -1;
      }

      if ((out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: node="sv << out_node_id;
      } else {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: objectserial="sv << out_objectserial << " (node="sv << out_node_id << ")"sv;
      }

      if (out_params->width == 0 || out_params->height == 0) {
        BOOST_LOG(error) << "[kwingrab] could not determine output dimensions"sv;
        return -1;
      }

      BOOST_LOG(info) << "[kwingrab] Screencasting output"sv
                      << " name "sv << out_params->name
                      << " position "sv << out_params->pos_x << "x"sv << out_params->pos_y
                      << " resolution "sv << out_params->width << "x"sv << out_params->height;
      return 0;
    }

    /**
     * @brief MEOW-TOUCH(unified-desktop-capture): build the whole-desktop region request.
     *
     * Computes the bounding box of every advertised output in logical coordinates and
     * installs a synthetic `output_parameter_t` describing it, so the rest of `start()` and
     * `configure_stream()` need no special-casing beyond reading the logical size back out.
     *
     * @return The computed region; `valid` is false when region capture cannot be used and
     *         the caller should fall back to single-output capture.
     */
    meow::display_union::union_region_t prepare_union_region() {
      using meow::display_union::union_status_t;

      const auto decision = meow::display_union::decide_union_capture(collect_output_geometry(), screencast_version_);
      const auto &region = decision.region;

      switch (decision.status) {
        case union_status_t::unsupported_protocol:
          BOOST_LOG(warning) << "[kwingrab] zkde_screencast_unstable_v1 version "sv << screencast_version_
                             << " does not provide stream_region (needs >= "sv << meow::display_union::min_stream_region_version
                             << "). Falling back to single-output capture."sv;
          return {};
        case union_status_t::no_usable_outputs:
          BOOST_LOG(warning) << "[kwingrab] Could not derive a whole-desktop region from the advertised outputs. Falling back to single-output capture."sv;
          return {};
        case union_status_t::exceeds_capture_limits:
          BOOST_LOG(error) << "[kwingrab] Whole-desktop region "sv << region.pixel_width << "x"sv << region.pixel_height
                           << " exceeds the "sv << meow::display_union::max_pixel_width << "x"sv << meow::display_union::max_pixel_height
                           << " capture limit. Falling back to single-output capture."sv;
          return {};
        case union_status_t::negative_origin:
          BOOST_LOG(error) << "[kwingrab] Whole-desktop region starts at "sv << region.x << ","sv << region.y
                           << ", left of / above the origin. Absolute pointer input is mapped from an origin-anchored "sv
                           << "desktop size, so the picture would be correct while the mouse landed elsewhere. Falling back "sv
                           << "to single-output capture. Move your outputs so the arrangement starts at 0,0 to use whole-desktop capture."sv;
          return {};
        case union_status_t::ok:
          break;
      }

      if (!region.covers_whole_region) {
        BOOST_LOG(info) << "[kwingrab] Outputs do not tile their bounding box; KWin renders the uncovered areas black."sv;
      }

      BOOST_LOG(info) << "[kwingrab] Whole-desktop region capture over "sv << region.contributing_outputs << " outputs:"sv
                      << " logical "sv << region.width << "x"sv << region.height
                      << " at "sv << region.x << ","sv << region.y
                      << " scale "sv << region.scale
                      << " -> capture resolution "sv << region.pixel_width << "x"sv << region.pixel_height
                      << " highest refresh "sv << (static_cast<double>(region.refresh_mhz) / 1000.0) << "Hz"sv;

      auto params = std::make_shared<output_parameter_t>();
      params->name = std::string(meow::display_union::union_output_name);
      params->pos_x = region.x;
      params->pos_y = region.y;
      params->width = region.pixel_width;
      params->height = region.pixel_height;
      params->logical_width = region.width;
      params->logical_height = region.height;
      params->refresh_mhz = region.refresh_mhz;
      params->is_union = true;
      out_params = std::move(params);
      return region;
    }

    uint32_t out_node_id = PW_ID_ANY;  ///< Out node ID.
    uint64_t out_objectserial = SPA_ID_INVALID;  ///< Out objectserial.
    std::shared_ptr<output_parameter_t> out_params = nullptr;  ///< Out params.

  private:
    // Wayland objects
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct kde_output_order_v1 *kde_output_order = nullptr;
    struct zkde_screencast_unstable_v1 *kde_screencast_v1_ = nullptr;
    struct zkde_screencast_stream_unstable_v1 *kde_screencast_stream_v1_ = nullptr;
    std::map<struct wl_output *, std::shared_ptr<output_parameter_t>> outputs;
    // MEOW-TOUCH(unified-desktop-capture): xdg-output objects and the bound screencast version.
    struct zxdg_output_manager_v1 *xdg_output_manager = nullptr;
    std::map<struct zxdg_output_v1 *, std::shared_ptr<output_parameter_t>> xdg_outputs;
    uint32_t screencast_version_ = 0;
    std::vector<std::string> output_order;
    bool stream_failed = false;
    bool stream_ready = false;
    std::string stream_error_msg;

    // Misc functions
    int wait_for_stream() {
      // Dispatch until we get created/failed, with a 5s timeout
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stream_ready && !stream_failed && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(wl_display);

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(wl_display);
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining.count() <= 0) {
          break;
        }

        if (poll(&pfd, 1, remaining.count()) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] wl_display_dispatch failed"sv;
          return -1;
        }
      }
      return 0;
    }

    size_t get_order_for_output_name(const std::string_view &name) const {
      for (size_t i = 0; i < output_order.size(); i++) {
        if (output_order[i] == name) {
          return i;
        }
      }
      // If nothing matches return list size (to ensure highest order)
      return output_order.size();
    }

    // Registry listener
    static void on_registry_global(void *data, struct wl_registry *reg, const uint32_t name, const char *interface, const uint32_t version) {
      auto *self = static_cast<screencast_t *>(data);
      if (!std::strcmp(interface, kde_output_order_v1_interface.name)) {
        // Bind version 1
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(1));
        self->kde_output_order = static_cast<struct kde_output_order_v1 *>(
          wl_registry_bind(reg, name, &kde_output_order_v1_interface, bind_ver)
        );
        kde_output_order_v1_add_listener(self->kde_output_order, &output_order_listener, self);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_order_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
        // Bind version 1 to 6 — We use stream_output from v1 for node_id (deprecated but good as a fall-back)
        //                       but also try to get the newer (re-use safe) pipewire objectserial from v6
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(6));
        self->kde_screencast_v1_ = static_cast<struct zkde_screencast_unstable_v1 *>(
          wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, bind_ver)
        );
        self->screencast_version_ = bind_ver;  // MEOW-TOUCH(unified-desktop-capture): stream_region needs >= 3
        BOOST_LOG(debug) << "[kwingrab] bound zkde_screencast_unstable_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        // MEOW-TOUCH(unified-desktop-capture): xdg-output carries the logical geometry that
        // stream_region needs and that wl_output cannot express under fractional scaling.
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(3));
        self->xdg_output_manager = static_cast<struct zxdg_output_manager_v1 *>(
          wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, bind_ver)
        );
        BOOST_LOG(debug) << "[kwingrab] bound zxdg_output_manager_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, wl_output_interface.name)) {
        // Bind version 4 - we need wl_output name for matching
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(4));
        auto *output = static_cast<struct wl_output *>(
          wl_registry_bind(reg, name, &wl_output_interface, bind_ver)
        );

        const auto [_, inserted] = self->outputs.try_emplace(output, std::make_shared<output_parameter_t>());
        if (inserted) {
          wl_output_add_listener(output, &output_listener, self);
          BOOST_LOG(debug) << "[kwingrab] bound wl_output version "sv << bind_ver << " instance: "sv << output;
        } else {
          // If we for some odd reason cannot add the output to the map clean it up and log a warning
          BOOST_LOG(warning) << "[kwingrab] Ignoring output "sv << output << " because map emplace failed."sv;
          wl_output_destroy(output);
        }
      }
    }

    static void on_registry_global_remove(void *data [[maybe_unused]], struct wl_registry *reg [[maybe_unused]], uint32_t name [[maybe_unused]]) {
      // We don't handle output hot-unplug during init
    }

    static constexpr struct wl_registry_listener registry_listener = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    // wl_output listener (for mode/dimensions/name)
    // MEOW-TOUCH(unified-desktop-capture): `transform` lost its [[maybe_unused]] because it is
    // now read. wl_output::mode reports the pre-transform scanout mode while
    // xdg_output::logical_size is post-transform, so without this a rotated output derives a
    // scale from two swapped axes. `pos_x`/`pos_y` are untouched and keep coming from here.
    static void on_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw [[maybe_unused]], int32_t ph [[maybe_unused]], int32_t subpixel [[maybe_unused]], const char *make [[maybe_unused]], const char *model [[maybe_unused]], int32_t transform) {
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->pos_x = x;
      output_parameter->pos_y = y;
      output_parameter->transform = transform;
    }

    // MEOW-TOUCH(unified-desktop-capture): `refresh` lost its [[maybe_unused]] because it is now read.
    static void on_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
      if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
        return;
      }
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->width = width;
      output_parameter->height = height;
      output_parameter->refresh_mhz = refresh;  // MEOW-TOUCH(unified-desktop-capture)
    }

    static void on_output_done(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_scale(void *data, struct wl_output *output, int32_t factor) {
      // MEOW-TOUCH(unified-desktop-capture): fallback scale for compositors without xdg-output.
      const auto *self = static_cast<screencast_t *>(data);
      self->outputs.at(output)->wl_scale = factor > 0 ? factor : 1;
    }

    static void on_output_name(void *data, struct wl_output *output, const char *name) {
      const auto *self = static_cast<screencast_t *>(data);
      self->outputs.at(output)->name = name;
    }

    static void on_output_description(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], const char *description [[maybe_unused]]) {
      // Currently unused
    }

    // MEOW-TOUCH(unified-desktop-capture): xdg-output listener — the authoritative source of
    // logical position and size, which is the coordinate space stream_region operates in.
    static void on_xdg_output_logical_position(void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
      const auto *self = static_cast<screencast_t *>(data);
      if (const auto entry = self->xdg_outputs.find(xdg_output); entry != self->xdg_outputs.end()) {
        // Stored beside `pos_x`/`pos_y`, never over them: those feed
        // display_t::offset_x/offset_y, which pipewire.cpp equality-matches against
        // wl::monitors(). Binding xdg-output for the union must stay invisible to
        // single-output capture. Only the union path reads these fields.
        entry->second->xdg_logical_x = x;
        entry->second->xdg_logical_y = y;
        entry->second->has_xdg_position = true;
      }
    }

    static void on_xdg_output_logical_size(void *data, struct zxdg_output_v1 *xdg_output, int32_t width, int32_t height) {
      const auto *self = static_cast<screencast_t *>(data);
      if (const auto entry = self->xdg_outputs.find(xdg_output); entry != self->xdg_outputs.end()) {
        entry->second->logical_width = width;
        entry->second->logical_height = height;
      }
    }

    static void on_xdg_output_done(void *data [[maybe_unused]], struct zxdg_output_v1 *xdg_output [[maybe_unused]]) {
      // Deprecated since xdg-output v3; wl_output::done is authoritative.
    }

    static void on_xdg_output_name(void *data [[maybe_unused]], struct zxdg_output_v1 *xdg_output [[maybe_unused]], const char *name [[maybe_unused]]) {
      // wl_output::name is already used for matching; nothing to do here.
    }

    static void on_xdg_output_description(void *data [[maybe_unused]], struct zxdg_output_v1 *xdg_output [[maybe_unused]], const char *description [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr struct zxdg_output_v1_listener xdg_output_listener = {
      .logical_position = on_xdg_output_logical_position,
      .logical_size = on_xdg_output_logical_size,
      .done = on_xdg_output_done,
      .name = on_xdg_output_name,
      .description = on_xdg_output_description,
    };

    static constexpr struct wl_output_listener output_listener = {
      .geometry = on_output_geometry,
      .mode = on_output_mode,
      .done = on_output_done,
      .scale = on_output_scale,
      .name = on_output_name,
      .description = on_output_description,
    };

    // Output order listener
    static void on_output_order_output(void *data, struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]], const char *output_name) {
      auto *self = static_cast<screencast_t *>(data);
      self->output_order.emplace_back(output_name);
    }

    static void on_output_order_done(void *data [[maybe_unused]], struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr kde_output_order_v1_listener output_order_listener = {
      .output = on_output_order_output,
      .done = on_output_order_done,
    };

    // ScreenCast v1 stream listener
    static void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]]) {
      auto *self = static_cast<screencast_t *>(data);
      BOOST_LOG(warning) << "[kwingrab] stream closed by server"sv;
      self->stream_failed = false;
      self->stream_ready = false;
      self->stream_error_msg = "stream closed by server";
    }

    static void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const uint32_t node) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_node_id = node;
      self->stream_failed = false;
      self->stream_ready = true;
      BOOST_LOG(debug) << "[kwingrab] created event, node_id="sv << node;
    }

    static void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const char *err_msg) {
      auto *self = static_cast<screencast_t *>(data);
      self->stream_failed = true;
      self->stream_ready = false;
      self->stream_error_msg = err_msg ? err_msg : "unknown error";
      BOOST_LOG(error) << "[kwingrab] failed event: "sv << self->stream_error_msg;
    }

    static void on_stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], uint32_t object_serial_hi, uint32_t object_serial_low) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_objectserial = static_cast<uint64_t>(object_serial_hi) << 32 | object_serial_low;
      // serial event always preceded the created event with the node id, so we only set stream_ready in created for v1
      BOOST_LOG(debug) << "[kwingrab] serial event, objectserial="sv << self->out_objectserial;
    }

    static constexpr struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
      .closed = on_stream_closed,
      .created = on_stream_created,
      .failed = on_stream_failed,
      .serial = on_stream_serial,
    };
  };

  /**
   * Display backend
   *
   * Orchestrates screencast_t and implements pipewire_display_t
   */
  class kwin_t: public pipewire::pipewire_display_t {
  public:
    /**
     * @brief MEOW-TOUCH(unified-desktop-capture): keep the union's logical size across a
     *        PipeWire resolution renegotiation.
     *
     * The base implementation recovers `logical_width`/`logical_height` by matching a single
     * `wl::monitors()` entry against this display's offset and size. A whole-desktop region
     * spans several monitors, so no entry can ever match and the values would silently stay
     * at 0 — which downstream (`video.cpp` touch-port setup) reads as "scaling unknown" and
     * mismaps absolute pointer input on any fractionally scaled desktop. Re-install the known
     * values first, then let the base fill in the desktop-wide fields as usual.
     */
    void verify_and_update_display_parameters() override {
      if (union_logical_width > 0 && union_logical_height > 0) {
        this->logical_width = union_logical_width;
        this->logical_height = union_logical_height;
      }
      pipewire::pipewire_display_t::verify_and_update_display_parameters();
    }

    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      screencast = std::make_unique<screencast_t>();
      if (screencast->init(true) < 0) {
        return -1;
      }
#if !defined(__FreeBSD__)
      // Check if KWin screencasting extension is accessible after first init attempt
      if (!screencast->is_kwin_screencasting_available()) {
        // KWin screencasting extension was not found. Drop ALL elevated privileges in case KWin is missing CAP_SYS_NICE
        BOOST_LOG(warning) << "[kwingrab] KWin screencasting unavailable after init. Trying again after dropping ALL elevated privileges."sv;
        platf::drop_elevated_privileges(true);
        // Retry screencast session init after privilege drop
        screencast.reset();  // Cleanup current screencast instance
        screencast = std::make_unique<screencast_t>();  // Create new screencast instance
        if (screencast->init(true) < 0) {
          return -1;
        }
      }
#endif
      if (screencast->start(display_name) < 0) {
        return -1;
      }
      if (screencast->out_params) {
        // Return values for pipewire init
        out_pipewire_fd = -1;  // KWin screencast capture runs on the local pipewire core
        out_pipewire_node = screencast->out_node_id;
        out_pipewire_objectserial = screencast->out_objectserial;
        // Set/update basic stream parameters on display_t
        this->offset_x = screencast->out_params->pos_x;
        this->offset_y = screencast->out_params->pos_y;
        this->width = screencast->out_params->width;
        this->height = screencast->out_params->height;
        // MEOW-TOUCH(unified-desktop-capture): the whole-desktop region has no backing
        // wl_output for pipewire_display_t to match on, so its logical size is reported here
        // and remembered for verify_and_update_display_parameters() above. Single-output
        // capture keeps the previous behaviour of leaving these at 0.
        union_logical_width = screencast->out_params->is_union ? screencast->out_params->logical_width : 0;
        union_logical_height = screencast->out_params->is_union ? screencast->out_params->logical_height : 0;
        this->logical_width = union_logical_width;  // Explicitly mark for pipewire_display_t to try to figure this out.
        this->logical_height = union_logical_height;  // Explicitly Mark for pipewire_display_t to try to figure this out.
        return 0;
      }
      return -1;
    }

    std::unique_ptr<screencast_t> screencast;  ///< Screencast.

  private:
    // MEOW-TOUCH(unified-desktop-capture): remembered union logical size, 0 for single-output.
    int union_logical_width = 0;
    int union_logical_height = 0;
  };
}  // namespace kwin

// Public API for misc.cpp
namespace platf {
  /**
   * @brief Create a KWin screencast display backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return KWin/PipeWire display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> kwin_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[kwingrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<kwin::kwin_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate KWin screencast display names.
   *
   * @return KWin display names, or an empty list when KWin capture is unavailable.
   */
  std::vector<std::string> kwin_display_names() {
    if (has_elevated_privileges(false)) {
      // We're still in the probing phase of Sunshine startup. Dropping portal security early will break KMS.
      // Just return a dummy screen for now. Display re-enumeration after encoder probing will yield full result.
      std::vector<std::string> display_names;
      display_names.emplace_back("");
      return display_names;
    }

    const auto screencast = std::make_unique<kwin::screencast_t>();
    if (screencast->init() < 0) {
      return {};
    }
    return screencast->get_output_names();
  }

  /**
   * @brief Check whether KWin screencast capture is available.
   *
   * @return True when KWin capture support is available.
   */
  bool kwin_available() {
    // Init screencast without permission setup (to not cause unneeded logs / temporary desktop files) and check KWin availability
    if (const auto screencast = std::make_unique<kwin::screencast_t>(); screencast->init(false) < 0 || !screencast->kwin_available()) {
      return false;
    }
    return true;
  }
}  // namespace platf
