#include "test/test_framework.h"

#include "ble/smp.h"
#include "bluetooth/adapter_model.h"
#include "cli/serve_args.h"
#include "control/commands.h"
#include "hci/controller.h"
#include "hci/le_air.h"
#include "sim/simulator.h"
#include "usb/control.h"
#include "usbip/protocol.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pb;
using namespace pb::hci;

extern "C" int pb_arch_contracts_anchor() { return 1; }

static void send(MiniController& c, const std::vector<uint8_t>& cmd) {
  c.handle_command(cmd.data(), cmd.size());
}

static std::vector<uint8_t> cmd_of(uint16_t op) {
  return {(uint8_t)(op & 0xFF), (uint8_t)(op >> 8), 0x00};
}

static bool event_is_unknown(const std::vector<uint8_t>& e) {
  if (e.size() >= 6 && e[0] == EVT_COMMAND_COMPLETE && e[5] == ST_UNKNOWN_CMD) return true;
  if (e.size() >= 3 && e[0] == EVT_COMMAND_STATUS && e[2] == ST_UNKNOWN_CMD) return true;
  return false;
}

static bool connect_peer(MiniController& c, uint64_t addr, uint8_t addr_type) {
  c.air().add(std::make_shared<VirtualAdvertiser>(addr, "PhantomKb"));
  std::vector<uint8_t> cmd = {0x0D, 0x20, 24, 0x60, 0x00, 0x30, 0x00, 0x00, addr_type};
  for (int i = 0; i < 6; ++i) cmd.push_back((uint8_t)((addr >> (8 * i)) & 0xFF));
  for (int i = 0; i < 12; ++i) cmd.push_back(0);
  send(c, cmd);
  std::vector<uint8_t> e;
  while (c.pop_event(e)) {
  }
  for (int i = 0; i < 30; ++i) c.tick_for_tests((uint64_t)i * 16);
  bool ok = false;
  while (c.pop_event(e)) {
    if (e.size() >= 4 && e[0] == EVT_LE_META && e[2] == LE_EVT_CONNECTION_COMPLETE && e[3] == ST_SUCCESS)
      ok = true;
    if (!e.empty() && e[0] == EVT_ENCRYPTION_CHANGE) ok = false;
  }
  return ok && c.has_connection();
}

PB_TEST(arch_supported_commands_match_handlers) {
  MiniController c(ControllerConfig{});
  send(c, cmd_of(CMD_READ_LOCAL_SUPPORTED_COMMANDS));
  std::vector<uint8_t> bitmap;
  PB_CHECK(c.pop_event(bitmap));
  PB_CHECK_EQ(bitmap[0], EVT_COMMAND_COMPLETE);
  PB_CHECK(bitmap.size() >= 6 + 64);

  const uint16_t kEncryptFamily[] = {
      CMD_LE_ENCRYPT, CMD_LE_RAND, CMD_LE_START_ENCRYPTION, CMD_LE_LTK_REQUEST_REPLY,
      CMD_LE_LTK_REQUEST_NEGATIVE_REPLY,
  };
  for (uint16_t op : kEncryptFamily) {
    size_t pos = supported_commands_bit(op);
    PB_CHECK(pos < 512);
    PB_CHECK_EQ(bitmap[6 + pos / 8] & (1u << (pos % 8)), 0u);
    bool listed = false;
    for (uint16_t have : supported_command_list())
      if (have == op) listed = true;
    PB_CHECK(!listed);
  }

  for (uint16_t op : supported_command_list()) {
    size_t pos = supported_commands_bit(op);
    PB_CHECK(pos < 512);
    PB_CHECK((bitmap[6 + pos / 8] & (1u << (pos % 8))) != 0);
    MiniController one(ControllerConfig{});
    send(one, cmd_of(op));
    std::vector<uint8_t> e;
    PB_CHECK(one.pop_event(e));
    PB_CHECK(!event_is_unknown(e));
    while (one.pop_event(e)) PB_CHECK(e.empty() || e[0] != EVT_ENCRYPTION_CHANGE);
  }

  MiniController enc(ControllerConfig{});
  for (uint16_t op : kEncryptFamily) {
    send(enc, cmd_of(op));
    std::vector<uint8_t> e;
    PB_CHECK(enc.pop_event(e));
    PB_CHECK(event_is_unknown(e));
    PB_CHECK(e[0] != EVT_ENCRYPTION_CHANGE);
  }
  for (int i = 0; i < 8; ++i) enc.tick_for_tests((uint64_t)i * 16);
  std::vector<uint8_t> e;
  while (enc.pop_event(e)) PB_CHECK(e[0] != EVT_ENCRYPTION_CHANGE);

  send(c, cmd_of(CMD_READ_LOCAL_SUPPORTED_FEATURES));
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[6] & 0x04, 0u);  // BR/EDR Encryption 不宣告
  send(c, cmd_of(CMD_LE_READ_LOCAL_SUPPORTED_FEATURES));
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[6] & 0x01, 0u);  // LE Encryption 不宣告
}

