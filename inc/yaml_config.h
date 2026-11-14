#ifndef __YAML_CONFIG_H__
#define __YAML_CONFIG_H__

// YAML system configuration for CacheSim (--config <file>.yaml).
//
// A YAML file holds every simulator setting. Each key maps to a command-line
// option (see kKeys in src/yaml_config.cc), and the file's values feed CLI11's
// config-file mechanism. So:
//   - a value in the file acts as if its flag were passed;
//   - a flag on the command line overrides the file;
//   - an unknown key is an error, not silently ignored.
// Timing-model parameters (the old configs/*.cfg keys) live in the cpu, l1i,
// l1d, l2, llc and memory sections, e.g. llc.latency -> llc_latency. A legacy
// flat .cfg file (cpu_frequency: 4000, ...) is valid input too.
//
// Supported YAML subset: nested block mappings (2-space or any consistent
// indent, no tabs), plain/single/double-quoted scalars, flow lists [a, b] and
// block lists (- a), and comments (# ..., or legacy // lines). Not supported:
// anchors, multi-document files, flow mappings {...}, multi-line scalars.

#include <CLI/CLI.hpp>
#include <istream>
#include <map>
#include <string>
#include <vector>

namespace yaml_config {

struct Entry {
    std::string path;                  // dotted key path, e.g. llc.tag_bdi.bases
    std::vector<std::string> values;   // one value, or the items of a list
    bool is_list = false;
    int line = 0;
};

// Parse the supported YAML subset into leaf entries, in file order. Throws
// std::runtime_error with "<source>:<line>: <reason>" on malformed input.
std::vector<Entry> parse(std::istream& in, const std::string& source = "config");

// CLI11 config formatter: YAML file -> option values, and the effective option
// values -> YAML (--dump-config).
class YamlConfig : public CLI::Config {
  public:
    std::string to_config(const CLI::App* app, bool default_also, bool write_description,
                          std::string prefix) const override;
    std::vector<CLI::ConfigItem> from_config(std::istream& input) const override;
};

// Register one CLI option per timing-model parameter (--cpu_rob_size, --llc_latency,
// ...). Values given in the YAML file or on the command line are collected into
// timing_params(); pass that to PerformanceModel::populateModel.
void add_timing_options(CLI::App& app);

// Transform that records the option's value as typed, before an enum
// CheckedTransformer rewrites it to a number. Chain it AFTER that transformer
// (CLI11 runs transforms added later first) so --dump-config writes "lru", not "5".
std::function<std::string(std::string)> keep_text(const std::string& option);
const std::map<std::string, uint64_t>& timing_params();

}  // namespace yaml_config

#endif
