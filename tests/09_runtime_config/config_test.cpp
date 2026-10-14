#include "common/runtime_config.h"

#include <chrono>
#include <iostream>
#include <functional>

namespace {
int checks = 0;

void check(bool condition, const std::string& label) {
  ++checks;
  if (!condition)
    throw std::runtime_error("FAIL: " + label);
}

template <typename F>
void rejects(const std::string& label, F&& operation) {
  try {
    operation();
  } catch (const Common::ConfigError&) {
    check(true, label);
    return;
  }
  check(false, label + " was accepted");
}

template <typename T>
std::string json(const T& config) {
  std::string buffer;
  const auto error = glz::write_json(config, buffer);
  check(!error, "test fixture serialization");
  return buffer;
}

std::string replace(std::string input, const std::string& old_value, const std::string& new_value) {
  const auto offset = input.find(old_value);
  check(offset != std::string::npos, "fixture replacement " + old_value);
  input.replace(offset, old_value.size(), new_value);
  return input;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("usage: config_test EXCHANGE_JSON CLIENT_JSON");
    const auto exchange = Common::loadExchangeConfig(argv[1]);
    const auto client = Common::loadClientConfig(argv[2]);
    check(exchange.market_data.incremental.port == 20001 &&
              exchange.market_data.incremental.address == "233.252.14.3",
          "exchange incremental endpoint");
    check(exchange.market_data.snapshot.port == 20000 &&
              exchange.market_data.snapshot.address == "233.252.14.1",
          "exchange snapshot endpoint");
    check(exchange.tcp.address == "127.0.0.1", "exchange tcp address");
    check(client.client_id == 1 && client.strategyType() == Common::AlgoType::MAKER,
          "client selection");
    const auto defaults = client.tickerConfig();
    for (const auto& ticker : defaults)
      check(ticker.clip_ == 10 && ticker.threshold_ == 1.0 &&
                ticker.risk_cfg_.max_order_size_ == 100 && ticker.risk_cfg_.max_position_ == 1000 &&
                ticker.risk_cfg_.max_loss_ == -1000000.0,
            "default parameters preserved");

    auto custom = client;
    custom.strategy.type = "taker";
    custom.strategy.tickers.push_back({2, {20, 2.5, {200, 2000, -2000000.0}}});
    custom.runtime.reset();
    auto parsed = Common::parseClientConfig(json(custom));
    check(parsed.strategyType() == Common::AlgoType::TAKER, "case insensitive strategy");
    auto parameters = parsed.tickerConfig();
    check(parameters[2].clip_ == 20 && parameters[2].threshold_ == 2.5 && parameters[1].clip_ == 10,
          "ticker override only affects selected ticker");
    check(parsed.runtimeConfig().random_orders == 10000 && parsed.runtimeConfig().run_seconds == 0,
          "optional runtime defaults");
    custom.strategy.type = "random";
    check(Common::parseClientConfig(json(custom)).strategyType() == Common::AlgoType::RANDOM,
          "random load mode");
    custom = client;
    custom.client_id = 255;
    custom.strategy.default_parameters.risk.max_position = 0;
    custom.runtime = Common::RuntimeConfig{86400, 900000, 1000000};
    check(Common::parseClientConfig(json(custom)).client_id == 255,
          "valid upper bounds and zero position");
    auto wildcard = exchange;
    wildcard.tcp.address = "0.0.0.0";
    check(Common::parseExchangeConfig(json(wildcard)).tcp.address == "0.0.0.0",
          "wildcard server bind");

    const auto original = json(client);
    check(Common::parseClientConfig("\xEF\xBB\xBF" + original).client_id == 1, "UTF-8 BOM");
    check(Common::parseClientConfig(original + " \r\n\t").client_id == 1, "trailing whitespace");
    auto override_config = client;
    override_config.strategy.default_parameters.clip = 1000;
    rejects("manual CLI override validation",
            [&] { Common::validateClientConfig(override_config); });
    rejects("malformed JSON", [] { Common::parseClientConfig("{broken"); });
    rejects("unknown key", [&] {
      Common::parseClientConfig(replace(original, "\"client_id\":", "\"client_idx\":"));
    });
    rejects("missing nested key",
            [&] { Common::parseClientConfig(replace(original, "\"clip\":10,", "")); });
    rejects("missing root key",
            [&] { Common::parseClientConfig(replace(original, "\"client_id\":1,", "")); });
    rejects("missing overrides array",
            [&] { Common::parseClientConfig(replace(original, ",\"tickers\":[]", "")); });
    rejects("wrong number type", [&] {
      Common::parseClientConfig(replace(original, "\"client_id\":1", "\"client_id\":\"1\""));
    });
    rejects("negative unsigned field", [&] {
      Common::parseClientConfig(replace(original, "\"client_id\":1", "\"client_id\":-1"));
    });
    rejects("non-finite number", [&] {
      Common::parseClientConfig(replace(original, "\"threshold\":1", "\"threshold\":NaN"));
    });
    rejects("trailing JSON document", [&] { Common::parseClientConfig(original + "{}"); });
    rejects("trailing garbage", [&] { Common::parseClientConfig(original + "oops"); });
    rejects("missing file",
            [] { Common::loadClientConfig("/this/path/does/not/exist/config.json"); });

    const auto reject_client = [&](const std::string& name,
                                   const std::function<void(Common::ClientConfig&)>& change) {
      auto bad = client;
      change(bad);
      rejects(name, [&] { Common::parseClientConfig(json(bad)); });
    };
    reject_client("zero client id", [](auto& c) { c.client_id = 0; });
    reject_client("large client id", [](auto& c) { c.client_id = 256; });
    reject_client("unknown strategy", [](auto& c) { c.strategy.type = "UNKNOWN"; });
    reject_client("invalid enum name", [](auto& c) { c.strategy.type = "INVALID"; });
    reject_client("zero port", [](auto& c) { c.tcp.port = 0; });
    reject_client("large port", [](auto& c) { c.market_data.snapshot.port = 65536; });
    reject_client("same udp port",
                  [](auto& c) { c.market_data.snapshot.port = c.market_data.incremental.port; });
    reject_client("unicast market stream",
                  [](auto& c) { c.market_data.incremental.address = "127.0.0.1"; });
    reject_client("bad address",
                  [](auto& c) { c.market_data.incremental.address = "233.999.1.1"; });
    reject_client("ipv6 address", [](auto& c) { c.tcp.address = "::1"; });
    reject_client("nonexistent interface",
                  [](auto& c) { c.market_data.interface = "does_not_exist"; });
    reject_client("empty interface", [](auto& c) { c.market_data.interface.clear(); });
    reject_client("zero remote address", [](auto& c) { c.tcp.address = "0.0.0.0"; });
    reject_client("multicast tcp address", [](auto& c) { c.tcp.address = "233.252.14.1"; });
    reject_client("broadcast tcp address", [](auto& c) { c.tcp.address = "255.255.255.255"; });
    reject_client("zero clip", [](auto& c) { c.strategy.default_parameters.clip = 0; });
    reject_client("clip exceeds risk", [](auto& c) { c.strategy.default_parameters.clip = 101; });
    reject_client("risk exceeds signed arithmetic",
                  [](auto& c) { c.strategy.default_parameters.risk.max_order_size = 2147483648u; });
    reject_client("position exceeds signed arithmetic",
                  [](auto& c) { c.strategy.default_parameters.risk.max_position = 2147483648u; });
    reject_client("negative threshold",
                  [](auto& c) { c.strategy.default_parameters.threshold = -0.1; });
    reject_client("positive loss",
                  [](auto& c) { c.strategy.default_parameters.risk.max_loss = 1; });
    reject_client("invalid ticker id",
                  [](auto& c) { c.strategy.tickers = {{8, c.strategy.default_parameters}}; });
    reject_client("duplicate ticker id", [](auto& c) {
      c.strategy.tickers = {{0, c.strategy.default_parameters}, {0, c.strategy.default_parameters}};
    });
    reject_client("bad ticker parameters", [](auto& c) {
      auto p = c.strategy.default_parameters;
      p.clip = 0;
      c.strategy.tickers = {{0, p}};
    });
    reject_client("run duration limit", [](auto& c) { c.runtime->run_seconds = 86401; });
    reject_client("order count limit", [](auto& c) { c.runtime->random_orders = 900001; });
    reject_client("delay limit", [](auto& c) { c.runtime->random_delay_us = 1000001; });
    std::cout << "CONFIG_CONTRACT_PASS checks=" << checks << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
