#include "performance_model.h"
#include <fstream>

PerformanceModel::PerformanceModel(const std::string& filename) {
    configs.resize(static_cast<size_t>(Params::numParameters)-1, static_cast<ParamType>(0));
    populateModel(filename);
}

void PerformanceModel::updateCycles(const std::string& action, uint64_t numInstructions) {
    ParamType latency = getParam(action);
    numCycles += numInstructions*latency;
}

ParamType PerformanceModel::getParam(const std::string& param) {
    return configs[static_cast<ParamType>(stringToEnumMap.at(param))];
}

void PerformanceModel::printParams() {
    for (const auto& [param, value]: stringToEnumMap) {
        fmt::print("Parameter: {} Value: {}\n",
                param, configs[static_cast<ParamType>(value)]);
    }
}

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

void PerformanceModel::populateModel(const std::string& filename) {
    configs.resize(static_cast<size_t>(Params::numParameters)-1, 0);

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
}
