// ============================================================================
//  src/statistics_mcp_tool_server.cpp — MCP Statistics and Arithmetic Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides summary, arithmetic, mean, and dispersion tools. Computations are
//  stateless and validate their inputs. Runs over stdio by default or
//  Streamable HTTP on port 8096 by default.
// ============================================================================

#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/server.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pooriayousefi::mcp
{
    constexpr std::size_t MAX_STATS_NUMBERS = 1'000'000;

    // Shared Json schema properties (mirrors git server's cwd_prop).
    const Json numbers_prop = {{"type", "array"}, {"items", {{"type", "number"}}}};
    const Json values_prop = {{"type", "array"}, {"items", {{"type", "number"}}}};
    const Json weights_prop = {{"type", "array"}, {"items", {{"type", "number"}}}};
    const Json operation_prop = {{"type", "string"}, {"enum", Json::array({"add", "subtract", "multiply", "divide", "modulo", "power"})}};
    const Json a_prop = {{"type", "number"}};
    const Json b_prop = {{"type", "number"}};
    const Json p_prop = {{"type", "number"}};
    const Json mean_prop = {{"type", "number"}};
    const Json trim_percent_prop = {{"type", "number"}};
    const Json winsor_percent_prop = {{"type", "number"}};

    double get_number(const Json& args, const char* key, double fallback)
    {
        double result = fallback;
        if (args.contains(key) && args[key].is_number())
        {
            result = args[key].get<double>();
        }
        return result;
    }

    std::optional<std::vector<double>> parse_number_array(const Json& args, const char* key)
    {
        std::optional<std::vector<double>> result;
        if (args.contains(key) && args[key].is_array())
        {
            const auto& arr = args[key];
            bool all_numbers = std::all_of(arr.begin(), arr.end(), [](const Json& v)
            {
                return v.is_number();
            });
            if (all_numbers)
            {
                std::vector<double> vec;
                vec.reserve(arr.size());
                for (const auto& v : arr)
                {
                    vec.push_back(v.get<double>());
                }
                result = std::move(vec);
            }
        }
        return result;
    }

    double quantile(const std::vector<double>& sorted_vals, double q)
    {
        double result = 0.0;
        std::size_t n = sorted_vals.size();
        double pos = q * static_cast<double>(n - 1);
        std::size_t lower = static_cast<std::size_t>(std::floor(pos));
        std::size_t upper = static_cast<std::size_t>(std::ceil(pos));
        double frac = pos - static_cast<double>(lower);
        if (lower == upper)
        {
            result = sorted_vals[lower];
        }
        else
        {
            result = sorted_vals[lower] * (1.0 - frac) + sorted_vals[upper] * frac;
        }
        return result;
    }

    Expected<Json, Error> mean_wrapper(
        const Json& args,
        const std::function<double(const std::vector<double>&)>& mean_fn
    )
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");

        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else
        {
            double res = mean_fn(*vals_opt);
            if (!std::isfinite(res))
            {
                result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
            }
            else
            {
                result.set_value(Json{{"value", res}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_calculate_statistics(const Json& args)
    {
        Expected<Json, Error> result{};
        auto nums_opt = parse_number_array(args, "numbers");

        if (!nums_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'numbers' array in arguments"});
        }
        else if (nums_opt->size() > MAX_STATS_NUMBERS)
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "'numbers' array exceeds the maximum of " + std::to_string(MAX_STATS_NUMBERS) + " elements."
            });
        }
        else if (nums_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "The numbers array is empty."});
        }
        else
        {
            const auto& nums = *nums_opt;
            std::size_t n = nums.size();
            double sum = std::accumulate(nums.begin(), nums.end(), 0.0);
            double mean = sum / static_cast<double>(n);

            std::vector<double> sorted = nums;
            std::sort(sorted.begin(), sorted.end());
            double min_val = sorted.front();
            double max_val = sorted.back();
            double median = (n % 2 == 0)
                ? (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0
                : sorted[n / 2];

            double variance = 0.0;
            for (double v : nums)
            {
                double d = v - mean;
                variance += d * d;
            }
            variance /= static_cast<double>(n);
            double std_dev = std::sqrt(variance);

            Json payload = {
                {"count", static_cast<double>(n)},
                {"sum", sum},
                {"mean", mean},
                {"median", median},
                {"min", min_val},
                {"max", max_val},
                {"variance", variance},
                {"standard_deviation", std_dev}
            };

            if (std::all_of(nums.begin(), nums.end(), [](double v)
            {
                return v > 0.0;
            }))
            {
                double product = 1.0;
                for (double v : nums)
                {
                    product *= v;
                }
                payload["geometric_mean"] = std::pow(product, 1.0 / static_cast<double>(n));
            }
            if (std::all_of(nums.begin(), nums.end(), [](double v)
            {
                return v != 0.0;
            }))
            {
                double sum_reciprocals = 0.0;
                for (double v : nums)
                {
                    sum_reciprocals += 1.0 / v;
                }
                payload["harmonic_mean"] = static_cast<double>(n) / sum_reciprocals;
            }
            double sum_squares = 0.0;
            for (double v : nums)
            {
                sum_squares += v * v;
            }
            payload["quadratic_mean"] = std::sqrt(sum_squares / static_cast<double>(n));

            result.set_value(std::move(payload));
        }
        return result;
    }

    Expected<Json, Error> tool_arithmetic_operation(const Json& args)
    {
        Expected<Json, Error> result{};

        if (!args.contains("operation") || !args.contains("a") || !args.contains("b"))
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing 'operation', 'a', or 'b' in arguments"});
        }
        else if (!args["operation"].is_string() || !args["a"].is_number() || !args["b"].is_number())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'operation' must be a string, and 'a'/'b' must be numbers."});
        }
        else
        {
            std::string op = args["operation"].get<std::string>();
            double a = args["a"].get<double>();
            double b = args["b"].get<double>();

            if (op == "add")
            {
                result.set_value(Json{{"value", a + b}});
            }
            else if (op == "subtract")
            {
                result.set_value(Json{{"value", a - b}});
            }
            else if (op == "multiply")
            {
                result.set_value(Json{{"value", a * b}});
            }
            else if (op == "divide")
            {
                if (b == 0.0)
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "Division by zero"});
                }
                else
                {
                    result.set_value(Json{{"value", a / b}});
                }
            }
            else if (op == "modulo")
            {
                if (b == 0.0)
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "Modulo by zero"});
                }
                else
                {
                    result.set_value(Json{{"value", std::fmod(a, b)}});
                }
            }
            else if (op == "power")
            {
                result.set_value(Json{{"value", std::pow(a, b)}});
            }
            else
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Unknown operation: " + op});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_arithmetic_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            return std::accumulate(vals.begin(), vals.end(), 0.0) / static_cast<double>(vals.size());
        });
    }

    Expected<Json, Error> tool_geometric_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            double product = 1.0;
            for (double v : vals)
            {
                product *= v;
            }
            return std::pow(product, 1.0 / static_cast<double>(vals.size()));
        });
    }

    Expected<Json, Error> tool_harmonic_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            double sum_reciprocals = 0.0;
            for (double v : vals)
            {
                sum_reciprocals += 1.0 / v;
            }
            return static_cast<double>(vals.size()) / sum_reciprocals;
        });
    }

    Expected<Json, Error> tool_contraharmonic_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            double sum_sq = 0.0;
            double sum = 0.0;
            for (double v : vals)
            {
                sum_sq += v * v;
                sum += v;
            }
            return sum_sq / sum;
        });
    }

    Expected<Json, Error> tool_quadratic_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            double sum_sq = 0.0;
            for (double v : vals)
            {
                sum_sq += v * v;
            }
            return std::sqrt(sum_sq / static_cast<double>(vals.size()));
        });
    }

    Expected<Json, Error> tool_cubic_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            double sum_cb = 0.0;
            for (double v : vals)
            {
                sum_cb += v * v * v;
            }
            return std::cbrt(sum_cb / static_cast<double>(vals.size()));
        });
    }

    Expected<Json, Error> tool_midrange_mean(const Json& args)
    {
        return mean_wrapper(args, [](const std::vector<double>& vals)
        {
            auto [min_it, max_it] = std::minmax_element(vals.begin(), vals.end());
            return (*min_it + *max_it) / 2.0;
        });
    }

    Expected<Json, Error> tool_generalized_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        double p = get_number(args, "p", 1.0);

        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else if (p == 0.0)
        {
            result = tool_geometric_mean(args);
        }
        else
        {
            double sum = 0.0;
            for (double v : *vals_opt)
            {
                sum += std::pow(v, p);
            }
            double res = std::pow(sum / static_cast<double>(vals_opt->size()), 1.0 / p);
            if (!std::isfinite(res))
            {
                result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
            }
            else
            {
                result.set_value(Json{{"value", res}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_weighted_generalized_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        auto wts_opt = parse_number_array(args, "weights");
        double p = get_number(args, "p", 1.0);

        if (!vals_opt.has_value() || !wts_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' or 'weights' array."});
        }
        else if (vals_opt->empty() || wts_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays cannot be empty."});
        }
        else if (vals_opt->size() != wts_opt->size())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays must have the same length."});
        }
        else
        {
            const auto& vals = *vals_opt;
            const auto& wts = *wts_opt;
            bool has_neg_weight = std::any_of(wts.begin(), wts.end(), [](double w)
            {
                return w < 0.0;
            });
            bool has_neg_value = std::any_of(vals.begin(), vals.end(), [](double v)
            {
                return v < 0.0;
            });

            if (p == 0.0)
            {
                if (has_neg_weight || has_neg_value)
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "Weighted generalized mean with p=0 requires positive values and non-negative weights."});
                }
                else
                {
                    double sum_w = std::accumulate(wts.begin(), wts.end(), 0.0);
                    double sum_wlogv = 0.0;
                    for (std::size_t i = 0; i < vals.size(); i++)
                    {
                        sum_wlogv += wts[i] * std::log(vals[i]);
                    }
                    double res = std::exp(sum_wlogv / sum_w);
                    if (!std::isfinite(res))
                    {
                        result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                    }
                    else
                    {
                        result.set_value(Json{{"value", res}});
                    }
                }
            }
            else if (has_neg_weight)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Weighted generalized mean requires non-negative weights."});
            }
            else
            {
                double sum_w = std::accumulate(wts.begin(), wts.end(), 0.0);
                if (sum_w == 0.0)
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "Sum of weights is zero."});
                }
                else
                {
                    double sum_wp = 0.0;
                    for (std::size_t i = 0; i < vals.size(); i++)
                    {
                        sum_wp += wts[i] * std::pow(vals[i], p);
                    }
                    double res = std::pow(sum_wp / sum_w, 1.0 / p);
                    if (!std::isfinite(res))
                    {
                        result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                    }
                    else
                    {
                        result.set_value(Json{{"value", res}});
                    }
                }
            }
        }
        return result;
    }

    Expected<Json, Error> tool_lehmer_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        double p = get_number(args, "p", 1.0);

        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else
        {
            double sum_v_p = 0.0;
            double sum_v_p1 = 0.0;
            for (double v : *vals_opt)
            {
                sum_v_p += std::pow(v, p);
                sum_v_p1 += std::pow(v, p + 1.0);
            }
            if (sum_v_p == 0.0)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Division by zero in Lehmer mean."});
            }
            else
            {
                double res = sum_v_p1 / sum_v_p;
                if (!std::isfinite(res))
                {
                    result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                }
                else
                {
                    result.set_value(Json{{"value", res}});
                }
            }
        }
        return result;
    }

    Expected<Json, Error> tool_weighted_lehmer_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        auto wts_opt = parse_number_array(args, "weights");
        double p = get_number(args, "p", 1.0);

        if (!vals_opt.has_value() || !wts_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' or 'weights' array."});
        }
        else if (vals_opt->empty() || wts_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays cannot be empty."});
        }
        else if (vals_opt->size() != wts_opt->size())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays must have the same length."});
        }
        else if (std::any_of(wts_opt->begin(), wts_opt->end(), [](double w)
        {
            return w < 0.0;
        }))
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Weights must be non-negative."});
        }
        else
        {
            double sum_wv_p = 0.0;
            double sum_wv_p1 = 0.0;
            for (std::size_t i = 0; i < vals_opt->size(); i++)
            {
                sum_wv_p += (*wts_opt)[i] * std::pow((*vals_opt)[i], p);
                sum_wv_p1 += (*wts_opt)[i] * std::pow((*vals_opt)[i], p + 1.0);
            }
            if (sum_wv_p == 0.0)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Division by zero in weighted Lehmer mean."});
            }
            else
            {
                double res = sum_wv_p1 / sum_wv_p;
                if (!std::isfinite(res))
                {
                    result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                }
                else
                {
                    result.set_value(Json{{"value", res}});
                }
            }
        }
        return result;
    }

    Expected<Json, Error> tool_trimean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->size() < 4)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Trimean requires at least 4 values."});
        }
        else
        {
            std::vector<double> sorted = *vals_opt;
            std::sort(sorted.begin(), sorted.end());
            double q1 = quantile(sorted, 0.25);
            double med = quantile(sorted, 0.50);
            double q3 = quantile(sorted, 0.75);
            result.set_value(Json{{"value", (q1 + 2.0 * med + q3) / 4.0}});
        }
        return result;
    }

    Expected<Json, Error> tool_interquartile_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else
        {
            std::vector<double> sorted = *vals_opt;
            std::sort(sorted.begin(), sorted.end());
            std::size_t n = sorted.size();
            std::size_t trim = n / 4;
            if (n == 2 * trim)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Not enough values to calculate interquartile mean."});
            }
            else
            {
                double sum = 0.0;
                for (std::size_t i = trim; i < n - trim; i++)
                {
                    sum += sorted[i];
                }
                result.set_value(Json{{"value", sum / static_cast<double>(n - 2 * trim)}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_trimmed_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        double trim = get_number(args, "trim_percent", 5.0);
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (trim < 0.0 || trim >= 50.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'trim_percent' must be between 0 and 50 (exclusive)."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else
        {
            std::vector<double> sorted = *vals_opt;
            std::sort(sorted.begin(), sorted.end());
            std::size_t n = sorted.size();
            std::size_t trim_count = static_cast<std::size_t>(std::floor(trim / 100.0 * static_cast<double>(n)));
            if (n == 2 * trim_count)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Trim count exceeds array size. Reduce trim_percent."});
            }
            else
            {
                double sum = 0.0;
                for (std::size_t i = trim_count; i < n - trim_count; i++)
                {
                    sum += sorted[i];
                }
                result.set_value(Json{{"value", sum / static_cast<double>(n - 2 * trim_count)}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_winsorized_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        double winsor = get_number(args, "winsor_percent", 5.0);
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (winsor < 0.0 || winsor >= 50.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'winsor_percent' must be between 0 and 50 (exclusive)."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else
        {
            std::vector<double> sorted = *vals_opt;
            std::sort(sorted.begin(), sorted.end());
            std::size_t n = sorted.size();
            std::size_t winsor_count = static_cast<std::size_t>(std::floor(winsor / 100.0 * static_cast<double>(n)));
            if (n != 1 && n == 2 * winsor_count)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Winsor count exceeds array size. Reduce winsor_percent."});
            }
            else
            {
                double lower = (winsor_count > 0) ? sorted[winsor_count - 1] : sorted.front();
                double upper = (winsor_count > 0) ? sorted[n - winsor_count] : sorted.back();
                double sum = 0.0;
                for (std::size_t i = 0; i < n; i++)
                {
                    if (i < winsor_count)
                    {
                        sum += lower;
                    }
                    else if (i >= n - winsor_count)
                    {
                        sum += upper;
                    }
                    else
                    {
                        sum += sorted[i];
                    }
                }
                result.set_value(Json{{"value", sum / static_cast<double>(n)}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_logarithmic_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->size() != 2)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Logarithmic mean is strictly defined for exactly 2 numbers."});
        }
        else
        {
            double x = (*vals_opt)[0];
            double y = (*vals_opt)[1];
            if (x <= 0.0 || y <= 0.0)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Logarithmic mean requires strictly positive numbers."});
            }
            else if (x == y)
            {
                result.set_value(Json{{"value", x}});
            }
            else
            {
                double res = (y - x) / (std::log(y) - std::log(x));
                if (!std::isfinite(res))
                {
                    result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite."});
                }
                else
                {
                    result.set_value(Json{{"value", res}});
                }
            }
        }
        return result;
    }


    Expected<Json, Error> tool_arithmetic_geometric_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->size() != 2)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Arithmetic-Geometric mean is strictly defined for exactly 2 numbers."});
        }
        else
        {
            double a = (*vals_opt)[0];
            double b = (*vals_opt)[1];
            if (a < 0.0 || b < 0.0)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Arithmetic-Geometric mean requires non-negative numbers."});
            }
            else
            {
                int iter = 0;
                while (iter < 1000 && std::abs(a - b) >= 1e-15)
                {
                    double new_a = (a + b) / 2.0;
                    double new_b = std::sqrt(a * b);
                    a = new_a;
                    b = new_b;
                    iter++;
                }
                result.set_value(Json{{"value", (a + b) / 2.0}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_geometric_harmonic_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->size() != 2)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Geometric-Harmonic mean is strictly defined for exactly 2 numbers."});
        }
        else
        {
            double a = (*vals_opt)[0];
            double b = (*vals_opt)[1];
            if (a <= 0.0 || b <= 0.0)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Geometric-Harmonic mean requires strictly positive numbers."});
            }
            else
            {
                int iter = 0;
                while (iter < 1000 && std::abs(a - b) >= 1e-15)
                {
                    double new_a = std::sqrt(a * b);
                    double new_b = (2.0 * a * b) / (a + b);
                    a = new_a;
                    b = new_b;
                    iter++;
                }
                result.set_value(Json{{"value", std::sqrt(a * b)}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_weighted_arithmetic_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        auto wts_opt = parse_number_array(args, "weights");
        if (!vals_opt.has_value() || !wts_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' or 'weights' array."});
        }
        else if (vals_opt->empty() || wts_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays cannot be empty."});
        }
        else if (vals_opt->size() != wts_opt->size())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays must have the same length."});
        }
        else if (std::any_of(wts_opt->begin(), wts_opt->end(), [](double w)
        {
            return w < 0.0;
        }))
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Weights must be non-negative."});
        }
        else
        {
            double sum_wv = 0.0;
            double sum_w = 0.0;
            for (std::size_t i = 0; i < vals_opt->size(); i++)
            {
                sum_wv += (*wts_opt)[i] * (*vals_opt)[i];
                sum_w += (*wts_opt)[i];
            }
            if (sum_w == 0.0)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Sum of weights is zero."});
            }
            else
            {
                double res = sum_wv / sum_w;
                if (!std::isfinite(res))
                {
                    result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                }
                else
                {
                    result.set_value(Json{{"value", res}});
                }
            }
        }
        return result;
    }

    Expected<Json, Error> tool_weighted_geometric_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        auto wts_opt = parse_number_array(args, "weights");
        if (!vals_opt.has_value() || !wts_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' or 'weights' array."});
        }
        else if (vals_opt->empty() || wts_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays cannot be empty."});
        }
        else if (vals_opt->size() != wts_opt->size())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays must have the same length."});
        }
        else
        {
            bool has_neg_weight = std::any_of(wts_opt->begin(), wts_opt->end(), [](double w)
            {
                return w < 0.0;
            });
            bool has_non_positive_value = std::any_of(vals_opt->begin(), vals_opt->end(), [](double v)
            {
                return v <= 0.0;
            });
            if (has_neg_weight || has_non_positive_value)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Weighted geometric mean requires positive values and non-negative weights."});
            }
            else
            {
                double sum_w = std::accumulate(wts_opt->begin(), wts_opt->end(), 0.0);
                double sum_wlogv = 0.0;
                for (std::size_t i = 0; i < vals_opt->size(); i++)
                {
                    sum_wlogv += (*wts_opt)[i] * std::log((*vals_opt)[i]);
                }
                if (sum_w == 0.0)
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "Sum of weights is zero."});
                }
                else
                {
                    double res = std::exp(sum_wlogv / sum_w);
                    if (!std::isfinite(res))
                    {
                        result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                    }
                    else
                    {
                        result.set_value(Json{{"value", res}});
                    }
                }
            }
        }
        return result;
    }
    Expected<Json, Error> tool_weighted_harmonic_mean(const Json& args)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        auto wts_opt = parse_number_array(args, "weights");
        if (!vals_opt.has_value() || !wts_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' or 'weights' array."});
        }
        else if (vals_opt->empty() || wts_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays cannot be empty."});
        }
        else if (vals_opt->size() != wts_opt->size())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' and 'weights' arrays must have the same length."});
        }
        else
        {
            bool has_neg_weight = std::any_of(wts_opt->begin(), wts_opt->end(), [](double w)
            {
                return w < 0.0;
            });
            bool has_zero_value = std::any_of(vals_opt->begin(), vals_opt->end(), [](double v)
            {
                return v == 0.0;
            });
            if (has_neg_weight || has_zero_value)
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Weighted harmonic mean requires non-zero values and non-negative weights."});
            }
            else
            {
                double sum_w = std::accumulate(wts_opt->begin(), wts_opt->end(), 0.0);
                double sum_w_over_v = 0.0;
                for (std::size_t i = 0; i < vals_opt->size(); i++)
                {
                    sum_w_over_v += (*wts_opt)[i] / (*vals_opt)[i];
                }
                double res = sum_w / sum_w_over_v;
                if (!std::isfinite(res))
                {
                    result.set_error(Error{error_code::INTERNAL_ERROR, "Result is not finite (overflow)."});
                }
                else
                {
                    result.set_value(Json{{"value", res}});
                }
            }
        }
        return result;
    }

    Expected<Json, Error> tool_variance_impl(const Json& args, bool sample)
    {
        Expected<Json, Error> result{};
        auto vals_opt = parse_number_array(args, "values");
        if (!vals_opt.has_value())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or invalid 'values' array."});
        }
        else if (vals_opt->empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'values' array is empty."});
        }
        else if (sample && vals_opt->size() < 2)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Sample variance requires at least 2 values."});
        }
        else
        {
            const auto& vals = *vals_opt;
            double mean = get_number(args, "mean", std::accumulate(vals.begin(), vals.end(), 0.0) / static_cast<double>(vals.size()));
            double sum_sq = 0.0;
            for (double v : vals)
            {
                double d = v - mean;
                sum_sq += d * d;
            }
            double denom = sample ? static_cast<double>(vals.size() - 1) : static_cast<double>(vals.size());
            result.set_value(Json{{"value", sum_sq / denom}});
        }
        return result;
    }

    Expected<Json, Error> tool_stddev_impl(const Json& args, bool sample)
    {
        Expected<Json, Error> result{};
        auto var = tool_variance_impl(args, sample);
        if (!var.has_value())
        {
            result.set_error(var.get_error());
        }
        else
        {
            result.set_value(Json{{"value", std::sqrt(var.get_value()["value"].get<double>())}});
        }
        return result;
    }

    Expected<Json, Error> tool_variance(const Json& args)
    {
        return tool_variance_impl(args, false);
    }

    Expected<Json, Error> tool_standard_deviation(const Json& args)
    {
        return tool_stddev_impl(args, false);
    }

    Expected<Json, Error> tool_sample_variance(const Json& args)
    {
        return tool_variance_impl(args, true);
    }

    Expected<Json, Error> tool_sample_standard_deviation(const Json& args)
    {
        return tool_stddev_impl(args, true);
    }
}

