#ifndef __PERFORMANCE_MODEL_H__
#define __PERFORMANCE_MODEL_H__

#include <iostream>
#include <fmt/core.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

using ParamType = uint64_t;
class PerformanceModel {
    inline static std::vector<ParamType> configs;
    inline static uint64_t numCycles;

    enum class Params {
        aluInstructionLatency,
        l1AccessLatency,
        l1ToL2AccessLatency,
        l2ToMemAccessLatency,
        memToL2ReturnLatency,
        l2ToL1ReturnLatency,
        l1ReturnLatency,

        // Do not remove this parameter. This tracks the number of parameters
        numParameters,
    };
    
    inline static const std::unordered_map<std::string, Params> stringToEnumMap = {
        {"aluInstructionLatency", Params::aluInstructionLatency},
        {"l1AccessLatency",       Params::l1AccessLatency},
        {"l1ToL2AccessLatency",  Params::l1ToL2AccessLatency},
        {"l2ToMemAccessLatency", Params::l2ToMemAccessLatency},
        {"memToL2ReturnLatency", Params::memToL2ReturnLatency},
        {"l2ToL1ReturnLatency",  Params::l2ToL1ReturnLatency},
        {"l1ReturnLatency",      Params::l1ReturnLatency},

        //No string to enum mapping for numParameters
    };
    
     static std::string trim(const std::string& str);
     static std::unordered_map<std::string, ParamType> parseConfigFile(const std::string& filename);
    public:
    PerformanceModel() {
        configs.resize(static_cast<size_t>(Params::numParameters)-1, static_cast<ParamType>(0));
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

};

#endif
