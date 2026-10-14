#include "liquidity_taker.h"

namespace Trading {
LiquidityTaker::LiquidityTaker(Common::Logger* logger, const FeatureEngine* feature_engine,
                               OrderManager* order_manager, const TradeEngineCfgHashMap& ticker_cfg)
    : feature_engine_(feature_engine),
      order_manager_(order_manager),
      logger_(logger),
      ticker_cfg_(ticker_cfg) {}
}
