// 恢复状态机：缓存同一快照周期，验证连续序号、每品种 CLEAR、首尾水位与订单簿哈希，再接上连续增量。
// build 只产生候选；整批进入下游队列后才 commit。缓存有上限，溢出重新等待完整快照。
// TRADE 是成交通知，不扣订单簿；MODIFY 设置绝对剩余量，CANCEL 删除订单，避免重复扣量。

#pragma once // 防止头文件重复包含。
#include <map> // 引入按键有序的缓存。
#include <optional> // 引入可缺省的恢复候选。
#include "common/book_hash.h" // 引入common/book_hash.h 的声明。
namespace Trading { // 所属命名空间。
class MarketRecovery { // 定义MarketRecovery 的接口和状态。
 public: // 对外接口。
  using Update=Exchange::MDPMarketUpdate; // 带序号和快照元数据的网络行情帧。
  using Event=Exchange::MEMarketUpdate; // 供下游应用的内部行情事件。
  using Type=Exchange::MarketUpdateType; // 行情事件类型别名。
  struct Batch {std::vector<Event> events;uint64_t watermark,hash;}; // 候选恢复事件、最终增量水位及状态哈希。
  explicit MarketRecovery(size_t limit=Common::ME_MAX_MARKET_UPDATES):limit_(limit) {ASSERT(limit>=Common::ME_MAX_TICKERS+2,"recovery cache too small");} // 设置缓存上限，至少容纳各品种 CLEAR 和首尾标记。
  void restart() {recovering_=true;snapshot_.clear();incremental_.clear();invalid_cycle_=true;} // 丢弃缓存进入恢复，等待更新的快照周期。
  bool recovering() const noexcept {return recovering_;} // 查询是否正在恢复。
  uint64_t expected() const noexcept {return expected_;} // 查询实时路径的下一条期望序号。
  size_t cached() const noexcept {return snapshot_.size()+incremental_.size();} // 统计两类缓存合计帧数。
  bool liveCandidate(const Update& update) { // 检查实时增量序号，暂不推进状态。
    if(recovering_)return false; // 恢复中不能直接接受实时增量。
    if(update.seq_num_==expected_)return true; // 下一条期望序号可尝试向下游投递。
    if(update.seq_num_<expected_) {++duplicates_;return false;} // 旧序号计为重复并忽略。
    ++gaps_;restart();return false; // 新序号跳跃意味着缺口，重新恢复。
  } // 结束当前作用域。

  void commitLive() {++expected_;} // 只有下游投递成功后才推进期望序号。
  bool ingest(bool snapshot,const Update& update) { // 把快照或增量纳入恢复缓存。
    if(!recovering_)return false; // 实时状态不使用恢复缓存。
    if(snapshot) { // 处理快照分支。
      if(!update.snapshot_cycle_ || update.snapshot_cycle_<cycle_)return false; // 拒绝零周期和过期周期。
      if(update.snapshot_cycle_>cycle_) { // 发现新的快照周期。
        cycle_=update.snapshot_cycle_;snapshot_.clear();invalid_cycle_=false; // 切换周期，清除上一周期快照。
        for(auto it=incremental_.begin();it!=incremental_.end() && it->first<=update.watermark_;)it=incremental_.erase(it); // 删除已由新快照水位覆盖的旧增量。
      } // 结束当前作用域。

      if(invalid_cycle_)return false; // 已判无效的周期不继续拼接。
      auto found=snapshot_.find(update.seq_num_); // 查找相同快照帧序号。
      if(found!=snapshot_.end()) { // 处理重复帧。
        if(found->second.me_market_update_.toString()!=update.me_market_update_.toString() || found->second.watermark_!=update.watermark_ || found->second.state_hash_!=update.state_hash_) {invalid_cycle_=true;snapshot_.clear();++invalid_;} // 同序号内容或元数据冲突，使整轮快照无效。
        else ++duplicates_; // 内容相同只增加重复计数。
        return false; // 重复帧不再次缓存。
      } // 结束当前作用域。

      snapshot_.emplace(update.seq_num_,update); // 按快照序号保存帧，允许到达顺序不同。
    } else { // 进入另一分支。
      auto [found,inserted]=incremental_.emplace(update.seq_num_,update); // 按增量序号插入，获得是否首次出现的结果。
      if(!inserted) { // 处理重复增量。
        ++duplicates_; // 增加重复计数。
        if(found->second.me_market_update_.toString()!=update.me_market_update_.toString()) {restart();++invalid_;return false;} // 同序号内容冲突，丢弃两类缓存重新恢复。
      } // 结束当前作用域。
    } // 结束当前作用域。

    if(cached()>limit_) {snapshot_.clear();incremental_.clear();invalid_cycle_=true;++overflows_;return false;} // 超过总缓存上限则清空，避免无界增长。
    return true; // 输入已纳入缓存。
  } // 结束当前作用域。

