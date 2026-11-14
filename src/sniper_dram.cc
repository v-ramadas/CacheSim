#include "sniper_dram.h"

#include <fmt/core.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace sniper {

static fs_t g_global_time = 0;
fs_t global_time() { return g_global_time; }
void set_global_time(fs_t t) { g_global_time = t; }

// ---------------------------------------------------------------------------------------- moving averages
// misc/moving_average.h: a ring of max_window_size + 1 slots holding the last max_window_size values.

void MovingAverage::add_to_window(fs_t next) {
    window_.push_back(next);
    if (window_.size() > max_)
        window_.pop_front();
}

void MovingArithmeticMean::update(fs_t next) {
    if (window_.size() == max_)
        sum_ -= window_.front();
    sum_ += next;
    add_to_window(next);
}

fs_t MovingArithmeticMean::compute() const {
    return sum_ / static_cast<uint32_t>(window_.size());   // SubsecondTime / UInt32 (integer)
}

fs_t MovingGeometricMean::compute() const {
    fs_t product = 1;                                      // setInternalDataForced(1); *= wraps like Sniper
    for (fs_t v : window_)
        product *= v;
    double r = std::pow((double)product, (double)1.0 / static_cast<uint32_t>(window_.size()));
    return static_cast<fs_t>(r);
}

fs_t MovingMedian::compute() const {
    return window_[window_.size() / 2];
}

std::unique_ptr<MovingAverage> MovingAverage::create(const std::string& type, uint32_t max_window_size) {
    if (type == "arithmetic_mean") return std::make_unique<MovingArithmeticMean>(max_window_size);
    if (type == "geometric_mean") return std::make_unique<MovingGeometricMean>(max_window_size);
    if (type == "median") return std::make_unique<MovingMedian>(max_window_size);
    throw std::runtime_error("Unsupported Average Type: " + type);
}

// ---------------------------------------------------------------------------------------- queue models

std::unique_ptr<QueueModel> QueueModel::create(const std::string& type, fs_t min_processing_time, const Config& cfg) {
    if (type == "basic")
        return std::make_unique<QueueModelBasic>(cfg.basic_ma_enabled, cfg.basic_ma_window, cfg.basic_ma_type);
    if (type == "history_list")
        return std::make_unique<QueueModelHistoryList>(min_processing_time, cfg.history_max_list_size,
                                                       cfg.history_analytical);
    if (type == "contention")
        return std::make_unique<QueueModelContention>();
    if (type == "windowed_mg1")
        return std::make_unique<QueueModelWindowedMG1>(cfg.mg1_window_ns);
    throw std::runtime_error("Unrecognized Queue Model Type(" + type + ")");
}

// queue_model_history_list.cc
QueueModelHistoryList::QueueModelHistoryList(fs_t min_processing_time, uint32_t max_list_size, bool analytical)
    : min_processing_time_(min_processing_time), max_list_size_(max_list_size), analytical_(analytical),
      average_delay_(MovingAverage::create("arithmetic_mean", max_list_size)) {
    fs_t max_simulation_time = FS << 63;
    free_intervals_.push_back({0, max_simulation_time});
}

fs_t QueueModelHistoryList::computeQueueDelay(fs_t pkt_time, fs_t processing_time) {
    assert(free_intervals_.size() >= 1);
    fs_t queue_delay;
    std::pair<fs_t, fs_t> oldest_interval = free_intervals_.front();
    if (analytical_ && (pkt_time + processing_time) <= oldest_interval.first) {
        requests_analytical++;
        queue_delay = average_delay_->compute();     // computeUsingAnalyticalModel
    } else {
        queue_delay = computeUsingHistoryList(pkt_time, processing_time);
        average_delay_->update(queue_delay);         // updateAverageDelay
    }
    utilized_time += processing_time;                // updateQueueUtilization
    requests++;
    total_queue_delay += queue_delay;
    return queue_delay;
}

