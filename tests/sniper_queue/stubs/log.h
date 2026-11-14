#pragma once
#include <cstdio>
#include <cstdlib>
#define LOG_PRINT(...) do {} while (0)
#define LOG_PRINT_WARNING_ONCE(...) do {} while (0)
#define LOG_PRINT_WARNING(...) do {} while (0)
#define LOG_PRINT_ERROR(...) do { std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); std::abort(); } while (0)
#define LOG_ASSERT_ERROR(cond, ...) do { if (!(cond)) { LOG_PRINT_ERROR(__VA_ARGS__); } } while (0)