  std::optional<Batch> build() { // 尝试生成可提交的完整恢复批次。
    if(snapshot_.empty() || invalid_cycle_)return std::nullopt; // 空快照或无效周期不能恢复。
    const auto& first=snapshot_.begin()->second;const auto& last=snapshot_.rbegin()->second; // 取得按序号排列的首尾帧。
    if(first.seq_num_!=0 || first.me_market_update_.type_!=Type::SNAPSHOT_START || last.me_market_update_.type_!=Type::SNAPSHOT_END)return std::nullopt; // 必须从序号 0 的 START 到 END，不能接受残缺快照。
    const auto watermark=first.watermark_; // 取快照覆盖的最后增量序号。
    if(last.watermark_!=watermark || first.me_market_update_.order_id_!=watermark || last.me_market_update_.order_id_!=watermark || first.state_hash_!=last.state_hash_) {invalid_cycle_=true;++invalid_;return std::nullopt;} // 首尾水位及声明哈希必须一致。
    std::array<bool,Common::ME_MAX_TICKERS> cleared{}; // 记录每个品种是否收到 CLEAR。
    State state;Batch batch{{},watermark,0};uint64_t sequence=0; // 建立临时订单状态、候选批次和连续性计数。
    for(const auto& [id,wrapped]:snapshot_) { // 按序号验证快照全部帧。
      if(id!=sequence++ || wrapped.watermark_!=watermark || wrapped.snapshot_cycle_!=cycle_ || wrapped.state_hash_!=first.state_hash_)return std::nullopt; // 要求帧连续且周期、水位、哈希声明一致。
      const auto& event=wrapped.me_market_update_; // 取内部行情事件。
      if(id==0)continue; // 首帧是元数据，不作为订单簿事件重放。
      if(event.type_==Type::SNAPSHOT_END) {if(id!=snapshot_.rbegin()->first)return std::nullopt;continue;} // END 只能出现在快照尾部。
      if(event.ticker_id_>=Common::ME_MAX_TICKERS)return std::nullopt; // 品种 ID 必须有效。
      if(event.type_==Type::CLEAR) { // 验证品种清空标记。
        if(cleared[event.ticker_id_])return std::nullopt; // 同品种不能出现重复 CLEAR。
        cleared[event.ticker_id_]=true; // 记录该品种已清空。
      } else if(event.type_!=Type::ADD || !cleared[event.ticker_id_])return std::nullopt; // 其余帧只能是对应 CLEAR 之后的 ADD。
      if(!apply(state,event))return std::nullopt; // 在临时状态中应用事件并验证业务字段。
      batch.events.push_back(event); // 保留需要交给下游重放的事件。
    } // 结束当前作用域。

    if(!std::all_of(cleared.begin(),cleared.end(),[](bool value){return value;}))return std::nullopt; // 所有品种都必须有 CLEAR，防止保留旧品种挂单。
    if(Common::bookHash(flatten(state))!=first.state_hash_) {++hash_failures_;invalid_cycle_=true;return std::nullopt;} // 实际重建状态的哈希必须匹配快照声明。
    uint64_t next=watermark+1; // 从快照水位的下一条增量开始续接。
    for(const auto& [id,wrapped]:incremental_) { // 按序号重放缓存的增量。
      if(id<next)continue; // 已被快照覆盖的旧增量跳过。
      if(id!=next)return std::nullopt; // 存在序号缺口，暂不能恢复。
      if(!apply(state,wrapped.me_market_update_))return std::nullopt; // 增量事件也必须通过订单簿语义校验。
      batch.events.push_back(wrapped.me_market_update_);++next; // 加入重放批次并推进连续序号。
    } // 结束当前作用域。

    batch.watermark=next-1;batch.hash=Common::bookHash(flatten(state));return batch; // 保存最终水位与哈希，返回候选而非直接恢复信任。
  } // 结束当前作用域。

