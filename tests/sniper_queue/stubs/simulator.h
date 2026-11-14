// Stub of Sniper's simulator.h for tests/sniper_queue: only what the queue-model sources call.
#pragma once
#include "fixed_types.h"
#include "subsecond_time.h"
#include <map>
#include <string>
#include <stdexcept>

struct StubCfg {
    std::map<std::string, std::string> v;
    bool getBool(const String& k) { return get(k) == "true"; }
    SInt64 getInt(const String& k) { return std::stoll(get(k)); }
    double getFloat(const String& k) { return std::stod(get(k)); }
    String getString(const String& k) { return String(get(k).c_str()); }
    std::string get(const String& k) {
        auto it = v.find(std::string(k.c_str()));
        if (it == v.end()) throw std::runtime_error("stub config: missing key " + std::string(k.c_str()));
        return it->second;
    }
};
struct StubSkew { SubsecondTime t; SubsecondTime getGlobalTime(bool = false) { return t; } };
struct StubDvfs { ComponentPeriod* p; const ComponentPeriod* getCoreDomain(int) { return p; } };
struct StubSim {
    StubCfg cfg; StubSkew skew; StubDvfs dvfs;
    StubCfg* getCfg() { return &cfg; }
    StubSkew* getClockSkewMinimizationServer() { return &skew; }
    StubDvfs* getDvfsManager() { return &dvfs; }
};
StubSim* Sim();
