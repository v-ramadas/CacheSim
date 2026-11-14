#ifndef __SNIPER_DRAM_H__
#define __SNIPER_DRAM_H__

// Port of Sniper's DRAM timing (snipersim, common/performance_model):
//   - DramPerfModelConstant::getAccessLatency  (dram_perf_model_constant.cc)
//   - QueueModel::create and the four queue models: history_list, windowed_mg1, basic, contention
//     (queue_model_*.cc, contention_model.cc), with MovingAverage (misc/moving_average.h)
//   - ComponentBandwidth::getRoundedLatency (misc/subsecond_time.h)
//   - DRAM controller count and address interleaving (memory_manager_base.cc, address_home_lookup.cc)
// Times are femtoseconds in a uint64_t, as Sniper's SubsecondTime, including its wrap-around arithmetic.
// tests/sniper_queue/ compiles Sniper's original sources next to these and checks identical results.

#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sniper {

using fs_t = uint64_t;                         // SubsecondTime: femtoseconds
constexpr fs_t FS = 1, PS = 1000, NS = 1000000, US = 1000000000ULL;
constexpr fs_t MAX_TIME = 0xffffffffffffffffULL;

// Parameters (Sniper config keys in comments). Defaults: Sniper config/base.cfg.
struct Config {
    double latency_ns = 100;                   // perf_model/dram/latency
    double per_controller_bandwidth = 5;       // perf_model/dram/per_controller_bandwidth (GB/s)
    int num_controllers = -1;                  // perf_model/dram/num_controllers (-1: by interleaving)
    int controllers_interleaving = 0;          // perf_model/dram/controllers_interleaving (0: one per core)
    int home_lookup_param = 6;                 // perf_model/dram_directory/home_lookup_param (log2 bytes)
    bool queue_enabled = true;                 // perf_model/dram/queue_model/enabled
    std::string queue_type = "history_list";   // perf_model/dram/queue_model/type
    uint32_t history_max_list_size = 100;      // queue_model/history_list/max_list_size
    bool history_analytical = true;            // queue_model/history_list/analytical_model_enabled
    uint64_t mg1_window_ns = 1000;             // queue_model/windowed_mg1/window_size
    bool basic_ma_enabled = true;              // queue_model/basic/moving_avg_enabled
    uint32_t basic_ma_window = 1024;           // queue_model/basic/moving_avg_window_size
    std::string basic_ma_type = "arithmetic_mean";   // queue_model/basic/moving_avg_type
};

// Sniper's ClockSkewMinimizationServer::getGlobalTime(), used by windowed_mg1: the earliest time any thread may
// still be at. Here: the slowest running core's clock (set by the scheduler).
fs_t global_time();
void set_global_time(fs_t t);

// ---------------------------------------------------------------------------------------- moving averages
class MovingAverage {
  public:
    explicit MovingAverage(uint32_t max_window_size) : max_(max_window_size) {}
    virtual ~MovingAverage() = default;
    virtual void update(fs_t next) = 0;
    virtual fs_t compute() const = 0;
    fs_t compute(fs_t next) { update(next); return compute(); }
    static std::unique_ptr<MovingAverage> create(const std::string& type, uint32_t max_window_size);
  protected:
    void add_to_window(fs_t next);             // keeps the last max_ values, in insertion order
    uint32_t max_;
    std::deque<fs_t> window_;
};

class MovingArithmeticMean : public MovingAverage {
  public:
    using MovingAverage::MovingAverage;
    using MovingAverage::compute;
    void update(fs_t next) override;
    fs_t compute() const override;
  private:
    fs_t sum_ = 0;
};

class MovingGeometricMean : public MovingAverage {
  public:
    using MovingAverage::MovingAverage;
    using MovingAverage::compute;
    void update(fs_t next) override { add_to_window(next); }
    fs_t compute() const override;
};

class MovingMedian : public MovingAverage {
  public:
    using MovingAverage::MovingAverage;
    using MovingAverage::compute;
    void update(fs_t next) override { add_to_window(next); }
    fs_t compute() const override;             // middle element in insertion order, as Sniper
};

// ---------------------------------------------------------------------------------------- queue models
class QueueModel {
  public:
    virtual ~QueueModel() = default;
    virtual fs_t computeQueueDelay(fs_t pkt_time, fs_t processing_time) = 0;
    static std::unique_ptr<QueueModel> create(const std::string& type, fs_t min_processing_time, const Config& cfg);
    uint64_t requests = 0, requests_analytical = 0;
    fs_t total_queue_delay = 0, utilized_time = 0;
};

