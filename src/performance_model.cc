#include "performance_model.h"
#include <fstream>

// Performance Model Initialization
PerformanceModel::PerformanceModel(const std::string& filename) {
    configs.resize(static_cast<size_t>(Params::numParameters), static_cast<ParamType>(0));
    populateModel(filename);
}

void PerformanceModel::populateModel(const std::string& filename) {
    //configs.resize(static_cast<size_t>(Params::numParameters)-1, 0);

    auto parsedData = PerformanceModel::parseConfigFile(filename);
    for (const auto& [param, value]: parsedData) {
        if (stringToEnumMap.find(param) == stringToEnumMap.end()) {
            throw std::runtime_error(fmt::format(
                "Fatal: Unrecognized parameter {} in {}.",
                    param, filename
            ));
        } else {
            configs[static_cast<ParamType>(stringToEnumMap.at(param))] = value;
        }
    }

    memory.init();
}

// Helper Functions

std::string PerformanceModel::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}
    
std::unordered_map<std::string, ParamType> PerformanceModel::parseConfigFile(const std::string& filename) {
    std::unordered_map<std::string, ParamType> configData;
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return configData;
    }
    std::string line;
    while (std::getline(file, line)) {
        line = PerformanceModel::trim(line);
        // 1. Skip empty lines or comment lines starting with // or #
        if (line.empty() || line.rfind("//", 0) == 0 || line.rfind("#", 0) == 0) {
            continue;
        }
        // 2. Locate the colon delimiter
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue; // Skip lines that don't match the "key: value" format
        }
        // 3. Extract and clean the key and value strings
        std::string key = PerformanceModel::trim(line.substr(0, colonPos));
        std::string valStr = PerformanceModel::trim(line.substr(colonPos + 1));
        // 4. Convert the value string to a float and store it
        try {
            float value = std::stof(valStr);
            configData[key] = value;
        } catch (const std::invalid_argument& e) {
            std::cerr << "Warning: Could not parse value for key '" << key << "': " << valStr << std::endl;
        }
    }

    file.close();
    return configData;
}


// Performance Model Behavior

void PerformanceModel::updateCycles(const std::string& action, uint64_t numInstructions) {
    ParamType latency = getParam(action);
    numCycles += numInstructions*latency;
}

ParamType PerformanceModel::getParam(const std::string& param) {
    if (stringToEnumMap.find(param) == stringToEnumMap.end()) {
        fmt::print("[FATAL] {} not found\n", param);
        std::exit(1);
    }
    return configs[static_cast<ParamType>(stringToEnumMap.at(param))];
}

void PerformanceModel::printParams() {
    for (const auto& [param, value]: stringToEnumMap) {
        fmt::print("{}: {}\n",
                param, configs[static_cast<ParamType>(value)]);
    }
}

void PerformanceModel::processCPU(const uint64_t /*numInstructions*/) {
    //uint64_t progressCycles = 0;

    //uint64_t num_decode_windows = (numInstructions + getParam("cpu_decode_width"))/getParam("cpu_decode_width");
    //progressCycles += getParam("cpu_decode_latency")*num_decode_windows;

    //uint64_t num_dispatch_windows = (numInstructions + getParam("cpu_dispatch_width"))/getParam("cpu_dispatch_width");
    //progressCycles += getParam("cpu_dispatch_latency")*num_dispatch_windows;

    //uint64_t num_scheduling_windows = (numInstructions + getParam("cpu_scheduler_size"))/getParam("cpu_scheduler_size");
    //progressCycles += getParam("cpu_schedule_latency")*num_scheduling_windows;

    //uint64_t num_execution_windows = (numInstructions + getParam("cpu_lq_width"))/getParam("cpu_lq_width");
    //progressCycles += getParam("cpu_execute_latency")*num_execution_windows;

    //numCycles += progressCycles;

    // We assume an IPC of 1. So update time by 1
    numCycles++;
}


void PerformanceModel::processL1D(uint64_t /*address*/, bool is_hit, bool is_fill) {
    if (is_fill) {
        numCycles += (getParam("l1d_latency")+1)/2;
    } else if (is_hit) {
        numCycles += ((getParam("l1d_latency")-1)/2);
    } else {
        numCycles += ((getParam("l1d_latency")-1)/2);
    }
}

void PerformanceModel::processLLC(uint64_t /*address*/, bool is_hit, bool is_fill) {
    if (is_fill) {
        numCycles += (getParam("llc_latency")+1)/2;
    } else if (is_hit) {
        numCycles += ((getParam("llc_latency")-1)/2);
    } else {
        numCycles += ((getParam("llc_latency")-1)/2);
    }
}

void PerformanceModel::processMemory(const uint64_t address) {
    uint64_t processCycles = 0;
    uint64_t memoryTicks = (numCycles*1000000.0d/getParam("cpu_frequency"));
    bool shouldRefresh = memoryTicks >= memory.lastRefresh + memory.tREF;
    if (performance::DETAILED_DRAM) {
        //TODO: Implement bus turnaround to switch between read and write
        // Refresh
        if (shouldRefresh) {
            memory.lastRefresh = memoryTicks;
            processCycles += memory.tRFC;
            std::fill(memory.openRowList.begin(), memory.openRowList.end(), UINT64_MAX);
        }
    
        // Bank Access
        uint64_t rank = memory.get_value(address, memory.rank_idx_max, memory.rank_idx_min);;
        uint64_t bankgroup = memory.get_value(address, memory.bankgroup_idx_max, memory.bankgroup_idx_min); 
        bankgroup = rank*getParam("memory_bankgroups"s) + bankgroup;
        uint64_t bank = memory.get_value(address, memory.bank_idx_max, memory.bank_idx_min);
        bank = bankgroup*getParam("memory_banks"s) + bank;
        //TODO: Bus congestion
        if (memory.bankgroupReadyStall[bankgroup] > memoryTicks) {
            processCycles += (memory.bankgroupReadyStall[bankgroup]-memoryTicks) + memory.busReturnTime;
        } else {
            processCycles += memory.busReturnTime;
        }
        memory.bankgroupReadyStall[bankgroup] = memoryTicks + memory.busReturnTime + memory.busBankgroupStall;
    
        // Service Request
        uint64_t row = memory.get_value(address, memory.row_idx_max, memory.row_idx_min);
        if (memory.openRowList[bank] == row) {
            processCycles++;
        } else if (memory.openRowList[bank] == UINT64_MAX) {
            // No open row
            processCycles += memory.tRCD;
            memory.openRowList[bank] = row;
        } else {
            processCycles += memory.tRP + memory.tRCD;
            memory.openRowList[bank] = row;
        }
        processCycles += memory.tCAS;
    } else {
        processCycles += memory.tCAS + memory.tRCD + memory.busReturnTime;
        if (shouldRefresh) {
            memory.lastRefresh = memoryTicks;
            processCycles += memory.tRFC;
        }
    }

    numCycles += uint64_t((processCycles/1000000.0d)*getParam("cpu_frequency"));
}
