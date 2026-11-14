#include "yaml_config.h"
#include "performance_model.h"

#include <fmt/core.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace yaml_config {

static std::map<std::string, std::vector<std::string>> g_text;   // option -> values as typed

// YAML key path -> command-line option (without the leading --), grouped by
// section. The order here is the order --dump-config writes. Timing-model
// parameters are not listed: <section>.<key> maps to --<section>_<key> when
// that is a PerformanceModel parameter (see resolve()).
static const std::vector<std::pair<std::string, std::string>> kKeys = {
    {"trace",                          "trace"},
    {"num_cores",                      "num-cores"},
    {"traces",                         "traces"},
    {"trace_format",                   "trace-format"},
    {"iters",                          "iters"},
    {"warmup_instructions",            "warmup-instructions"},
    {"gen_stats",                      "gen-stats"},
    {"enable_predictor",               "enable-predictor"},

    {"l1d.sets",                       "l1-sets"},
    {"l1d.ways",                       "l1-ways"},
    {"l1d.amoeba.enabled",             "l1-amoeba"},
    {"l1d.amoeba.fetch",               "l1-amoeba-fetch"},
    {"l1d.amoeba.keep_llc",            "l1-amoeba-keep-llc"},
    {"l1d.amoeba.record",              "l1-amoeba-record"},
    {"l1d.amoeba.oracle",              "l1-amoeba-oracle"},

    {"l2.enabled",                     "l2"},
    {"l2.sets",                        "l2-sets"},
    {"l2.ways",                        "l2-ways"},

    {"llc.sets",                       "num-cache-sets"},
    {"llc.ways",                       "num-cache-ways"},
    {"llc.block_size",                 "cache-block-size"},
    {"llc.replacement_policy",         "replacement-policy"},
    {"llc.insertion_policy",           "insertion-policy"},
    {"llc.no_iso_area",                "no-iso-area"},
    {"llc.tag_bdi.bases",              "tag-bdi-bases"},
    {"llc.tag_bdi.delta",              "tag-bdi-delta"},
    {"llc.tag_bdi.unconstrained",      "tag-bdi-unconstrained"},
    {"llc.amoeba.insert",              "amoeba-insert"},
    {"llc.amoeba.words",               "amoeba-words"},
    {"llc.amoeba.free_tags",           "amoeba-free-tags"},
    {"llc.popt.elem_size",             "popt-elem-size"},
    {"llc.popt.graph_dir",             "popt-graph-dir"},
    {"llc.popt.matrix_dir",            "popt-matrix-dir"},
    {"llc.ghost_cache.enabled",        "use-ghost-cache"},
    {"llc.ghost_cache.size",           "ghost-cache-size"},
    {"llc.ghost_cache.min_serviced",   "ghost-cache-min-serviced"},

    {"set_dueling.enabled",            "set-dueling"},
    {"set_dueling.mode",               "dueling-mode"},
    {"set_dueling.num_duels",          "num-duels"},
    {"set_dueling.period",             "dueling-period"},
    {"set_dueling.psel_max",           "psel-max"},
    {"set_dueling.psel_threshold",     "psel-threshold"},
    {"set_dueling.psel_hysteresis",    "psel-hysteresis"},
    {"set_dueling.conf_epoch",         "conf-epoch"},
    {"set_dueling.conf_max",           "conf-max"},
    {"set_dueling.conf_margin",        "conf-margin"},
    {"set_dueling.z_threshold",        "z-threshold"},
    {"set_dueling.window_size",        "window-size"},
    {"set_dueling.window_z_threshold", "window-z-threshold"},
    {"set_dueling.log_metrics",        "log-dueling-metrics"},

    {"memory.detailed_dram",           "detailed-dram"},

    {"debug.all",                      "debug"},
    {"debug.llc",                      "debug-llc"},
    {"debug.l1",                       "debug-l1"},
    {"debug.replacement_policy",       "debug-replacement-policy"},
    {"debug.tag_bdi",                  "debug-tag-bdi"},
    {"debug.ghost_cache",              "debug-ghost-cache"},
    {"debug.start",                    "debug-start"},
    {"debug.end",                      "debug-end"},
};

static const std::vector<std::string> kTimingSections = {"cpu", "l1i", "l1d", "l2", "llc", "memory"};

static bool is_timing_param(const std::string& name) {
    static const std::set<std::string> names = [] {
        auto v = PerformanceModel::paramNames();
        return std::set<std::string>(v.begin(), v.end());
    }();
    return names.count(name) > 0;
}

// Option name for a key path, or "" if the key is unknown.
static std::string resolve(const std::string& path) {
    for (const auto& [key, opt] : kKeys)
        if (key == path)
            return opt;
    auto dot = path.find('.');
    if (dot == std::string::npos)
        return is_timing_param(path) ? path : "";   // legacy flat .cfg key, e.g. llc_latency
    std::string section = path.substr(0, dot), rest = path.substr(dot + 1);
    if (rest.find('.') == std::string::npos &&
        std::find(kTimingSections.begin(), kTimingSections.end(), section) != kTimingSections.end() &&
        is_timing_param(section + "_" + rest))
        return section + "_" + rest;
    return "";
}

