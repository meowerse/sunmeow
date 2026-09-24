/**
 * @file tests/unit/meow/test_tailnet_network.cpp
 * @brief Pin how src/network.cpp classifies Tailscale (CGNAT) addresses.
 *
 * sunmeow is deployed over Tailscale: host and phone reach each other on 100.64.0.0/10
 * (IPv4) and fd7a:115c:a1e0::/48 (IPv6). Two decisions hang off `net::from_address()` for
 * those peers, and each silently changes behaviour if the classification ever moves:
 *
 *  - `net::encryption_mode_for_address()` picks `lan_encryption_mode` for PC/LAN peers and
 *    `wan_encryption_mode` for everything else (RTSP setup, `/launch`, `/resume`).
 *  - `confighttp` refuses the web UI when `from_address()` exceeds `origin_web_ui_allowed`,
 *    whose default is `lan` -- so a tailnet address classified WAN locks the user out of
 *    the web UI they reach over the tailnet.
 *  - Video packet size is NOT one of them: it is negotiated by the client
 *    (`x-nv-video[0].packetSize`) and capped only by the host's `packetsize` option; the
 *    address class never enters it, so there is nothing to pin for it here. Whether packets
 *    fit the 1280-byte tailnet MTU is decided on the CLIENT, by moonlight-common-c
 *    (`Connection.c`): over IPv4 it treats 100.64.0.0/10 as remote and caps packets at 1024,
 *    which fits; over IPv6 it treats fd7a:115c:a1e0::/48 (inside fc00::/7) as LOCAL and sends
 *    no cap, so the client's default packet size exceeds the tailnet MTU. A host reached over
 *    Tailscale IPv6 (e.g. a MagicDNS AAAA record) needs `packetsize = 1184` in its config --
 *    see README.meow.md.
 *
 * These are upstream decisions, pinned from a meow test so a sync that reshuffles the
 * range tables cannot change them unnoticed.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <string>
#include <tuple>

// local includes
#include <src/config.h>
#include <src/network.h>

namespace {

  /**
   * @brief One address and the network class it must resolve to.
   */
  struct MeowTailnetClassification: BaseTest, testing::WithParamInterface<std::tuple<std::string, net::net_e>> {};

  TEST_P(MeowTailnetClassification, FromAddressClassifiesTailnetRange) {
    const auto &[address, expected] = GetParam();
    EXPECT_EQ(net::from_address(address), expected) << address;
  }

  INSTANTIATE_TEST_SUITE_P(
    MeowTailnetNetwork,
    MeowTailnetClassification,
    testing::Values(
      // The live deployment's host and phone.
      std::make_tuple("100.118.62.58", net::LAN),
      std::make_tuple("100.115.87.2", net::LAN),
      // Both ends of 100.64.0.0/10, and one address either side of it.
      std::make_tuple("100.64.0.1", net::LAN),
      std::make_tuple("100.127.255.254", net::LAN),
      std::make_tuple("100.63.255.255", net::WAN),
      std::make_tuple("100.128.0.1", net::WAN),
      // An IPv4-mapped tailnet address is normalized before classification.
      std::make_tuple("::ffff:100.115.87.2", net::LAN),
      // Tailscale's IPv6 ULA prefix sits inside fc00::/7.
      std::make_tuple("fd7a:115c:a1e0::1", net::LAN),
      std::make_tuple("fd7a:115c:a1e0:ab12:4843:cd96:6274:3a02", net::LAN)
    )
  );

  /**
   * @brief Save and restore the two encryption policies around each test.
   */
  class MeowTailnetEncryption: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      saved_lan = config::stream.lan_encryption_mode;
      saved_wan = config::stream.wan_encryption_mode;
      // Distinct values, so a test can tell which policy was chosen.
      config::stream.lan_encryption_mode = config::ENCRYPTION_MODE_OPPORTUNISTIC;
      config::stream.wan_encryption_mode = config::ENCRYPTION_MODE_MANDATORY;
    }

    void TearDown() override {
      config::stream.lan_encryption_mode = saved_lan;
      config::stream.wan_encryption_mode = saved_wan;
      BaseTest::TearDown();
    }

    int saved_lan {};  ///< `lan_encryption_mode` restored after the test.
    int saved_wan {};  ///< `wan_encryption_mode` restored after the test.
  };

  TEST_F(MeowTailnetEncryption, TailnetPeersGetTheLanPolicy) {
    for (const char *peer : {"100.115.87.2", "100.64.0.1", "fd7a:115c:a1e0::1"}) {
      EXPECT_EQ(net::encryption_mode_for_address(boost::asio::ip::make_address(peer)), config::ENCRYPTION_MODE_OPPORTUNISTIC) << peer;
    }
  }

  TEST_F(MeowTailnetEncryption, AddressesOutsideTheTailnetRangeGetTheWanPolicy) {
    for (const char *peer : {"100.128.0.1", "100.63.255.255", "8.8.8.8"}) {
      EXPECT_EQ(net::encryption_mode_for_address(boost::asio::ip::make_address(peer)), config::ENCRYPTION_MODE_MANDATORY) << peer;
    }
  }

  TEST_F(MeowTailnetEncryption, TailnetPeersPassTheDefaultWebUiOriginCheck) {
    // confighttp denies when from_address(peer) > origin_web_ui_allowed; the default is "lan".
    const auto allowed = net::from_enum_string("lan");
    EXPECT_FALSE(net::from_address("100.115.87.2") > allowed);
    EXPECT_TRUE(net::from_address("100.128.0.1") > allowed);
  }

}  // namespace
