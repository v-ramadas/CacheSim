#ifndef __PERFORMANCE_MODEL_H__
#define __PERFORMANCE_MODEL_H__

#include <iostream>
#include <fmt/core.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>

using ParamType = uint64_t;

using namespace std::string_literals;

namespace performance {
    extern bool DETAILED_DRAM;
}

class PerformanceModel {
    inline static std::vector<ParamType> configs;
    inline static uint64_t numCycles;

    enum class Params {
        cpu_frequency,
        cpu_ifetch_buffer_size,
        cpu_decode_buffer_size,
        cpu_dispatch_buffer_size,
        cpu_register_file_size,
        cpu_rob_size,
        cpu_lq_size,
        cpu_sq_size,
        cpu_fetch_width,
        cpu_decode_width,
        cpu_dispatch_width,
        cpu_execute_width,
        cpu_lq_width,
        cpu_sq_width,
        cpu_retire_width,
        cpu_scheduler_size,
        cpu_dispatch_latency,
        cpu_decode_latency,
        cpu_schedule_latency,
        cpu_execute_latency,

        l1i_frequency,
        l1i_rq_size,
        l1i_wq_size,
        l1i_pq_size,
        l1i_mshr_size,
        l1i_latency,
        l1i_max_tag_check,
        l1i_max_fill,

        l1d_frequency,
        l1d_rq_size,
        l1d_wq_size,
        l1d_pq_size,
        l1d_mshr_size,
        l1d_latency,
        l1d_max_tag_check,
        l1d_max_fill,

        llc_frequency,
        llc_rq_size,
        llc_wq_size,
        llc_pq_size,
        llc_mshr_size,
        llc_latency,
        llc_max_tag_check,
        llc_max_fill,

        memory_frequency,
        memory_data_rate,
        memory_channels,
        memory_ranks,
        memory_bankgroups,
        memory_banks,
        memory_bank_rows,
        memory_bank_columns,
        memory_channel_width,
        memory_wq_size,
        memory_rq_size,
        memory_tCAS,
        memory_tRCD,
        memory_tRP,
        memory_tRAS,
        memory_refresh_period,
        memory_refreshes_per_period,

        // Do not remove this parameter. This tracks the number of parameters
        numParameters,
    };
    
    inline static const std::unordered_map<std::string, Params> stringToEnumMap = {
        {"cpu_frequency", Params::cpu_frequency},
        {"cpu_ifetch_buffer_size", Params::cpu_ifetch_buffer_size},
        {"cpu_decode_buffer_size", Params::cpu_decode_buffer_size},
        {"cpu_dispatch_buffer_size", Params::cpu_dispatch_buffer_size},
        {"cpu_register_file_size", Params::cpu_register_file_size},
        {"cpu_rob_size", Params::cpu_rob_size},
        {"cpu_lq_size", Params::cpu_lq_size},
        {"cpu_sq_size", Params::cpu_sq_size},
        {"cpu_fetch_width", Params::cpu_fetch_width},
        {"cpu_decode_width", Params::cpu_decode_width},
        {"cpu_dispatch_width", Params::cpu_dispatch_width},
        {"cpu_execute_width", Params::cpu_execute_width},
        {"cpu_lq_width", Params::cpu_lq_width},
        {"cpu_sq_width", Params::cpu_sq_width},
        {"cpu_retire_width", Params::cpu_retire_width},
        {"cpu_scheduler_size", Params::cpu_scheduler_size},
        {"cpu_dispatch_latency", Params::cpu_dispatch_latency},
        {"cpu_decode_latency", Params::cpu_decode_latency},
        {"cpu_schedule_latency", Params::cpu_schedule_latency},
        {"cpu_execute_latency", Params::cpu_execute_latency},

        {"l1i_frequency", Params::l1i_frequency},
        {"l1i_rq_size", Params::l1i_rq_size},
        {"l1i_wq_size", Params::l1i_wq_size},
        {"l1i_pq_size", Params::l1i_pq_size},
        {"l1i_mshr_size", Params::l1i_mshr_size},
        {"l1i_latency", Params::l1i_latency},
        {"l1i_max_tag_check", Params::l1i_max_tag_check},
        {"l1i_max_fill", Params::l1i_max_fill},

        {"l1d_frequency", Params::l1d_frequency},
        {"l1d_rq_size", Params::l1d_rq_size},
        {"l1d_wq_size", Params::l1d_wq_size},
        {"l1d_pq_size", Params::l1d_pq_size},
        {"l1d_mshr_size", Params::l1d_mshr_size},
        {"l1d_latency", Params::l1d_latency},
        {"l1d_max_tag_check", Params::l1d_max_tag_check},
        {"l1d_max_fill", Params::l1d_max_fill},

        {"llc_frequency", Params::llc_frequency},
        {"llc_rq_size", Params::llc_rq_size},
        {"llc_wq_size", Params::llc_wq_size},
        {"llc_pq_size", Params::llc_pq_size},
        {"llc_mshr_size", Params::llc_mshr_size},
        {"llc_latency", Params::llc_latency},
        {"llc_max_tag_check", Params::llc_max_tag_check},
        {"llc_max_fill", Params::llc_max_fill},

        {"memory_frequency", Params::memory_frequency},
        {"memory_data_rate", Params::memory_data_rate},
        {"memory_channels", Params::memory_channels},
        {"memory_ranks", Params::memory_ranks},
        {"memory_bankgroups", Params::memory_bankgroups},
        {"memory_banks", Params::memory_banks},
        {"memory_bank_rows", Params::memory_bank_rows},
        {"memory_bank_columns", Params::memory_bank_columns},
        {"memory_channel_width", Params::memory_channel_width},
        {"memory_wq_size", Params::memory_wq_size},
        {"memory_rq_size", Params::memory_rq_size},
        {"memory_tCAS", Params::memory_tCAS},
        {"memory_tRCD", Params::memory_tRCD},
        {"memory_tRP", Params::memory_tRP},
        {"memory_tRAS", Params::memory_tRAS},
        {"memory_refresh_period", Params::memory_refresh_period},
        {"memory_refreshes_per_period", Params::memory_refreshes_per_period},


        //No string to enum mapping for numParameters
    };
    