int main(int argc, char* argv[])
{
    using namespace pooriayousefi::mcp;

    Server server{"mcp-tool-statistics-server", "1.0.0"};

    server.set_instructions(
        "Statistics and arithmetic tool server over MCP 2026-07-28. "
        "All tools are pure, stateless numeric computations: no filesystem, network, or shell access. "
        "Use calculate_statistics for a one-shot descriptive summary (count, sum, mean, median, min, max, "
        "variance, standard deviation, plus geometric/harmonic/quadratic means when mathematically defined). "
        "Use arithmetic_operation for exact two-operand arithmetic (add, subtract, multiply, divide, modulo, power). "
        "The remaining tools are specialized means: unweighted ones take 'values'; weighted ones take 'values' "
        "plus 'weights'; generalized and Lehmer means take an optional exponent 'p'; trimmed and winsorized means "
        "take 'trim_percent'/'winsor_percent' in [0, 50); dispersion tools take 'values' and an optional "
        "precomputed 'mean'. Every tool returns Json; empty arrays, mismatched lengths, division by zero, and "
        "domain violations (e.g. non-positive values for geometric/harmonic means) return INVALID_PARAMS, "
        "and non-finite overflow results return INTERNAL_ERROR."
    );

    Tool calculate_statistics{};
    calculate_statistics.name = "calculate_statistics";
    calculate_statistics.title = "Calculate statistics";
    calculate_statistics.description = "Calculates exact statistical metrics for a list of numbers.";
    calculate_statistics.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"numbers", numbers_prop}
        }},
        {"required", Json::array({"numbers"})}
    };
    calculate_statistics.handler = tool_calculate_statistics;
    server.add_tool(std::move(calculate_statistics));

    Tool arithmetic_operation{};
    arithmetic_operation.name = "arithmetic_operation";
    arithmetic_operation.title = "Arithmetic operation";
    arithmetic_operation.description = "Performs exact arithmetic on two numbers.";
    arithmetic_operation.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"operation", operation_prop},
            {"a", a_prop},
            {"b", b_prop}
        }},
        {"required", Json::array({"operation", "a", "b"})}
    };
    arithmetic_operation.handler = tool_arithmetic_operation;
    server.add_tool(std::move(arithmetic_operation));

    Tool arithmetic_mean{};
    arithmetic_mean.name = "arithmetic_mean";
    arithmetic_mean.title = "Arithmetic mean";
    arithmetic_mean.description = "Arithmetic mean.";
    arithmetic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    arithmetic_mean.handler = tool_arithmetic_mean;
    server.add_tool(std::move(arithmetic_mean));

    Tool geometric_mean{};
    geometric_mean.name = "geometric_mean";
    geometric_mean.title = "Geometric mean";
    geometric_mean.description = "Geometric mean (non-negative).";
    geometric_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    geometric_mean.handler = tool_geometric_mean;
    server.add_tool(std::move(geometric_mean));

    Tool harmonic_mean{};
    harmonic_mean.name = "harmonic_mean";
    harmonic_mean.title = "Harmonic mean";
    harmonic_mean.description = "Harmonic mean (non-zero).";
    harmonic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    harmonic_mean.handler = tool_harmonic_mean;
    server.add_tool(std::move(harmonic_mean));

    Tool generalized_mean{};
    generalized_mean.name = "generalized_mean";
    generalized_mean.title = "Generalized mean";
    generalized_mean.description = "Power / Holder mean.";
    generalized_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"p", p_prop}
        }},
        {"required", Json::array({"values", "p"})}
    };
    generalized_mean.handler = tool_generalized_mean;
    server.add_tool(std::move(generalized_mean));

    Tool weighted_generalized_mean{};
    weighted_generalized_mean.name = "weighted_generalized_mean";
    weighted_generalized_mean.title = "Weighted generalized mean";
    weighted_generalized_mean.description = "Weighted power mean.";
    weighted_generalized_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"weights", weights_prop},
            {"p", p_prop}
        }},
        {"required", Json::array({"values", "weights", "p"})}
    };
    weighted_generalized_mean.handler = tool_weighted_generalized_mean;
    server.add_tool(std::move(weighted_generalized_mean));

    Tool lehmer_mean{};
    lehmer_mean.name = "lehmer_mean";
    lehmer_mean.title = "Lehmer mean";
    lehmer_mean.description = "Lehmer mean.";
    lehmer_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"p", p_prop}
        }},
        {"required", Json::array({"values", "p"})}
    };
    lehmer_mean.handler = tool_lehmer_mean;
    server.add_tool(std::move(lehmer_mean));

    Tool weighted_lehmer_mean{};
    weighted_lehmer_mean.name = "weighted_lehmer_mean";
    weighted_lehmer_mean.title = "Weighted Lehmer mean";
    weighted_lehmer_mean.description = "Weighted Lehmer mean.";
    weighted_lehmer_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"weights", weights_prop},
            {"p", p_prop}
        }},
        {"required", Json::array({"values", "weights", "p"})}
    };
    weighted_lehmer_mean.handler = tool_weighted_lehmer_mean;
    server.add_tool(std::move(weighted_lehmer_mean));

    Tool contraharmonic_mean{};
    contraharmonic_mean.name = "contraharmonic_mean";
    contraharmonic_mean.title = "Contraharmonic mean";
    contraharmonic_mean.description = "Contraharmonic mean (Lehmer p=2).";
    contraharmonic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    contraharmonic_mean.handler = tool_contraharmonic_mean;
    server.add_tool(std::move(contraharmonic_mean));

    Tool quadratic_mean{};
    quadratic_mean.name = "quadratic_mean";
    quadratic_mean.title = "Quadratic mean";
    quadratic_mean.description = "RMS / quadratic mean.";
    quadratic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    quadratic_mean.handler = tool_quadratic_mean;
    server.add_tool(std::move(quadratic_mean));

    Tool cubic_mean{};
    cubic_mean.name = "cubic_mean";
    cubic_mean.title = "Cubic mean";
    cubic_mean.description = "Cubic mean.";
    cubic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    cubic_mean.handler = tool_cubic_mean;
    server.add_tool(std::move(cubic_mean));

    Tool midrange_mean{};
    midrange_mean.name = "midrange_mean";
    midrange_mean.title = "Midrange mean";
    midrange_mean.description = "Midrange (min+max)/2.";
    midrange_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    midrange_mean.handler = tool_midrange_mean;
    server.add_tool(std::move(midrange_mean));

    Tool trimean{};
    trimean.name = "trimean";
    trimean.title = "Trimean";
    trimean.description = "Tukey trimean.";
    trimean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    trimean.handler = tool_trimean;
    server.add_tool(std::move(trimean));

    Tool interquartile_mean{};
    interquartile_mean.name = "interquartile_mean";
    interquartile_mean.title = "Interquartile mean";
    interquartile_mean.description = "Interquartile mean (midmean).";
    interquartile_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    interquartile_mean.handler = tool_interquartile_mean;
    server.add_tool(std::move(interquartile_mean));

    Tool trimmed_mean{};
    trimmed_mean.name = "trimmed_mean";
    trimmed_mean.title = "Trimmed mean";
    trimmed_mean.description = "Trimmed mean.";
    trimmed_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"trim_percent", trim_percent_prop}
        }},
        {"required", Json::array({"values", "trim_percent"})}
    };
    trimmed_mean.handler = tool_trimmed_mean;
    server.add_tool(std::move(trimmed_mean));

    Tool winsorized_mean{};
    winsorized_mean.name = "winsorized_mean";
    winsorized_mean.title = "Winsorized mean";
    winsorized_mean.description = "Winsorized mean.";
    winsorized_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"winsor_percent", winsor_percent_prop}
        }},
        {"required", Json::array({"values", "winsor_percent"})}
    };
    winsorized_mean.handler = tool_winsorized_mean;
    server.add_tool(std::move(winsorized_mean));

    Tool logarithmic_mean{};
    logarithmic_mean.name = "logarithmic_mean";
    logarithmic_mean.title = "Logarithmic mean";
    logarithmic_mean.description = "Logarithmic mean (exactly 2 positives).";
    logarithmic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    logarithmic_mean.handler = tool_logarithmic_mean;
    server.add_tool(std::move(logarithmic_mean));

    Tool arithmetic_geometric_mean{};
    arithmetic_geometric_mean.name = "arithmetic_geometric_mean";
    arithmetic_geometric_mean.title = "Arithmetic-geometric mean";
    arithmetic_geometric_mean.description = "AGM (exactly 2 non-negative).";
    arithmetic_geometric_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    arithmetic_geometric_mean.handler = tool_arithmetic_geometric_mean;
    server.add_tool(std::move(arithmetic_geometric_mean));

    Tool geometric_harmonic_mean{};
    geometric_harmonic_mean.name = "geometric_harmonic_mean";
    geometric_harmonic_mean.title = "Geometric-harmonic mean";
    geometric_harmonic_mean.description = "GHM (exactly 2 positives).";
    geometric_harmonic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop}
        }},
        {"required", Json::array({"values"})}
    };
    geometric_harmonic_mean.handler = tool_geometric_harmonic_mean;
    server.add_tool(std::move(geometric_harmonic_mean));

    Tool weighted_arithmetic_mean{};
    weighted_arithmetic_mean.name = "weighted_arithmetic_mean";
    weighted_arithmetic_mean.title = "Weighted arithmetic mean";
    weighted_arithmetic_mean.description = "Weighted arithmetic mean.";
    weighted_arithmetic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"weights", weights_prop}
        }},
        {"required", Json::array({"values", "weights"})}
    };
    weighted_arithmetic_mean.handler = tool_weighted_arithmetic_mean;
    server.add_tool(std::move(weighted_arithmetic_mean));

    Tool weighted_geometric_mean{};
    weighted_geometric_mean.name = "weighted_geometric_mean";
    weighted_geometric_mean.title = "Weighted geometric mean";
    weighted_geometric_mean.description = "Weighted geometric mean.";
    weighted_geometric_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"weights", weights_prop}
        }},
        {"required", Json::array({"values", "weights"})}
    };
    weighted_geometric_mean.handler = tool_weighted_geometric_mean;
    server.add_tool(std::move(weighted_geometric_mean));

    Tool weighted_harmonic_mean{};
    weighted_harmonic_mean.name = "weighted_harmonic_mean";
    weighted_harmonic_mean.title = "Weighted harmonic mean";
    weighted_harmonic_mean.description = "Weighted harmonic mean.";
    weighted_harmonic_mean.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"weights", weights_prop}
        }},
        {"required", Json::array({"values", "weights"})}
    };
    weighted_harmonic_mean.handler = tool_weighted_harmonic_mean;
    server.add_tool(std::move(weighted_harmonic_mean));

    Tool variance{};
    variance.name = "variance";
    variance.title = "Variance";
    variance.description = "Population variance.";
    variance.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"mean", mean_prop}
        }},
        {"required", Json::array({"values"})}
    };
    variance.handler = tool_variance;
    server.add_tool(std::move(variance));

    Tool standard_deviation{};
    standard_deviation.name = "standard_deviation";
    standard_deviation.title = "Standard deviation";
    standard_deviation.description = "Population standard deviation.";
    standard_deviation.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"mean", mean_prop}
        }},
        {"required", Json::array({"values"})}
    };
    standard_deviation.handler = tool_standard_deviation;
    server.add_tool(std::move(standard_deviation));

    Tool sample_variance{};
    sample_variance.name = "sample_variance";
    sample_variance.title = "Sample variance";
    sample_variance.description = "Sample variance (n-1).";
    sample_variance.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"mean", mean_prop}
        }},
        {"required", Json::array({"values"})}
    };
    sample_variance.handler = tool_sample_variance;
    server.add_tool(std::move(sample_variance));

    Tool sample_standard_deviation{};
    sample_standard_deviation.name = "sample_standard_deviation";
    sample_standard_deviation.title = "Sample standard deviation";
    sample_standard_deviation.description = "Sample standard deviation (n-1).";
    sample_standard_deviation.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"values", values_prop},
            {"mean", mean_prop}
        }},
        {"required", Json::array({"values"})}
    };
    sample_standard_deviation.handler = tool_sample_standard_deviation;
    server.add_tool(std::move(sample_standard_deviation));

    const int exit_code = server.run_cli(argc, argv, 8096);
    return exit_code;
}