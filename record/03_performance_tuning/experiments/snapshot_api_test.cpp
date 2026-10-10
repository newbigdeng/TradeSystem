#include <type_traits>
#include <utility>
#include "exchange/market_data/snapshot_synthesizer.h"
static_assert(std::is_same_v<decltype(std::declval<Exchange::SnapshotSynthesizer&>().publishSnapshot()),void>);
static_assert(std::is_same_v<decltype(std::declval<Exchange::SnapshotSynthesizer&>().addToSnapshot(static_cast<const Exchange::MDPMarketUpdate*>(nullptr))),void>);
int main(){return 0;}
