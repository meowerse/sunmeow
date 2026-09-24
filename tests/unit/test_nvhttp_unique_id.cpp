/**
 * @file tests/unit/test_nvhttp_unique_id.cpp
 * @brief Test host uniqueid recovery when the persisted state carries an unusable value.
 */

#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// lib includes
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "../certificate_test_utils.h"

#include <src/config.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>

namespace fs = std::filesystem;

/**
 * @brief Isolate uniqueid recovery tests from the user's Sunshine state.
 */
class UniqueIdRecoveryTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    original_unique_id = http::unique_id;
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "unique_id_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code error;
    fs::remove(state_file, error);
  }

  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code error;
    fs::remove(state_file, error);

    http::unique_id = original_unique_id;
    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

  /**
   * @brief Write a state file whose `root.uniqueid` holds @p value.
   */
  void write_state_with_unique_id(const std::string &value) const {
    std::ofstream out {state_file};
    ASSERT_TRUE(out.is_open());
    out << R"({"root":{"uniqueid":")" << value << R"(","named_devices":[]}})";
  }

  /**
   * @brief Write a state file holding @p unique_id and one enabled paired client.
   */
  void write_state_with_client(const std::string &unique_id, const std::string &cert_pem) const {
    std::string escaped;
    for (const char c : cert_pem) {
      if (c == '\n') {
        escaped += "\\n";
      } else if (c == '\r') {
        // dropped: JSON fixture keeps LF-only PEM
      } else {
        escaped += c;
      }
    }

    std::ofstream out {state_file};
    ASSERT_TRUE(out.is_open());
    out << R"({"root":{"uniqueid":")" << unique_id << R"(","named_devices":[{"name":"paired","cert":")"
        << escaped << R"(","uuid":"11111111-2222-3333-4444-555555555555","enabled":"true"}]}})";
  }

  /**
   * @brief Read `root.uniqueid` back from the state file on disk.
   */
  std::string persisted_unique_id() const {
    boost::property_tree::ptree tree;
    boost::property_tree::read_json(state_file.string(), tree);
    return tree.get<std::string>("root.uniqueid", "");
  }

  /**
   * @brief Read the state file's raw bytes.
   */
  std::string state_file_bytes() const {
    std::ifstream in {state_file, std::ios::binary};
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
  }

  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  std::string original_unique_id;  ///< Host uniqueid restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

/**
 * @brief An empty persisted uniqueid must be regenerated, not adopted.
 *
 * @details Regression: `get_optional` yields an engaged optional for `"uniqueid": ""`, so an
 * absence-only check left the id empty forever. Sunshine then served `<uniqueid/>`, which
 * Moonlight rejects as a missing mandatory field, making the host permanently unpairable.
 */
TEST_F(UniqueIdRecoveryTest, EmptyPersistedUniqueIdIsRegenerated) {
  write_state_with_unique_id("");
  http::unique_id.clear();

  nvhttp::test_support::reload_client_state();

  EXPECT_FALSE(http::unique_id.empty());
}

/**
 * @brief A usable persisted uniqueid must survive a reload unchanged.
 */
TEST_F(UniqueIdRecoveryTest, ValidPersistedUniqueIdIsPreserved) {
  const std::string expected = "B6FE1D89-0611-48CD-A751-15EFDB7437D8";
  write_state_with_unique_id(expected);
  http::unique_id.clear();

  nvhttp::test_support::reload_client_state();

  EXPECT_EQ(http::unique_id, expected);
}

/**
 * @brief A regenerated uniqueid must be distinct per recovery, not a fixed placeholder.
 */
TEST_F(UniqueIdRecoveryTest, RegeneratedUniqueIdIsNotAConstant) {
  write_state_with_unique_id("");
  http::unique_id.clear();
  nvhttp::test_support::reload_client_state();
  const std::string first = http::unique_id;

  write_state_with_unique_id("");
  http::unique_id.clear();
  nvhttp::test_support::reload_client_state();

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(http::unique_id.empty());
  EXPECT_NE(first, http::unique_id);
}

/**
 * @brief Recovering an empty uniqueid must NOT drop the paired-client list.
 *
 * @details Regression for a bug introduced by the first version of this fix: it regenerated the
 * id and then `return`ed, reusing the early-exit meant for a file with no credentials at all.
 * That skipped `named_devices` parsing, so every paired client vanished from memory -- and the
 * next save_state() would have written the empty list back, destroying the very pairings this
 * fix exists to protect. Identity recovery and client loading must both happen.
 */
TEST_F(UniqueIdRecoveryTest, EmptyUniqueIdRecoveryPreservesPairedClients) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Sunshine Paired Client");

  // A state file carrying a paired client alongside an unusable (empty) host identity.
  write_state_with_client("", credentials.x509);
  http::unique_id.clear();
  nvhttp::test_support::reset_client_state();

  nvhttp::test_support::reload_client_state();

  EXPECT_FALSE(http::unique_id.empty()) << "identity was not recovered";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509))
    << "paired client was dropped while recovering the uniqueid";
}

/**
 * @brief A recovered uniqueid must be written back, so the host keeps one identity.
 *
 * @details Regression: the first version of this fix regenerated the id in memory only.
 * `save_state()` runs only on pair/unpair/enable changes, so a host whose paired clients keep
 * streaming never rewrote the file: it kept `""`, and every restart served a NEW uuid. Moonlight
 * identifies a host by that uuid and rejects a poll that returns a different one, so the paired
 * clients this fix protects would have lost the host on every restart instead.
 */
TEST_F(UniqueIdRecoveryTest, RecoveredUniqueIdIsPersistedAndStableAcrossReloads) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Sunshine Paired Client");
  write_state_with_client("", credentials.x509);
  http::unique_id.clear();

  nvhttp::test_support::reload_client_state();
  const std::string recovered = http::unique_id;

  ASSERT_FALSE(recovered.empty());
  EXPECT_EQ(persisted_unique_id(), recovered) << "the recovered id was not written to the state file";

  http::unique_id.clear();
  nvhttp::test_support::reload_client_state();

  EXPECT_EQ(http::unique_id, recovered) << "the host identity changed across a restart";
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509))
    << "persisting the recovered id dropped the paired client";
}

/**
 * @brief An unreadable state file must still leave the host with a usable uniqueid.
 *
 * @details `read_json` failing used to return with `http::unique_id` still empty, so
 * /serverinfo served `<uniqueid/>` -- the same unpairable state as an empty persisted id. The
 * file itself must be left alone: it may be recoverable by hand, and overwriting it would
 * destroy whatever pairings it still holds.
 */
TEST_F(UniqueIdRecoveryTest, UnreadableStateFileStillYieldsAUniqueIdAndIsNotOverwritten) {
  {
    std::ofstream out {state_file};
    ASSERT_TRUE(out.is_open());
    out << R"({"root":{"uniqueid":"B6FE1D89-0611-48CD-A751-15EFDB7437D8","named_devices":[)";  // truncated
  }
  const std::string before = state_file_bytes();
  http::unique_id.clear();

  nvhttp::test_support::reload_client_state();

  EXPECT_FALSE(http::unique_id.empty());
  EXPECT_EQ(state_file_bytes(), before) << "an unreadable state file was rewritten";
}
