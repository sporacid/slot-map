#include "spore/slot_map.hpp"

#include "plf/colony.hpp"

#include <chrono>
#include <format>
#include <iostream>
#include <random>
#include <ranges>
#include <vector>

namespace spore::benchmarks
{
    using duration_t = std::chrono::steady_clock::duration;

    struct bench_result
    {
        std::string_view name;
        size_t size;
        size_t parallelism;
        duration_t get;
        duration_t set;
        duration_t reset;
        duration_t iteration;

        duration_t total() const
        {
            return get + set + reset + iteration;
        }
    };

    struct bench_config
    {
        size_t parallelism = 1;
        size_t iteration = 4;
        size_t action_min = 1'000;
        size_t action_max = 30'000;
        size_t read_num = 3;
        size_t rng_seed = 0;
    };

    template <size_t size_v>
    struct bench_value
    {
        uint8_t data[size_v] {};
    };

    template <typename value_t>
    struct plf_adapter : plf::colony<value_t>
    {
        using iterator = plf::colony<value_t>::iterator;

        using plf::colony<value_t>::emplace;

        SPORE_SLOT_MAP_INLINE value_t& at(const iterator& it) const
        {
            return *it;
        }

        SPORE_SLOT_MAP_INLINE bool erase(const iterator& it)
        {
            return plf::colony<value_t>::erase(it) != plf::colony<value_t>::end();
        }
    };

    template <typename value_t>
    SPORE_SLOT_MAP_INLINE void no_optimizations(value_t& value)
    {
#if defined(__clang__)
        asm volatile("" : "+r,m"(value) : : "memory");
#elif defined(__GNUC__)
        asm volatile("" : "+m,r"(value) : : "memory");
#elif defined(_MSC_VER)
        (void) *reinterpret_cast<volatile value_t*>(&value);
        _ReadWriteBarrier();
#endif
    }

    template <typename value_t>
    SPORE_SLOT_MAP_INLINE void no_optimizations(const value_t& value)
    {
#if defined(__clang__) || defined(__GNUC__)
        asm volatile("" : : "r,m"(value) : "memory");
#elif defined(_MSC_VER)
        (void) *reinterpret_cast<const volatile value_t*>(&value);
        _ReadWriteBarrier();
#endif
    }

    template <typename key_t, typename value_t, typename map_t>
    void bench(map_t& map, const bench_config& config, std::vector<bench_result>& results, const std::string_view name)
    {
        std::atomic<bool> start;

        std::atomic<uint64_t> get;
        std::atomic<uint64_t> set;
        std::atomic<uint64_t> reset;
        std::atomic<uint64_t> iteration;

        std::vector<std::jthread> threads;
        threads.reserve(config.parallelism);

        for (size_t thread_index = 0; thread_index < config.parallelism; ++thread_index)
        {
            auto action = [&, thread_index] {
                std::vector<key_t> keys;
                keys.reserve(config.action_max);

                while (not start)
                {
                }

                std::mt19937 rng { static_cast<unsigned int>(config.rng_seed + thread_index) };
                std::uniform_int_distribution rng_distribution { config.action_min, config.action_max };

                const auto add_duration = [](std::atomic<uint64_t>& duration, auto&& action) {
                    const auto then = std::chrono::steady_clock::now();
                    action();
                    const auto now = std::chrono::steady_clock::now();
                    duration.fetch_add((now - then).count());
                };

                for (size_t iteration_index = 0; iteration_index < config.iteration; ++iteration_index)
                {
                    const size_t iteration_num = rng_distribution(rng);

                    add_duration(set, [&] {
                        for (size_t action_index = 0; action_index < iteration_num; ++action_index)
                        {
                            const key_t key = map.emplace();
                            no_optimizations(key);
                            keys.push_back(key);
                        }
                    });

                    add_duration(get, [&] {
                        for (size_t read_index = 0; read_index < config.read_num; ++read_index)
                        {
                            for (size_t action_index = 0; action_index < iteration_num; ++action_index)
                            {
                                const key_t key = keys[action_index];
                                value_t& value = map.at(key);
                                no_optimizations(value);
                            }
                        }
                    });

                    add_duration(iteration, [&] {
                        for (const value_t& value : map)
                        {
                            no_optimizations(value);
                        }
                    });

                    add_duration(reset, [&] {
                        for (size_t action_index = 0; action_index < iteration_num; ++action_index)
                        {
                            const key_t key = keys[action_index];
                            const bool erased = map.erase(key);
                            no_optimizations(erased);
                        }
                    });

                    keys.clear();
                }
            };

            threads.emplace_back(std::move(action));
        }

        using namespace std::chrono_literals;

        std::this_thread::sleep_for(200ms);

        start.store(true, std::memory_order_release);

        std::ranges::for_each(threads, &std::jthread::join);

        results.push_back({
            .name = name,
            .size = sizeof(value_t),
            .parallelism = config.parallelism,
            .get { get.load() },
            .set { set.load() },
            .reset { reset.load() },
            .iteration { iteration.load() },
        });
    }

    template <size_t size_v, size_t... sizes_v, typename func_t>
    void for_each_size(const std::integer_sequence<size_t, size_v, sizes_v...>, func_t&& func)
    {
        func.template operator()<size_v>();

        if constexpr (sizeof...(sizes_v) > 0)
        {
            for_each_size(std::integer_sequence<size_t, sizes_v...>(), std::forward<func_t>(func));
        }
    }

