#pragma once

#include <memory>
#include "strategy.h"

namespace Common {
class Logger;
}

namespace Trading {
class FeatureEngine;
class OrderManager;

// 只在启动时选择和创建策略；热路径不再判断策略名称。
std::unique_ptr<Strategy> createStrategy(Common::AlgoType type, Common::Logger* logger,
                                         const FeatureEngine* features, OrderManager* orders,
                                         const Common::TradeEngineCfgHashMap& config);
}