     static std::string trim(const std::string& str);
     static std::unordered_map<std::string, ParamType> parseConfigFile(const std::string& filename);
    public:
    PerformanceModel() {
        configs.resize(static_cast<size_t>(Params::numParameters), static_cast<ParamType>(0));
    }

    PerformanceModel(const std::string& filename);

    ~PerformanceModel() {
        configs.clear();
    }

    static void populateModel(const std::string& filename);

    static ParamType getParam(const std::string& param);

    static void printParams();

    static uint64_t getCycles() {return PerformanceModel::numCycles;}

    static void updateCycles(const std::string& action, uint64_t numInstructions = 1);

    static void processCPU(const uint64_t numInstructions = 1);

    static void processL1D(uint64_t address, bool is_hit, bool is_fill);

    static void processLLC(uint64_t address, bool is_hit, bool is_fill);

    static void processMemory(const uint64_t numInstructions = 1);

    private:
    struct Memory {
    
        std::vector<uint64_t> bankgroupReadyStall;
        std::vector<uint64_t> openRowList;
        uint64_t lastRefresh;
        uint64_t busReturnTime;
        uint64_t busBankgroupStall;
        uint32_t row_idx_min;
        uint32_t row_idx_max;
        uint32_t col_idx_min;
        uint32_t col_idx_max;
        uint32_t rank_idx_min;
        uint32_t rank_idx_max;
        uint32_t bank_idx_min;
        uint32_t bank_idx_max;
        uint32_t bankgroup_idx_min;
        uint32_t bankgroup_idx_max;
        uint32_t channel_idx_min;
        uint32_t channel_idx_max;
        uint32_t offset_idx_min;
        uint32_t offset_idx_max;
        uint64_t tRP;
        uint64_t tRCD;
        uint64_t tCAS;
        uint64_t tREF;
        uint64_t tRFC;

        Memory() {};
        void init() {
            lastRefresh = 0;
            busReturnTime = uint64_t(1000000/getParam("memory_data_rate"));
            busBankgroupStall = uint64_t(1000000/getParam("memory_data_rate"));
            openRowList.resize(PerformanceModel::getParam("memory_ranks"s)*PerformanceModel::getParam("memory_banks"s)*PerformanceModel::getParam("memory_bankgroups"s),
                    UINT64_MAX);
            bankgroupReadyStall.resize(PerformanceModel::getParam("memory_ranks"s)*PerformanceModel::getParam("memory_bankgroups"s), 0);
            row_idx_min = 6;
            row_idx_max = row_idx_min + std::max(int32_t(std::log2(PerformanceModel::getParam("memory_bank_rows"s)) - 1), int32_t(0));
            col_idx_min = row_idx_min+1;
            col_idx_max = col_idx_min + std::max((int32_t)(std::log2(PerformanceModel::getParam("memory_bank_columns"s)) - 1), int32_t(0));
            rank_idx_min = col_idx_min+1;
            rank_idx_max = rank_idx_min + std::max((int32_t)(std::log2(PerformanceModel::getParam("memory_ranks"s)) - 1), int32_t(0));
            bank_idx_min = rank_idx_max+1;
            bank_idx_max = bank_idx_min + std::max((int32_t)(std::log2(PerformanceModel::getParam("memory_banks"s)) - 1), int32_t(0));
            bankgroup_idx_min = bank_idx_min+1;
            bankgroup_idx_max = bankgroup_idx_min + std::max((int32_t)(std::log2(PerformanceModel::getParam("memory_bankgroups"s)) - 1), int32_t(0));
            channel_idx_min = bankgroup_idx_min+1;
            channel_idx_max = channel_idx_min + std::max((int32_t)(std::log2(PerformanceModel::getParam("memory_channels"s)) - 1), int32_t(0));
            offset_idx_min = channel_idx_min+1;
            offset_idx_max = offset_idx_min + std::max((int32_t)(std::log2(PerformanceModel::getParam("memory_channel_width"s)) - 1), int32_t(0));

            tRP = PerformanceModel::getParam("memory_tRP"s)*uint64_t(1000000/getParam("memory_frequency"));
            tRCD = PerformanceModel::getParam("memory_tRCD"s)*uint64_t(1000000/getParam("memory_frequency"));
            tCAS = PerformanceModel::getParam("memory_tCAS"s)*uint64_t(1000000/getParam("memory_frequency"));
            tREF = (PerformanceModel::getParam("memory_refresh_period")*1000000000)/PerformanceModel::getParam("memory_refreshes_per_period");
            tRFC = std::ceil(std::sqrt((8.0d*PerformanceModel::getParam("memory_bank_rows"s)*PerformanceModel::getParam("memory_bank_columns"s)*PerformanceModel::getParam("memory_banks"s)*PerformanceModel::getParam("memory_bankgroups"s))/1024.0/1024.0/1024.0))*PerformanceModel::getParam("memory_tRAS"s)*uint64_t(1000000/getParam("memory_frequency"));

        }

        uint64_t get_value(uint64_t address, uint64_t max, uint64_t min) {
            if (min == max) return 0UL;
            return (address & ((0x1 << (max - min))-1) << min) >> min;
        }
    };

    public:
    inline static Memory memory;
};

#endif
