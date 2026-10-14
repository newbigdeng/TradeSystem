#pragma once

#include <cmath>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "common/socket_utils.h"
#include "common/types.h"

namespace Common {

// JSON 仅在启动时读取；工作线程使用校验完成后的普通配置值。
struct NetworkEndpoint {
  std::string address;
  uint32_t port = 0;
};

struct MarketDataConfig {
  std::string interface;
  NetworkEndpoint incremental;
  NetworkEndpoint snapshot;
};

struct ExchangeConfig {
  MarketDataConfig market_data;
  NetworkEndpoint tcp;
};

struct RuntimeRiskConfig {
  uint32_t max_order_size = 0;
  uint32_t max_position = 0;
  double max_loss = 0;
};

struct StrategyParameters {
  uint32_t clip = 0;
  double threshold = 0;
  RuntimeRiskConfig risk;
};

struct TickerStrategyConfig {
  uint32_t ticker_id = 0;
  StrategyParameters parameters;
};

struct StrategyConfig {
  std::string type;
  StrategyParameters default_parameters;
  std::vector<TickerStrategyConfig> tickers;
};

// RANDOM 是测试负载模式；运行期限和负载强度不属于策略回调热路径。
struct RuntimeConfig {
  uint64_t run_seconds = 0;
  uint64_t random_orders = 10000;
  uint64_t random_delay_us = 20000;
};

struct ClientConfig {
  uint32_t client_id = 0;
  MarketDataConfig market_data;
  NetworkEndpoint tcp;
  StrategyConfig strategy;
  std::optional<RuntimeConfig> runtime;

  AlgoType strategyType() const;
  TradeEngineCfgHashMap tickerConfig() const;