class QueueModelHistoryList : public QueueModel {
  public:
    QueueModelHistoryList(fs_t min_processing_time, uint32_t max_list_size, bool analytical);
    fs_t computeQueueDelay(fs_t pkt_time, fs_t processing_time) override;
  private:
    fs_t computeUsingHistoryList(fs_t pkt_time, fs_t processing_time);
    fs_t min_processing_time_;
    uint32_t max_list_size_;
    bool analytical_;
    std::list<std::pair<fs_t, fs_t>> free_intervals_;
    std::unique_ptr<MovingAverage> average_delay_;
};

class QueueModelWindowedMG1 : public QueueModel {
  public:
    explicit QueueModelWindowedMG1(uint64_t window_ns) : window_size_(window_ns * NS) {}
    fs_t computeQueueDelay(fs_t pkt_time, fs_t processing_time) override;
  private:
    void addItem(fs_t pkt_time, fs_t service_time);
    void removeItems(fs_t earliest_time);
    const fs_t window_size_;
    std::multimap<fs_t, fs_t> window_;
    uint64_t num_arrivals_ = 0, service_time_sum_ = 0, service_time_sum2_ = 0;   // ps, ps^2
};

class QueueModelBasic : public QueueModel {
  public:
    QueueModelBasic(bool ma_enabled, uint32_t ma_window, const std::string& ma_type);
    fs_t computeQueueDelay(fs_t pkt_time, fs_t processing_time) override;
  private:
    fs_t queue_time_ = 0;
    std::unique_ptr<MovingAverage> moving_average_;
};

// ContentionModel (num_outstanding slots), as used by the "contention" queue model with one slot.
class ContentionModel {
  public:
    explicit ContentionModel(uint32_t num_outstanding = 1)
        : num_outstanding_(num_outstanding), time_(num_outstanding, {0, 0}) {}
    fs_t getCompletionTime(fs_t t_start, fs_t t_delay, uint64_t tag = 0);
    uint64_t n_requests = 0, n_outoforder = 0, n_simultaneous = 0;
    fs_t total_delay = 0;
  private:
    uint32_t num_outstanding_;
    std::vector<std::pair<fs_t, uint64_t>> time_;
    fs_t t_last_ = 0;
};

class QueueModelContention : public QueueModel {
  public:
    QueueModelContention() : contention_(1) {}
    fs_t computeQueueDelay(fs_t pkt_time, fs_t processing_time) override;
  private:
    ContentionModel contention_;
};

// ---------------------------------------------------------------------------------------- DRAM
// ComponentBandwidth(bits per ns) and its getRoundedLatency, with Sniper's float -> uint64 truncation.
class ComponentBandwidth {
  public:
    explicit ComponentBandwidth(float bw_in_bits_per_ns);
    fs_t getRoundedLatency(uint64_t bits_transmitted) const;
    uint64_t bits_per_us() const { return bw_in_bits_per_us_; }
  private:
    uint64_t bw_in_bits_per_us_;
};

// One DRAM controller: DramPerfModelConstant with its own queue model.
class DramController {
  public:
    DramController(const Config& cfg, uint32_t cache_block_size);
    // DramPerfModelConstant::getAccessLatency: queue delay + transfer time + fixed access cost.
    fs_t getAccessLatency(fs_t pkt_time, uint64_t pkt_size);
    // The queue delay and transfer time alone (--detailed-dram: our bank model replaces the fixed cost).
    fs_t getQueueAndTransfer(fs_t pkt_time, uint64_t pkt_size);
    uint64_t accesses = 0;
    fs_t total_access_latency = 0, total_queueing_delay = 0;
    fs_t last_queue_delay = 0;                 // queue delay of the latest request
    std::unique_ptr<QueueModel> queue;
  private:
    ComponentBandwidth bandwidth_;
    fs_t access_cost_;
};

// All controllers: count and address interleaving as Sniper.
class Dram {
  public:
    void init(const Config& cfg, uint32_t num_cores, uint32_t cache_block_size);
    bool ready() const { return !controllers_.empty(); }
    uint32_t home(uint64_t address) const;
    DramController& controller(uint64_t address) { return *controllers_[home(address)]; }
    size_t num_controllers() const { return controllers_.size(); }
    const Config& config() const { return cfg_; }
    uint64_t reads = 0, writes = 0;
    fs_t read_latency_sum = 0, read_queue_sum = 0, write_queue_sum = 0;
    void print_stats() const;
  private:
    Config cfg_;
    uint32_t home_shift_ = 6;
    std::vector<std::unique_ptr<DramController>> controllers_;
};

}  // namespace sniper

#endif