PB_TEST(arch_smp_rejects_confirm_for_zero_addresses) {
  ble::SmpResponder smp;
  uint8_t central[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t periph[6] = {0x55, 0x44, 0x33, 0x22, 0x11, 0xC0};
  smp.set_connection_addresses(central, 0, periph, 1);
  PB_CHECK(smp.has_addresses());
  uint8_t ia[6], ra[6];
  ble::SmpResponder::pack_c1_address(central, ia);
  ble::SmpResponder::pack_c1_address(periph, ra);
  bool nonzero = false;
  for (int i = 0; i < 6; ++i) nonzero = nonzero || ia[i] || ra[i];
  PB_CHECK(nonzero);

  uint8_t preq[7] = {0x01, 0x03, 0x00, 0x00, 0x10, 0x03, 0x00};
  auto pres = smp.handle(preq, 7);
  PB_CHECK(pres.has_value());
  PB_CHECK_EQ(pres->size(), 7u);
  PB_CHECK_EQ((*pres)[4], 0x10);

  uint8_t mrand[16];
  for (int i = 0; i < 16; ++i) mrand[i] = (uint8_t)(0x50 + i);
  uint8_t tk[16] = {0};
  uint8_t zeros[6] = {0};
  auto wrong = ble::SmpResponder::c1(tk, mrand, preq, pres->data(), 0, zeros, 0, zeros);
  auto right = ble::SmpResponder::c1(tk, mrand, preq, pres->data(), smp.initiating_addr_type(),
                                     smp.initiating_addr(), smp.responding_addr_type(),
                                     smp.responding_addr());
  PB_CHECK(!std::equal(wrong.begin(), wrong.end(), right.begin()));

  std::vector<uint8_t> confirm{0x03};
  confirm.insert(confirm.end(), wrong.begin(), wrong.end());
  auto crsp = smp.handle(confirm.data(), confirm.size());
  PB_CHECK(crsp.has_value());
  PB_CHECK_EQ((*crsp)[0], 0x03);
  // 我方 confirm 也不得按全 0 地址计算
  uint8_t srand[16];
  for (int i = 0; i < 16; ++i) srand[i] = (uint8_t)(0x10 + i);
  auto ours_if_zero = ble::SmpResponder::c1(tk, srand, preq, pres->data(), 0, zeros, 0, zeros);
  PB_CHECK(!std::equal(ours_if_zero.begin(), ours_if_zero.end(), crsp->begin() + 1));

  std::vector<uint8_t> random{0x04};
  random.insert(random.end(), mrand, mrand + 16);
  auto rrsp = smp.handle(random.data(), random.size());
  PB_CHECK(rrsp.has_value());
  PB_CHECK_EQ(rrsp->size(), 2u);
  PB_CHECK_EQ((*rrsp)[0], 0x05);
  PB_CHECK_EQ((*rrsp)[1], 0x04);  // Confirm Value Failed
  PB_CHECK(!smp.paired());

  ble::SmpResponder bare;
  auto denied = bare.handle(confirm.data(), confirm.size());
  PB_CHECK(denied.has_value());
  PB_CHECK_EQ((*denied)[0], 0x05);
  PB_CHECK_EQ(denied->size(), 2u);
}

PB_TEST(arch_smp_accepts_confirm_for_real_addresses) {
  ble::SmpResponder smp;
  uint8_t central[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  uint8_t periph[6] = {0x55, 0x44, 0x33, 0x22, 0x11, 0xC0};
  smp.set_connection_addresses(central, 0, periph, 1);
  uint8_t preq[7] = {0x01, 0x04, 0x00, 0x01, 0x10, 0x03, 0x03};
  auto pres = smp.handle(preq, 7);
  PB_CHECK(pres.has_value());
  uint8_t mrand[16];
  for (int i = 0; i < 16; ++i) mrand[i] = (uint8_t)(0x80 + i);
  uint8_t tk[16] = {0};
  auto host_c = ble::SmpResponder::c1(tk, mrand, preq, pres->data(), smp.initiating_addr_type(),
                                      smp.initiating_addr(), smp.responding_addr_type(),
                                      smp.responding_addr());
  std::vector<uint8_t> confirm{0x03};
  confirm.insert(confirm.end(), host_c.begin(), host_c.end());
  PB_CHECK(smp.handle(confirm.data(), confirm.size()).has_value());
  std::vector<uint8_t> random{0x04};
  random.insert(random.end(), mrand, mrand + 16);
  auto rrsp = smp.handle(random.data(), random.size());
  PB_CHECK(rrsp.has_value());
  PB_CHECK_EQ((*rrsp)[0], 0x04);
  PB_CHECK_EQ(rrsp->size(), 17u);
  PB_CHECK(smp.paired());
}

PB_TEST(arch_le_create_connection_disallowed_when_connected) {
  MiniController c(ControllerConfig{});
  PB_CHECK(connect_peer(c, 0xC01122334455ull, 1));
  uint16_t handle = c.connection_handle();
  std::vector<uint8_t> again = {0x0D, 0x20, 24, 0x60, 0x00, 0x30, 0x00, 0x00, 0x01};
  for (int i = 0; i < 6; ++i) again.push_back(0x10 + i);
  for (int i = 0; i < 12; ++i) again.push_back(0);
  send(c, again);
  std::vector<uint8_t> e;
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[2], ST_CMD_DISALLOWED);
  PB_CHECK_EQ(e[4], 0x0D);
  PB_CHECK_EQ(e[5], 0x20);
  for (int i = 0; i < 30; ++i) c.tick_for_tests(1000 + (uint64_t)i * 16);
  while (c.pop_event(e)) {
    bool conn_complete = e.size() >= 4 && e[0] == EVT_LE_META && e[2] == LE_EVT_CONNECTION_COMPLETE &&
                         e[3] == ST_SUCCESS;
    PB_CHECK(!conn_complete);
  }
  PB_CHECK(c.has_connection());
  PB_CHECK_EQ(c.connection_handle(), handle);
}

PB_TEST(arch_le_read_channel_map_is_command_complete) {
  MiniController idle(ControllerConfig{});
  send(idle, {0x15, 0x20, 0x02, 0x01, 0x00});
  std::vector<uint8_t> e;
  PB_CHECK(idle.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
  PB_CHECK(e[0] != EVT_COMMAND_STATUS);
  PB_CHECK_EQ(e[5], ST_UNKNOWN_CONN_ID);
  PB_CHECK(e.size() >= 13u);  // status + handle + 5-byte map

  MiniController c(ControllerConfig{});
  PB_CHECK(connect_peer(c, 0xC01122334455ull, 1));
  uint16_t h = c.connection_handle();
  send(c, {0x15, 0x20, 0x02, (uint8_t)(h & 0xFF), (uint8_t)(h >> 8)});
  PB_CHECK(c.pop_event(e));
  PB_CHECK_EQ(e[0], EVT_COMMAND_COMPLETE);
  PB_CHECK_EQ(e[3], 0x15);
  PB_CHECK_EQ(e[4], 0x20);
  PB_CHECK_EQ(e[5], ST_SUCCESS);
  PB_CHECK_EQ(e[6], (uint8_t)(h & 0xFF));
  PB_CHECK_EQ(e[7], (uint8_t)(h >> 8));
  PB_CHECK_EQ(e.size(), 13u);
  PB_CHECK_EQ(e[8], 0xFF);
  PB_CHECK_EQ(e[11], 0xFF);
  PB_CHECK_EQ(e[12], 0x1F);
}

PB_TEST(arch_cli_serve_default_port_matches_gui) {
  auto d = cli::parse_serve_args({});
  PB_CHECK(d.ok);
  PB_CHECK_EQ(d.port, 3425);
  PB_CHECK_EQ(sim::Simulator::server_port_from_env(), 3425);
}

PB_TEST(arch_control_path_spaces_and_ble_type_words) {
  auto quoted = control::tokenize("load-hid \"/tmp/my profile.json\"");
  PB_CHECK_EQ(quoted.size(), 2u);
  PB_CHECK_EQ(quoted[1], std::string("/tmp/my profile.json"));
  auto words = control::tokenize("ble-type C0:11:22:33:44:55 hello world");
  PB_CHECK_EQ(words.size(), 4u);
  PB_CHECK_EQ(control::join_tokens(words, 2), std::string("hello world"));
  auto qtext = control::tokenize("ble-type C0:11:22:33:44:55 \"hello world\"");
  PB_CHECK_EQ(qtext.size(), 3u);
  PB_CHECK_EQ(qtext[2], std::string("hello world"));

  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "pb contract profile";
  fs::create_directories(dir);
  fs::path file = dir / "keyboard.json";
  {
    std::ofstream out(file);
    out << "{\n"
           "  \"template\": \"keyboard\",\n"
           "  \"vid\": \"0x1209\",\n"
           "  \"pid\": \"0xB001\",\n"
           "  \"product\": \"Phantom Keyboard\"\n"
           "}\n";
  }
  std::string path = file.string();
  PB_CHECK(path.find(' ') != std::string::npos);

  sim::Simulator sim;
  auto loaded = control::execute(sim, "load-hid \"" + path + "\"");
  PB_CHECK(!loaded.empty());
  PB_CHECK(loaded[0].rfind("OK ", 0) == 0);

  auto unquoted = control::execute(sim, "load-hid " + path);
  PB_CHECK(!unquoted.empty());
  PB_CHECK(unquoted[0].rfind("OK ", 0) == 0);

  auto typed = control::execute(sim, "ble-type C0:11:22:33:44:55 hello world");
  PB_CHECK(!typed.empty());
  PB_CHECK(typed[0].find("unknown command") == std::string::npos);
  PB_CHECK(typed[0].find("no bt adapter") != std::string::npos);

  auto quoted_type = control::execute(sim, "ble-type C0:11:22:33:44:55 \"hello world\"");
  PB_CHECK(quoted_type[0].find("unknown command") == std::string::npos);
}

PB_TEST(arch_sco_alt0_agrees_with_no_iso_urbs) {
  bt::AdapterModel m(ControllerConfig{}, 7, 1);
  const uint8_t get_cfg[8] = {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x00};
  auto cfg = m.on_control(usb::parse_setup(get_cfg), {});
  PB_CHECK_EQ(cfg.size(), 48u);
  int iso_endpoints = 0;
  int interfaces = 0;
  int empty_sco = 0;
  for (size_t i = 0; i + 1 < cfg.size();) {
    uint8_t len = cfg[i];
    if (len < 2 || i + len > cfg.size()) break;
    if (cfg[i + 1] == 0x04 && len >= 9) {
      ++interfaces;
      if (cfg[i + 2] == 1 && cfg[i + 3] == 0 && cfg[i + 4] == 0) ++empty_sco;
    }
    if (cfg[i + 1] == 0x05 && len >= 3 && (cfg[i + 3] & 0x03) == 0x01) ++iso_endpoints;
    i += len;
  }
  PB_CHECK_EQ(interfaces, 2);
  PB_CHECK_EQ(empty_sco, 1);
  PB_CHECK_EQ(iso_endpoints, 0);

  auto ret = usbip::pack_ret_submit(1, 2, 3, usbip::DIR_IN, 0, 0, nullptr, 0);
  PB_CHECK(ret.size() >= 48u);
  // number_of_packets 在 RET_SUBMIT 偏移 32，大端。传输层不支持 iso。
  PB_CHECK_EQ(ret[32], 0);
  PB_CHECK_EQ(ret[33], 0);
  PB_CHECK_EQ(ret[34], 0);
  PB_CHECK_EQ(ret[35], 0);
}