  void commit(const Batch& batch) {expected_=batch.watermark+1;recovering_=false;snapshot_.clear();incremental_.clear();++recoveries_;} // 整批入下游队列后才提交期望序号并清空缓存。
  uint64_t gaps_=0,duplicates_=0,overflows_=0,invalid_=0,hash_failures_=0,recoveries_=0; // 恢复诊断计数，由所属线程更新。
 private: // 内部状态与辅助接口。
  using State=std::array<std::map<Common::OrderId,Event>,Common::ME_MAX_TICKERS>; // 每个品种按订单 ID 管理临时存量。
  static std::vector<Event> flatten(const State& state) { // 将临时订单状态展开成哈希输入。
    std::vector<Event> events;for(const auto& book:state)for(const auto& [id,event]:book){(void)id;events.push_back(event);}return events; // 收集各品种当前仍在簿上的订单。
  } // 结束当前作用域。

  static bool apply(State& state,const Event& event) { // 在临时订单簿中验证并应用单个事件。
    if(event.ticker_id_>=Common::ME_MAX_TICKERS)return false; // 先验证品种，避免数组越界。
    auto &orders=state[event.ticker_id_]; // 取对应品种的订单集合。
    if(event.type_==Type::CLEAR){orders.clear();return true;} // CLEAR 清空该品种。
    if(event.type_==Type::TRADE)return event.qty_>0 && event.qty_!=Common::Qty_INVALID && event.price_>0 && event.price_!=Common::Price_INVALID && (event.side_==Common::Side::BUY || event.side_==Common::Side::SELL); // TRADE 只验证成交字段，不扣挂单量，也不要求其订单 ID 有效。
    if(!event.order_id_ || event.order_id_>=Common::ME_MAX_ORDER_IDS)return false; // 其他订单簿事件要求有效订单 ID。
    auto found=orders.find(event.order_id_); // 定位已有挂单。
    if(event.type_==Type::ADD) { // 处理新增订单。
      if(found!=orders.end() || event.price_<=0 || event.price_==Common::Price_INVALID || !event.qty_ || event.qty_==Common::Qty_INVALID || !event.priority_ || event.priority_==Common::Priority_INVALID || (event.side_!=Common::Side::BUY && event.side_!=Common::Side::SELL))return false; // ADD 必须不存在，并具有合法价格、量、优先级和方向。
      orders.emplace(event.order_id_,event);return true; // 加入存量订单并确认成功。
    } // 结束当前作用域。

    if(found==orders.end() || found->second.side_!=event.side_ || found->second.price_!=event.price_)return false; // 修改/删除必须找到挂单，方向和价格不能改变。
    if(event.type_==Type::CANCEL){orders.erase(found);return true;} // CANCEL 直接删除，不根据 qty 再扣一次。
    if(event.type_==Type::MODIFY && event.qty_>0 && event.qty_!=Common::Qty_INVALID) {found->second.qty_=event.qty_;return true;} // MODIFY 用绝对剩余量覆盖，要求数量为正。
    return false; // 其余未支持类型或非法参数拒绝。
  } // 结束当前作用域。

  size_t limit_;bool recovering_=true,invalid_cycle_=false; // 缓存上限、恢复状态与快照周期有效性。
  uint64_t cycle_=0,expected_=1; // 当前快照周期与下一条期望增量序号。
  std::map<uint64_t,Update> snapshot_,incremental_; // 分别按序号保存快照和增量。
}; // 结束当前作用域。
} // 结束当前作用域。