  RuntimeConfig runtimeConfig() const {
    return runtime.value_or(RuntimeConfig{});
  }
};

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace Common

// 显式声明配置字段，字段拼错或必填字段缺失时立即报告错误。
template <>
struct glz::meta<Common::NetworkEndpoint> {
  using T = Common::NetworkEndpoint;
  static constexpr auto value = object("address", &T::address, "port", &T::port);
};

template <>
struct glz::meta<Common::MarketDataConfig> {
  using T = Common::MarketDataConfig;
  static constexpr auto value =
      object("interface", &T::interface, "incremental", &T::incremental, "snapshot", &T::snapshot);
};

template <>
struct glz::meta<Common::ExchangeConfig> {
  using T = Common::ExchangeConfig;
  static constexpr auto value = object("market_data", &T::market_data, "tcp", &T::tcp);
};

template <>
struct glz::meta<Common::RuntimeRiskConfig> {
  using T = Common::RuntimeRiskConfig;
  static constexpr auto value = object("max_order_size", &T::max_order_size, "max_position",
                                       &T::max_position, "max_loss", &T::max_loss);
};

template <>
struct glz::meta<Common::StrategyParameters> {
  using T = Common::StrategyParameters;
  static constexpr auto value =
      object("clip", &T::clip, "threshold", &T::threshold, "risk", &T::risk);
};

template <>
struct glz::meta<Common::TickerStrategyConfig> {
  using T = Common::TickerStrategyConfig;
  static constexpr auto value = object("ticker_id", &T::ticker_id, "parameters", &T::parameters);
};

template <>
struct glz::meta<Common::StrategyConfig> {
  using T = Common::StrategyConfig;
  static constexpr auto value = object("type", &T::type, "default_parameters",
                                       &T::default_parameters, "tickers", &T::tickers);
};

template <>
struct glz::meta<Common::RuntimeConfig> {
  using T = Common::RuntimeConfig;
  static constexpr auto value = object("run_seconds", &T::run_seconds, "random_orders",
                                       &T::random_orders, "random_delay_us", &T::random_delay_us);
};

template <>
struct glz::meta<Common::ClientConfig> {
  using T = Common::ClientConfig;
  static constexpr auto value =
      object("client_id", &T::client_id, "market_data", &T::market_data, "tcp", &T::tcp, "strategy",
             &T::strategy, "runtime", &T::runtime);
};

namespace Common {
namespace ConfigDetail {

inline void require(bool condition, const std::string& message) {
  if (!condition)
    throw ConfigError(message);
}

inline in_addr ipv4(const NetworkEndpoint& endpoint, const std::string& field) {
  in_addr address{};
  require(inet_pton(AF_INET, endpoint.address.c_str(), &address) == 1,
          field + ".address 必须是数字 IPv4 地址");
  require(endpoint.port >= 1 && endpoint.port <= 65535, field + ".port 必须在 1 到 65535 之间");
  return address;
}

inline void validate(const MarketDataConfig& market) {
  require(!market.interface.empty() && !getIfaceIP(market.interface).empty(),
          "market_data.interface 必须是本机具有 IPv4 地址的网卡");
  const auto incremental = ipv4(market.incremental, "market_data.incremental");
  const auto snapshot = ipv4(market.snapshot, "market_data.snapshot");
  require(IN_MULTICAST(ntohl(incremental.s_addr)), "增量行情地址必须是 IPv4 组播地址");
  require(IN_MULTICAST(ntohl(snapshot.s_addr)), "快照行情地址必须是 IPv4 组播地址");
  // Linux 默认可能把同端口的其他组播也交给接收 socket；两条流使用不同端口。
  require(market.incremental.port != market.snapshot.port,
          "增量行情与快照行情必须使用不同的 UDP 端口");
}

inline void validateTCP(const NetworkEndpoint& endpoint, bool server) {
  const auto address = ipv4(endpoint, "tcp");
  const auto host = ntohl(address.s_addr);
  require(!IN_MULTICAST(host) && host != INADDR_BROADCAST,
          "tcp.address 必须是单播地址，监听端也可使用 0.0.0.0");
  require(server || host != INADDR_ANY, "客户端 tcp.address 不能是 0.0.0.0");
}

inline void validate(const StrategyParameters& parameters) {
  constexpr auto max_signed_quantity = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  require(parameters.clip > 0 && parameters.clip <= parameters.risk.max_order_size &&
              parameters.risk.max_order_size <= max_signed_quantity,
          "策略 clip 必须为正数且不超过 max_order_size，数量上限不能超过 INT32_MAX");
  require(parameters.risk.max_position <= max_signed_quantity, "max_position 不能超过 INT32_MAX");
  require(std::isfinite(parameters.threshold) && parameters.threshold >= 0,
          "threshold 必须是有限的非负数");
  require(std::isfinite(parameters.risk.max_loss) && parameters.risk.max_loss <= 0,
          "max_loss 必须是有限的非正数");
}

inline void validate(const ExchangeConfig& config) {
  validate(config.market_data);
  validateTCP(config.tcp, true);
}

inline void validate(const ClientConfig& config) {
  require(config.client_id > 0 && config.client_id < ME_MAX_NUM_CLIENTS,
          "client_id 必须在 1 到 255 之间");
  validate(config.market_data);
  validateTCP(config.tcp, false);
  require(config.strategyType() != AlgoType::INVALID,
          "strategy.type 仅支持 MAKER、TAKER 或 RANDOM");
  validate(config.strategy.default_parameters);
  std::array<bool, ME_MAX_TICKERS> seen{};
  for (const auto& ticker : config.strategy.tickers) {
    require(ticker.ticker_id < ME_MAX_TICKERS, "ticker_id 必须在 0 到 7 之间");
    require(!seen[ticker.ticker_id], "同一个 ticker_id 不能重复配置");
    seen[ticker.ticker_id] = true;
    validate(ticker.parameters);
  }
  const auto runtime = config.runtimeConfig();
  require(runtime.run_seconds <= 86400, "run_seconds 不能超过 86400 秒");
  require(runtime.random_orders <= 900000, "random_orders 不能超过 900000");
  require(runtime.random_delay_us <= 1000000, "random_delay_us 不能超过 1000000 微秒");
}

inline TradeEngineCfg tickerConfig(const StrategyParameters& parameters) {
  return {parameters.clip,
          parameters.threshold,
          {parameters.risk.max_order_size, parameters.risk.max_position, parameters.risk.max_loss}};
}

template <typename T>
inline T parse(std::string buffer) {
  T config{};
  // 兼容 Windows 编辑器写入的 UTF-8 BOM；JSON 内容仍由 Glaze 解析。
  if (buffer.starts_with("\xEF\xBB\xBF"))
    buffer.erase(0, 3);
  // 严格 JSON、完整字段名比较、拒绝未知/缺失字段；不接受 NaN、Inf 或尾随内容。
  constexpr glz::opts options{.error_on_unknown_keys = true,
                              .use_hash_comparison = false,
                              .force_conformance = true,
                              .error_on_missing_keys = true};
  const auto error = glz::read<options>(config, buffer);
  if (error)
    throw ConfigError("JSON 配置格式错误：\n" + glz::format_error(error, buffer));
  validate(config);
  return config;
}

template <typename T>
inline T load(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw ConfigError("无法打开配置文件：" + path);
  std::string buffer{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad())
    throw ConfigError("读取配置文件失败：" + path);
  try {
    return parse<T>(std::move(buffer));
  } catch (const ConfigError& error) {
    throw ConfigError(path + "：" + error.what());
  }
}

}  // namespace ConfigDetail

inline AlgoType ClientConfig::strategyType() const {
  auto type = strategy.type;
  for (auto& character : type)
    character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  const auto algo = stringToAlgoType(type);
  return algo == AlgoType::MAKER || algo == AlgoType::TAKER || algo == AlgoType::RANDOM
             ? algo
             : AlgoType::INVALID;
}

inline TradeEngineCfgHashMap ClientConfig::tickerConfig() const {
  TradeEngineCfgHashMap config;
  config.fill(ConfigDetail::tickerConfig(strategy.default_parameters));
  for (const auto& ticker : strategy.tickers)
    config[ticker.ticker_id] = ConfigDetail::tickerConfig(ticker.parameters);
  return config;
}

inline ExchangeConfig parseExchangeConfig(std::string buffer) {
  return ConfigDetail::parse<ExchangeConfig>(std::move(buffer));
}

inline ClientConfig parseClientConfig(std::string buffer) {
  return ConfigDetail::parse<ClientConfig>(std::move(buffer));
}

inline ExchangeConfig loadExchangeConfig(const std::string& path) {
  return ConfigDetail::load<ExchangeConfig>(path);
}

inline ClientConfig loadClientConfig(const std::string& path) {
  return ConfigDetail::load<ClientConfig>(path);
}

inline void validateExchangeConfig(const ExchangeConfig& config) {
  ConfigDetail::validate(config);
}

inline void validateClientConfig(const ClientConfig& config) {
  ConfigDetail::validate(config);
}

}  // namespace Common
