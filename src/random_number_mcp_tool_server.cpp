// ============================================================================
//  src/random_number_mcp_tool_server.cpp — MCP Random Distribution Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides 17 random distribution tools using standard-library generators.
//  Sampling requests validate parameters and limit the requested sample
//  count. Runs over stdio by default or Streamable HTTP on port 8095 by default.
// ============================================================================

#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/server.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <random>
#include <string>
#include <utility>

namespace pooriayousefi::mcp
{
    constexpr std::size_t MAX_RANDOM_COUNT = 100'000;

    double get_number(const Json& args, const char* key, double fallback)
    {
        double result = fallback;
        if (args.contains(key) && args[key].is_number())
        {
            result = args[key].get<double>();
        }
        return result;
    }

    int get_random_count(const Json& args)
    {
        int result = 1;
        if (args.contains("count") && args["count"].is_number())
        {
            int ic = args["count"].get<int>();
            if (ic < 1)
            {
                result = 1;
            }
            else if (ic > static_cast<int>(MAX_RANDOM_COUNT))
            {
                result = -1;
            }
            else
            {
                result = ic;
            }
        }
        return result;
    }

    std::mt19937_64& random_engine()
    {
        thread_local std::mt19937_64 gen{std::random_device{}()};
        return gen;
    }

    Expected<Json, Error> handle_random(const Json& args, const std::function<double()>& gen_fn)
    {
        Expected<Json, Error> result{};

        int count = get_random_count(args);

        if (count == -1)
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "'count' exceeds the maximum of " + std::to_string(MAX_RANDOM_COUNT) + "."
            });
        }
        else if (count == 1)
        {
            result.set_value(Json{{"value", gen_fn()}});
        }
        else
        {
            Json arr = Json::array();
            for (int i = 0; i < count; i++)
            {
                arr.push_back(gen_fn());
            }
            result.set_value(Json{{"values", std::move(arr)}, {"count", count}});
        }

        return result;
    }

    // ------------------------------------------------------------------ tools

    Expected<Json, Error> tool_random_uniform_int(const Json& args)
    {
        Expected<Json, Error> result{};

        double min_val = get_number(args, "min_val", 0.0);
        double max_val = get_number(args, "max_val", 100.0);
        int a = static_cast<int>(std::min(min_val, max_val));
        int b = static_cast<int>(std::max(min_val, max_val));

        result = handle_random(
            args,
            [a, b]()
            {
                std::uniform_int_distribution<int> dist(a, b);
                return static_cast<double>(dist(random_engine()));
            }
        );

        return result;
    }

    Expected<Json, Error> tool_random_uniform_real(const Json& args)
    {
        Expected<Json, Error> result{};

        double min_val = get_number(args, "min_val", 0.0);
        double max_val = get_number(args, "max_val", 1.0);
        double a = std::min(min_val, max_val);
        double b = std::max(min_val, max_val);

        result = handle_random(
            args,
            [a, b]()
            {
                std::uniform_real_distribution<double> dist(a, b);
                return dist(random_engine());
            }
        );

        return result;
    }

    Expected<Json, Error> tool_random_bernoulli(const Json& args)
    {
        Expected<Json, Error> result{};

        double p = get_number(args, "p", 0.5);
        if (p < 0.0 || p > 1.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Probability 'p' must be between 0 and 1."});
        }
        else
        {
            result = handle_random(
                args,
                [p]()
                {
                    std::bernoulli_distribution dist(p);
                    return dist(random_engine()) ? 1.0 : 0.0;
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_binomial(const Json& args)
    {
        Expected<Json, Error> result{};

        int t = static_cast<int>(get_number(args, "t", 1.0));
        double p = get_number(args, "p", 0.5);
        if (p < 0.0 || p > 1.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Probability 'p' must be between 0 and 1."});
        }
        else
        {
            result = handle_random(
                args,
                [t, p]()
                {
                    std::binomial_distribution<int> dist(t, p);
                    return static_cast<double>(dist(random_engine()));
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_geometric(const Json& args)
    {
        Expected<Json, Error> result{};

        double p = get_number(args, "p", 0.5);
        if (p < 0.0 || p > 1.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Probability 'p' must be between 0 and 1."});
        }
        else
        {
            result = handle_random(
                args,
                [p]()
                {
                    std::geometric_distribution<int> dist(p);
                    return static_cast<double>(dist(random_engine()));
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_negative_binomial(const Json& args)
    {
        Expected<Json, Error> result{};

        int k = static_cast<int>(get_number(args, "k", 1.0));
        double p = get_number(args, "p", 0.5);
        if (p < 0.0 || p > 1.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Probability 'p' must be between 0 and 1."});
        }
        else
        {
            result = handle_random(
                args,
                [k, p]()
                {
                    std::negative_binomial_distribution<int> dist(k, p);
                    return static_cast<double>(dist(random_engine()));
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_poisson(const Json& args)
    {
        Expected<Json, Error> result{};

        double mean = get_number(args, "mean", 1.0);
        if (mean < 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'mean' must be non-negative."});
        }
        else
        {
            result = handle_random(
                args,
                [mean]()
                {
                    std::poisson_distribution<int> dist(mean);
                    return static_cast<double>(dist(random_engine()));
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_exponential(const Json& args)
    {
        Expected<Json, Error> result{};

        double lambda = get_number(args, "lambda", 1.0);
        if (lambda <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'lambda' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [lambda]()
                {
                    std::exponential_distribution<double> dist(lambda);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_gamma(const Json& args)
    {
        Expected<Json, Error> result{};

        double alpha = get_number(args, "alpha", 1.0);
        double beta = get_number(args, "beta", 1.0);
        if (alpha <= 0.0 || beta <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'alpha' and 'beta' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [alpha, beta]()
                {
                    std::gamma_distribution<double> dist(alpha, beta);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_weibull(const Json& args)
    {
        Expected<Json, Error> result{};

        double shape = get_number(args, "shape", 1.0);
        double scale = get_number(args, "scale", 1.0);
        if (shape <= 0.0 || scale <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'shape' and 'scale' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [shape, scale]()
                {
                    std::weibull_distribution<double> dist(shape, scale);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_extreme_value(const Json& args)
    {
        Expected<Json, Error> result{};

        double loc = get_number(args, "location", 0.0);
        double scale = get_number(args, "scale", 1.0);
        if (scale <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'scale' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [loc, scale]()
                {
                    std::extreme_value_distribution<double> dist(loc, scale);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_normal(const Json& args)
    {
        Expected<Json, Error> result{};

        double mean = get_number(args, "mean", 0.0);
        double stddev = get_number(args, "stddev", 1.0);
        if (stddev < 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'stddev' must be >= 0."});
        }
        else
        {
            result = handle_random(
                args,
                [mean, stddev]()
                {
                    std::normal_distribution<double> dist(mean, stddev);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_lognormal(const Json& args)
    {
        Expected<Json, Error> result{};

        double mean = get_number(args, "mean", 0.0);
        double stddev = get_number(args, "stddev", 1.0);
        if (stddev < 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'stddev' must be >= 0."});
        }
        else
        {
            result = handle_random(
                args,
                [mean, stddev]()
                {
                    std::lognormal_distribution<double> dist(mean, stddev);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_chi_squared(const Json& args)
    {
        Expected<Json, Error> result{};

        double n = get_number(args, "n", 1.0);
        if (n <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Degrees of freedom 'n' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [n]()
                {
                    std::chi_squared_distribution<double> dist(n);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_cauchy(const Json& args)
    {
        Expected<Json, Error> result{};

        double loc = get_number(args, "location", 0.0);
        double scale = get_number(args, "scale", 1.0);
        if (scale <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'scale' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [loc, scale]()
                {
                    std::cauchy_distribution<double> dist(loc, scale);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_fisher_f(const Json& args)
    {
        Expected<Json, Error> result{};

        double m = get_number(args, "m", 1.0);
        double n = get_number(args, "n", 1.0);
        if (m <= 0.0 || n <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Degrees of freedom 'm' and 'n' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [m, n]()
                {
                    std::fisher_f_distribution<double> dist(m, n);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }

    Expected<Json, Error> tool_random_student_t(const Json& args)
    {
        Expected<Json, Error> result{};

        double n = get_number(args, "n", 1.0);
        if (n <= 0.0)
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Degrees of freedom 'n' must be greater than 0."});
        }
        else
        {
            result = handle_random(
                args,
                [n]()
                {
                    std::student_t_distribution<double> dist(n);
                    return dist(random_engine());
                }
            );
        }

        return result;
    }
}

int main(int argc, char* argv[])
{
    using namespace pooriayousefi::mcp;

    Server server{"mcp-tool-random-number-server", "1.0.0"};
    server.set_instructions("Random sampling tools for seventeen probability distributions. Every tool accepts its distribution parameters plus an optional integer 'count' (default 1, hard cap 100000) and returns a single value for count=1 or a values array with the sample count otherwise. Parameters are validated before sampling and the mt19937_64 engine is thread-local, seeded from std::random_device.");

    const Json count_prop = {
        {"type", "integer"},
        {"description", "Number of samples to generate (default 1)."}
    };

    Tool random_uniform_int{};
    random_uniform_int.name = "random_uniform_int";
    random_uniform_int.title = "Random uniform int";
    random_uniform_int.description = "Generates random integers uniformly on [min_val, max_val].";
    random_uniform_int.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"min_val", {{"type", "integer"}}},
            {"max_val", {{"type", "integer"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"min_val", "max_val"})}
    };
    random_uniform_int.handler = tool_random_uniform_int;
    server.add_tool(std::move(random_uniform_int));

    Tool random_uniform_real{};
    random_uniform_real.name = "random_uniform_real";
    random_uniform_real.title = "Random uniform real";
    random_uniform_real.description = "Generates random floats uniformly on [min_val, max_val).";
    random_uniform_real.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"min_val", {{"type", "number"}}},
            {"max_val", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"min_val", "max_val"})}
    };
    random_uniform_real.handler = tool_random_uniform_real;
    server.add_tool(std::move(random_uniform_real));

    Tool random_bernoulli{};
    random_bernoulli.name = "random_bernoulli";
    random_bernoulli.title = "Random bernoulli";
    random_bernoulli.description = "Bernoulli(p) samples as 0/1.";
    random_bernoulli.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"p", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"p"})}
    };
    random_bernoulli.handler = tool_random_bernoulli;
    server.add_tool(std::move(random_bernoulli));

    Tool random_binomial{};
    random_binomial.name = "random_binomial";
    random_binomial.title = "Random binomial";
    random_binomial.description = "Binomial(t, p) samples.";
    random_binomial.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"t", {{"type", "integer"}}},
            {"p", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"t", "p"})}
    };
    random_binomial.handler = tool_random_binomial;
    server.add_tool(std::move(random_binomial));

    Tool random_geometric{};
    random_geometric.name = "random_geometric";
    random_geometric.title = "Random geometric";
    random_geometric.description = "Geometric(p): trials before first success.";
    random_geometric.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"p", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"p"})}
    };
    random_geometric.handler = tool_random_geometric;
    server.add_tool(std::move(random_geometric));

    Tool random_negative_binomial{};
    random_negative_binomial.name = "random_negative_binomial";
    random_negative_binomial.title = "Random negative binomial";
    random_negative_binomial.description = "Negative binomial: failures before k successes.";
    random_negative_binomial.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"k", {{"type", "integer"}}},
            {"p", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"k", "p"})}
    };
    random_negative_binomial.handler = tool_random_negative_binomial;
    server.add_tool(std::move(random_negative_binomial));

    Tool random_poisson{};
    random_poisson.name = "random_poisson";
    random_poisson.title = "Random poisson";
    random_poisson.description = "Poisson(mean) samples.";
    random_poisson.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"mean", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"mean"})}
    };
    random_poisson.handler = tool_random_poisson;
    server.add_tool(std::move(random_poisson));

    Tool random_exponential{};
    random_exponential.name = "random_exponential";
    random_exponential.title = "Random exponential";
    random_exponential.description = "Exponential(lambda) samples.";
    random_exponential.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"lambda", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"lambda"})}
    };
    random_exponential.handler = tool_random_exponential;
    server.add_tool(std::move(random_exponential));

    Tool random_gamma{};
    random_gamma.name = "random_gamma";
    random_gamma.title = "Random gamma";
    random_gamma.description = "Gamma(alpha, beta) samples.";
    random_gamma.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"alpha", {{"type", "number"}}},
            {"beta", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"alpha", "beta"})}
    };
    random_gamma.handler = tool_random_gamma;
    server.add_tool(std::move(random_gamma));

    Tool random_weibull{};
    random_weibull.name = "random_weibull";
    random_weibull.title = "Random weibull";
    random_weibull.description = "Weibull(shape, scale) samples.";
    random_weibull.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"shape", {{"type", "number"}}},
            {"scale", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"shape", "scale"})}
    };
    random_weibull.handler = tool_random_weibull;
    server.add_tool(std::move(random_weibull));

    Tool random_extreme_value{};
    random_extreme_value.name = "random_extreme_value";
    random_extreme_value.title = "Random extreme value";
    random_extreme_value.description = "Extreme value (Gumbel) samples.";
    random_extreme_value.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"location", {{"type", "number"}}},
            {"scale", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"location", "scale"})}
    };
    random_extreme_value.handler = tool_random_extreme_value;
    server.add_tool(std::move(random_extreme_value));

    Tool random_normal{};
    random_normal.name = "random_normal";
    random_normal.title = "Random normal";
    random_normal.description = "Normal(mean, stddev) samples.";
    random_normal.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"mean", {{"type", "number"}}},
            {"stddev", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"mean", "stddev"})}
    };
    random_normal.handler = tool_random_normal;
    server.add_tool(std::move(random_normal));

    Tool random_lognormal{};
    random_lognormal.name = "random_lognormal";
    random_lognormal.title = "Random lognormal";
    random_lognormal.description = "Lognormal(mean, stddev) samples.";
    random_lognormal.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"mean", {{"type", "number"}}},
            {"stddev", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"mean", "stddev"})}
    };
    random_lognormal.handler = tool_random_lognormal;
    server.add_tool(std::move(random_lognormal));

    Tool random_chi_squared{};
    random_chi_squared.name = "random_chi_squared";
    random_chi_squared.title = "Random chi-squared";
    random_chi_squared.description = "Chi-squared(n) samples.";
    random_chi_squared.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"n", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"n"})}
    };
    random_chi_squared.handler = tool_random_chi_squared;
    server.add_tool(std::move(random_chi_squared));

    Tool random_cauchy{};
    random_cauchy.name = "random_cauchy";
    random_cauchy.title = "Random cauchy";
    random_cauchy.description = "Cauchy(location, scale) samples.";
    random_cauchy.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"location", {{"type", "number"}}},
            {"scale", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"location", "scale"})}
    };
    random_cauchy.handler = tool_random_cauchy;
    server.add_tool(std::move(random_cauchy));

    Tool random_fisher_f{};
    random_fisher_f.name = "random_fisher_f";
    random_fisher_f.title = "Random Fisher F";
    random_fisher_f.description = "Fisher F(m, n) samples.";
    random_fisher_f.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"m", {{"type", "number"}}},
            {"n", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"m", "n"})}
    };
    random_fisher_f.handler = tool_random_fisher_f;
    server.add_tool(std::move(random_fisher_f));

    Tool random_student_t{};
    random_student_t.name = "random_student_t";
    random_student_t.title = "Random Student-t";
    random_student_t.description = "Student-t(n) samples.";
    random_student_t.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"n", {{"type", "number"}}},
            {"count", count_prop}
        }},
        {"required", Json::array({"n"})}
    };
    random_student_t.handler = tool_random_student_t;
    server.add_tool(std::move(random_student_t));

    const int exit_code = server.run_cli(argc, argv, 8095);
    return exit_code;
}