fs_t QueueModelHistoryList::computeUsingHistoryList(fs_t pkt_time, fs_t processing_time) {
    assert(free_intervals_.size() <= max_list_size_);
    fs_t queue_delay = MAX_TIME;
    for (auto curr_it = free_intervals_.begin(); curr_it != free_intervals_.end(); curr_it++) {
        std::pair<fs_t, fs_t> interval = *curr_it;
        if ((pkt_time >= interval.first) && ((pkt_time + processing_time) <= interval.second)) {
            queue_delay = 0;
            curr_it = free_intervals_.erase(curr_it);
            if ((pkt_time - interval.first) >= min_processing_time_)
                free_intervals_.insert(curr_it, {interval.first, pkt_time});
            if ((interval.second - (pkt_time + processing_time)) >= min_processing_time_)
                free_intervals_.insert(curr_it, {pkt_time + processing_time, interval.second});
            break;
        } else if (pkt_time < interval.first) {
            queue_delay = interval.first - pkt_time;
            curr_it = free_intervals_.erase(curr_it);
            if ((interval.second - (interval.first + processing_time)) >= min_processing_time_)
                free_intervals_.insert(curr_it, {interval.first + processing_time, interval.second});
            break;
        }
    }
    if (queue_delay == MAX_TIME)
        throw std::runtime_error("history_list: free interval not found");
    if (free_intervals_.size() > max_list_size_)
        free_intervals_.erase(free_intervals_.begin());
    return queue_delay;
}

// queue_model_windowed_mg1.cc
fs_t QueueModelWindowedMG1::computeQueueDelay(fs_t pkt_time, fs_t processing_time) {
    fs_t t_queue = 0;
    // SubsecondTime subtraction wraps like Sniper's.
    removeItems(std::max(global_time() - window_size_, pkt_time - 10 * window_size_));
    if (num_arrivals_ > 1) {
        double utilization = (double)service_time_sum_ / (window_size_ / PS);
        double arrival_rate = (double)num_arrivals_ / (window_size_ / PS);
        double service_time_Es2 = service_time_sum2_ / num_arrivals_;   // UInt64 / UInt64
        if (utilization > .99)
            utilization = .99;
        t_queue = static_cast<uint64_t>(arrival_rate * service_time_Es2 / (2 * (1. - utilization))) * PS;
        if (t_queue > window_size_)
            t_queue = window_size_;
    }
    addItem(pkt_time, processing_time);
    requests++;
    utilized_time += processing_time;
    total_queue_delay += t_queue;
    return t_queue;
}

void QueueModelWindowedMG1::addItem(fs_t pkt_time, fs_t service_time) {
    window_.insert({pkt_time, service_time});
    num_arrivals_++;
    service_time_sum_ += service_time / PS;
    service_time_sum2_ += (service_time / PS) * (service_time / PS);
}

void QueueModelWindowedMG1::removeItems(fs_t earliest_time) {
    while (!window_.empty() && window_.begin()->first < earliest_time) {
        auto entry = window_.begin();
        num_arrivals_--;
        service_time_sum_ -= entry->second / PS;
        service_time_sum2_ -= (entry->second / PS) * (entry->second / PS);
        window_.erase(entry);
    }
}

// queue_model_basic.cc
QueueModelBasic::QueueModelBasic(bool ma_enabled, uint32_t ma_window, const std::string& ma_type) {
    if (ma_enabled)
        moving_average_ = MovingAverage::create(ma_type, ma_window);
}

fs_t QueueModelBasic::computeQueueDelay(fs_t pkt_time, fs_t processing_time) {
    fs_t ref_time = moving_average_ ? moving_average_->compute(pkt_time) : pkt_time;
    fs_t queue_delay = (queue_time_ > ref_time) ? (queue_time_ - ref_time) : 0;
    queue_time_ = std::max(queue_time_, ref_time) + processing_time;
    requests++;
    total_queue_delay += queue_delay;
    utilized_time += processing_time;
    return queue_delay;
}

// contention_model.cc: getCompletionTime(SubsecondTime ...)
fs_t ContentionModel::getCompletionTime(fs_t t_start, fs_t t_delay, uint64_t tag) {
    if (num_outstanding_ == 0)
        return t_start + t_delay;
    fs_t t_end;
    if (t_start == 0) {
        t_end = t_delay;
    } else if (t_start < t_last_) {
        t_end = t_start + t_delay;          // out of order: no congestion, only transfer latency
        ++n_outoforder;
    } else {
        if (t_start == t_last_)
            n_simultaneous++;
        uint64_t unit = 0;
        for (uint32_t i = 0; i < num_outstanding_; ++i) {
            if (time_[i].first <= t_start) {
                unit = i;
                break;
            } else if (time_[i].first < time_[unit].first) {
                unit = i;
            }
        }
        fs_t t_begin = (t_start < time_[unit].first) ? time_[unit].first : t_start;
        t_end = t_begin + t_delay;
        time_[unit].first = t_end;
        time_[unit].second = tag;
        total_delay += t_begin - t_start;
        t_last_ = t_start;
    }
    ++n_requests;
    return t_end;
}