    void print_results(const std::span<const bench_result> results)
    {
        constexpr auto to_millis = [](const duration_t duration) {
            return std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(duration).count();
        };

        const std::string header = std::format("{:<5} | {:<5} | {:<15} | {:<15} | {:<15} | {:<15} | {:<25}", "par", "size", "get (ms)", "set (ms)", "reset (ms)", "iter (ms)", "total (ms)", "name");
        const std::string separator = std::format("{0:-<5} | {0:-<5} | {0:-<15} | {0:-<15} | {0:-<15} | {0:-<15} | {0:-<15} | {0:-<25}", "");

        size_t current_size = 0;

        std::cout << header << std::endl;

        for (const bench_result& result : results)
        {
            std::string result_string = std::format("{:>5} | {:>5} | {:>15.4f} | {:>15.4f} | {:>15.4f} | {:>15.4f} | {:>15.4f} | {:<25}",
                result.parallelism, result.size, to_millis(result.get), to_millis(result.set), to_millis(result.reset), to_millis(result.iteration), to_millis(result.total()), result.name);

            if (current_size != result.size)
            {
                current_size = result.size;

                std::cout << separator << std::endl;
            }

            std::cout << result_string << std::endl;
        }

        std::cout << separator << std::endl;
        std::cout << std::endl;
    }
}

int main()
{
    using namespace spore;
    using namespace spore::benchmarks;

    constexpr auto size_sequence = std::integer_sequence<size_t, 1, 8, 32, 128, 512>();
    constexpr auto sort_results = [](std::vector<bench_result>& results) {
        std::ranges::sort(results, std::ranges::less {},
            [](const bench_result& result) { return std::tuple(result.size, result.parallelism, result.get.count()); });
    };

    // Single thread
    {
        constexpr bench_config configs[] {
            bench_config {
                .parallelism = 1,
                .iteration = 10,
                .action_min = 10'000,
                .action_max = 50'000,
                .read_num = 25,
            },
            bench_config {
                .parallelism = 1,
                .iteration = 10,
                .action_min = 100'000,
                .action_max = 500'000,
                .read_num = 10,
            },
        };

        constexpr size_t capacity = std::ranges::max(configs, std::ranges::less {}, &bench_config::action_max).action_max;

        std::vector<bench_result> results;
        results.reserve(128);

        // slot map static
        for_each_size(size_sequence, [&]<size_t size_v> {
            std::ranges::for_each(configs, [&](const bench_config& config) {
                static_slot_map_st<slot_key, bench_value<size_v>, capacity> map;
                bench<slot_key, bench_value<size_v>>(map, config, results, "slot map (static st)");
            });
        });

        // slot map dynamic
        for_each_size(size_sequence, [&]<size_t size_v> {
            std::ranges::for_each(configs, [&](const bench_config& config) {
                slot_map_st<slot_key, bench_value<size_v>, capacity> map;
                bench<slot_key, bench_value<size_v>>(map, config, results, "slot map (dynamic st)");
            });
        });

        // plf colony
        for_each_size(size_sequence, [&]<size_t size_v> {
            std::ranges::for_each(configs, [&](const bench_config& config) {
                using colony = plf_adapter<bench_value<size_v>>;
                colony map;
                bench<typename colony::iterator, bench_value<size_v>>(map, config, results, "plf colony");
            });
        });

        sort_results(results);
        print_results(results);
    }

    // Multi thread
    {
        constexpr bench_config configs[] {
            bench_config {
                .parallelism = 2,
                .iteration = 5,
                .action_min = 50'000,
                .action_max = 250'000,
                .read_num = 5,
            },
            bench_config {
                .parallelism = 4,
                .iteration = 5,
                .action_min = 25'000,
                .action_max = 125'000,
                .read_num = 5,
            },
            bench_config {
                .parallelism = 8,
                .iteration = 5,
                .action_min = 10'000,
                .action_max = 75'000,
                .read_num = 5,
            },
            bench_config {
                .parallelism = 16,
                .iteration = 3,
                .action_min = 5'000,
                .action_max = 30'000,
                .read_num = 5,
            },
        };

        constexpr auto projection = [](const bench_config& config) { return config.parallelism * config.action_max; };
        constexpr size_t capacity = std::ranges::max(configs | std::views::transform(projection), std::ranges::less {});

        std::vector<bench_result> results;
        results.reserve(128);

        // slot map static
        for_each_size(size_sequence, [&]<size_t size_v> {
            std::ranges::for_each(configs, [&](const bench_config& config) {
                static_slot_map_mt<slot_key, bench_value<size_v>, capacity> map;
                bench<slot_key, bench_value<size_v>>(map, config, results, "slot map (static mt)");
            });
        });

        // slot map dynamic
        for_each_size(size_sequence, [&]<size_t size_v> {
            std::ranges::for_each(configs, [&](const bench_config& config) {
                slot_map_mt<slot_key, bench_value<size_v>, capacity> map;
                bench<slot_key, bench_value<size_v>>(map, config, results, "slot map (dynamic mt)");
            });
        });

        // plf colony
        // for_each_size(size_sequence, [&]<size_t size_v> {
        //     std::ranges::for_each(configs, [&](const bench_config& config) {
        //         using colony = plf_adapter<bench_value<size_v>>;
        //         colony map;
        //         bench<typename colony::iterator, bench_value<size_v>>(map, config, results, "plf colony");
        //     });
        // });

        sort_results(results);
        print_results(results);
    }

    std::cin.get();

    return 0;
}