// ---------------------------------------------------------------------------- parser

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Drop a trailing "# comment" (a # at the start or after whitespace, outside quotes).
static std::string strip_comment(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"')
                ++i;
            else if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

static std::string unquote(const std::string& raw, const std::string& where) {
    std::string s = trim(raw);
    if (s.size() >= 1 && (s[0] == '"' || s[0] == '\'')) {
        char q = s[0];
        if (s.size() < 2 || s.back() != q)
            throw std::runtime_error(where + ": unterminated quoted string");
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            char c = s[i];
            if (q == '\'' && c == '\'' && s[i + 1] == '\'') { out += '\''; ++i; }
            else if (q == '"' && c == '\\') {
                char n = s[++i];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            } else out += c;
        }
        return out;
    }
    if (!s.empty() && (s[0] == '{' || s[0] == '&' || s[0] == '*' || s[0] == '|' || s[0] == '>'))
        throw std::runtime_error(where + ": unsupported YAML construct '" + s + "'");
    return s;
}

// Split "[a, 'b c', d]" into items.
static std::vector<std::string> flow_list(const std::string& s, const std::string& where) {
    if (s.back() != ']')
        throw std::runtime_error(where + ": unterminated list");
    std::vector<std::string> items;
    std::string body = s.substr(1, s.size() - 2), cur;
    char quote = 0;
    for (char c : body) {
        if (quote) {
            if (c == quote) quote = 0;
            cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            cur += c;
        } else if (c == ',') {
            items.push_back(unquote(cur, where));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty() || !items.empty())
        items.push_back(unquote(cur, where));
    return items;
}

std::vector<Entry> parse(std::istream& in, const std::string& source) {
    struct Open { int indent; std::string path; bool has_child; int line; };
    std::vector<Open> stack;            // mappings / lists opened by "key:" with no value
    std::vector<Entry> out;
    std::set<std::string> seen;
    std::string raw;
    int lineno = 0;

    auto close_until = [&](int indent) {
        while (!stack.empty() && indent <= stack.back().indent) {
            if (!stack.back().has_child)
                throw std::runtime_error(fmt::format("{}:{}: key '{}' has no value", source,
                                                     stack.back().line, stack.back().path));
            stack.pop_back();
        }
    };

    while (std::getline(in, raw)) {
        ++lineno;
        std::string where = fmt::format("{}:{}", source, lineno);
        std::string t = trim(raw);
        if (t.empty() || t[0] == '#' || t.rfind("//", 0) == 0)
            continue;
        if (t == "---")
            continue;
        size_t indent = raw.find_first_not_of(' ');
        if (raw[indent] == '\t')
            throw std::runtime_error(where + ": tabs are not allowed for indentation");
        std::string content = trim(strip_comment(raw));
        if (content.empty())
            continue;

        // Block list item: "- value" under an open key.
        if (content == "-" || content.rfind("- ", 0) == 0) {
            // A list may sit at the same indent as its key.
            while (!stack.empty() && (int)indent < stack.back().indent)
                close_until(stack.back().indent);
            if (stack.empty())
                throw std::runtime_error(where + ": list item without a key");
            Open& o = stack.back();
            if (!out.empty() && out.back().path == o.path && out.back().is_list) {
                // continuing the list
            } else if (!o.has_child) {
                out.push_back({o.path, {}, true, lineno});
                seen.insert(o.path);
            } else {
                throw std::runtime_error(where + ": list item inside a mapping");
            }
            o.has_child = true;
            std::string v = trim(content.substr(1));
            if (!v.empty() && v.back() == ':')
                throw std::runtime_error(where + ": lists of mappings are not supported");
            out.back().values.push_back(unquote(v, where));
            continue;
        }

        size_t colon = std::string::npos;
        char quote = 0;
        for (size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == ':' && (i + 1 == content.size() || content[i + 1] == ' ')) { colon = i; break; }
        }
        if (colon == std::string::npos)
            throw std::runtime_error(where + ": expected 'key: value'");
        std::string key = trim(content.substr(0, colon));
        std::string value = trim(content.substr(colon + 1));
        if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            throw std::runtime_error(where + ": invalid key '" + key + "'");

        close_until((int)indent);
        if (!stack.empty() && out.size() && out.back().is_list && out.back().path == stack.back().path)
            throw std::runtime_error(where + ": key inside a list");
        std::string path = stack.empty() ? key : stack.back().path + "." + key;
        if (!stack.empty())
            stack.back().has_child = true;
        if (seen.count(path))
            throw std::runtime_error(where + ": duplicate key '" + path + "'");
        seen.insert(path);

        if (value.empty()) {
            stack.push_back({(int)indent, path, false, lineno});
        } else if (value[0] == '[') {
            out.push_back({path, flow_list(value, where), true, lineno});
        } else {
            out.push_back({path, {unquote(value, where)}, false, lineno});
        }
    }
    close_until(-1);
    return out;
}

// ---------------------------------------------------------------------------- CLI11 glue

std::vector<CLI::ConfigItem> YamlConfig::from_config(std::istream& input) const {
    std::vector<Entry> entries;
    try {
        entries = parse(input, "--config");
    } catch (const std::runtime_error& e) {
        throw CLI::ConfigError(e.what());
    }
    std::vector<CLI::ConfigItem> items;
    for (const auto& e : entries) {
        std::string opt = resolve(e.path);
        if (opt.empty())
            throw CLI::ConfigError(fmt::format("--config:{}: unknown key '{}' (see kKeys in src/yaml_config.cc, "
                                               "or a timing parameter under cpu/l1i/l1d/l2/llc/memory)", e.line, e.path));
        CLI::ConfigItem item;
        item.name = opt;
        item.inputs = e.values;
        items.push_back(std::move(item));
    }
    return items;
}

static bool needs_quotes(const std::string& v) {
    if (v.empty())
        return true;
    if (v.front() == ' ' || v.back() == ' ')
        return true;
    for (const char* bad : {": ", " #", "[", "]", "{", "}", ","})
        if (v.find(bad) != std::string::npos)
            return true;
    return v[0] == '"' || v[0] == '\'' || (v[0] == '-' && v.size() > 1 && v[1] == ' ');
}

static std::string yaml_scalar(const std::string& v) {
    if (!needs_quotes(v))
        return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string YamlConfig::to_config(const CLI::App* app, bool default_also, bool, std::string) const {
    // Collect (path, value) pairs, then write them as nested mappings.
    std::vector<std::pair<std::string, std::string>> leaves;
    auto value_of = [&](const std::string& opt_name, std::string& out) {
        const CLI::Option* opt = app->get_option_no_throw("--" + opt_name);
        if (opt == nullptr)
            return false;
        std::vector<std::string> vals;
        if (opt->count() > 0) {
            auto t = g_text.find(opt_name);
            vals = t != g_text.end() ? t->second : opt->results();
        } else if (default_also && !opt->get_default_str().empty()) {
            vals = {opt->get_default_str()};
        } else if (default_also && opt->get_expected_min() == 0) {
            vals = {"false"};      // unset flag
        } else {
            return false;
        }
        bool flag = opt->get_expected_min() == 0;
        for (auto& v : vals) {
            if (flag && (v == "0" || v == "1"))
                v = v == "1" ? "true" : "false";
            v = yaml_scalar(v);
        }
        if (vals.size() == 1)
            out = vals[0];
        else {
            out = "[";
            for (size_t i = 0; i < vals.size(); ++i)
                out += (i ? ", " : "") + vals[i];
            out += "]";
        }
        return true;
    };

    std::string v;
    for (const auto& [path, opt] : kKeys)
        if (value_of(opt, v))
            leaves.emplace_back(path, v);
    for (const auto& name : PerformanceModel::paramNames()) {
        if (!value_of(name, v))
            continue;
        auto us = name.find('_');
        leaves.emplace_back(name.substr(0, us) + "." + name.substr(us + 1), v);
    }

    // Group leaves by section, keeping the first-seen order of sections.
    std::ostringstream os;
    os << "# CacheSim effective configuration (--dump-config)\n";
    std::vector<std::string> printed;   // path components currently open
    std::vector<std::string> order;
    for (const auto& [path, val] : leaves) {
        std::string top = path.substr(0, path.find('.'));
        if (std::find(order.begin(), order.end(), top) == order.end())
            order.push_back(top);
    }
    for (const auto& top : order) {
        printed.clear();
        for (const auto& [path, val] : leaves) {
            if (path.substr(0, path.find('.')) != top)
                continue;
            std::vector<std::string> parts;
            std::stringstream ss(path);
            for (std::string p; std::getline(ss, p, '.');)
                parts.push_back(p);
            size_t common = 0;
            while (common + 1 < parts.size() && common < printed.size() && printed[common] == parts[common])
                ++common;
            printed.resize(common);
            if (common == 0 && parts.size() > 1)
                os << "\n";
            for (size_t i = common; i + 1 < parts.size(); ++i) {
                os << std::string(2 * i, ' ') << parts[i] << ":\n";
                printed.push_back(parts[i]);
            }
            os << std::string(2 * (parts.size() - 1), ' ') << parts.back() << ": " << val << "\n";
        }
    }
    return os.str();
}


std::function<std::string(std::string)> keep_text(const std::string& option) {
    return [option](std::string v) {
        g_text[option].push_back(v);
        return v;
    };
}

static std::map<std::string, uint64_t> g_timing;

void add_timing_options(CLI::App& app) {
    auto* group = app.add_option_group("Timing model",
        "Timing-model parameters; in YAML they sit under cpu/l1i/l1d/l2/llc/memory (llc.latency = --llc_latency)");
    for (const auto& name : PerformanceModel::paramNames()) {
        group->add_option_function<double>("--" + name, [name](const double& v) {
            g_timing[name] = static_cast<uint64_t>(v);
        }, "timing parameter " + name);
    }
}

const std::map<std::string, uint64_t>& timing_params() { return g_timing; }

}  // namespace yaml_config