// queue_model_contention.cc
fs_t QueueModelContention::computeQueueDelay(fs_t pkt_time, fs_t processing_time) {
    fs_t t_complete = contention_.getCompletionTime(pkt_time, processing_time);
    fs_t t_queue = t_complete - pkt_time - processing_time;
    requests++;
    total_queue_delay += t_queue;
    utilized_time += processing_time;
    return t_queue;
}

// ---------------------------------------------------------------------------------------- DRAM

ComponentBandwidth::ComponentBandwidth(float bw_in_bits_per_ns)
    : bw_in_bits_per_us_(static_cast<uint64_t>(bw_in_bits_per_ns * 1000.0f)) {}   // * TimeConverter<float>::UStoNS(1)

fs_t ComponentBandwidth::getRoundedLatency(uint64_t bits_transmitted) const {
    fs_t us = US;
    return ((bits_transmitted * us) + (us / 2)) / bw_in_bits_per_us_;
}

DramController::DramController(const Config& cfg, uint32_t cache_block_size)
    : bandwidth_(static_cast<float>(8 * cfg.per_controller_bandwidth)),           // GB/s -> bits per ns
      access_cost_(static_cast<uint64_t>((static_cast<float>(cfg.latency_ns) * 1000.0f) * 1000.0f)) {   // NStoFS<float>
    if (cfg.queue_enabled)
        queue = QueueModel::create(cfg.queue_type, bandwidth_.getRoundedLatency(8 * cache_block_size), cfg);
}

fs_t DramController::getQueueAndTransfer(fs_t pkt_time, uint64_t pkt_size) {
    fs_t processing_time = bandwidth_.getRoundedLatency(8 * pkt_size);
    fs_t queue_delay = queue ? queue->computeQueueDelay(pkt_time, processing_time) : 0;
    total_queueing_delay += queue_delay;
    last_queue_delay = queue_delay;
    return queue_delay + processing_time;
}

fs_t DramController::getAccessLatency(fs_t pkt_time, uint64_t pkt_size) {
    fs_t queue_and_transfer = getQueueAndTransfer(pkt_time, pkt_size);
    fs_t access_latency = queue_and_transfer + access_cost_;
    accesses++;
    total_access_latency += access_latency;
    return access_latency;
}

void Dram::init(const Config& cfg, uint32_t num_cores, uint32_t cache_block_size) {
    cfg_ = cfg;
    // memory_manager_base.cc: num_controllers -1 places one controller every controllers_interleaving cores
    // (0 = on every core); otherwise num_controllers are placed by the network model.
    uint32_t n;
    if (cfg.num_controllers == -1)
        n = cfg.controllers_interleaving ? (num_cores + cfg.controllers_interleaving - 1) / cfg.controllers_interleaving
                                         : num_cores;
    else
        n = static_cast<uint32_t>(cfg.num_controllers);
    if (n == 0)
        n = 1;
    home_shift_ = static_cast<uint32_t>(cfg.home_lookup_param);
    controllers_.clear();
    for (uint32_t i = 0; i < n; ++i)
        controllers_.push_back(std::make_unique<DramController>(cfg, cache_block_size));
}

uint32_t Dram::home(uint64_t address) const {
    // address_home_lookup.cc: (address >> ahl_param) % total_modules
    return static_cast<uint32_t>((address >> home_shift_) % controllers_.size());
}

void Dram::print_stats() const {
    uint64_t req = 0, analytical = 0;
    for (const auto& c : controllers_)
        if (c->queue) {
            req += c->queue->requests;
            analytical += c->queue->requests_analytical;
        }
    auto ns = [](fs_t t, uint64_t n) { return n ? (double)t / n / NS : 0.0; };
    fmt::print("DRAM (Sniper constant model): controllers {} | latency {} ns | {} GB/s per controller | queue {} | "
               "reads {} writes {} | avg read latency {:.3f} ns | avg queue delay: reads {:.3f} ns writes {:.3f} ns",
               controllers_.size(), cfg_.latency_ns, cfg_.per_controller_bandwidth,
               cfg_.queue_enabled ? cfg_.queue_type : std::string("off"), reads, writes,
               ns(read_latency_sum, reads), ns(read_queue_sum, reads), ns(write_queue_sum, writes));
    if (cfg_.queue_enabled && cfg_.queue_type == "history_list")
        fmt::print(" | analytical-model requests {:.2f}%", req ? 100.0 * analytical / req : 0.0);
    fmt::print("\n");
}

}  // namespace sniper
