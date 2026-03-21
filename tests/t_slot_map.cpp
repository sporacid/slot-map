#include "spore/slot_map.hpp"

#include "catch2/catch_all.hpp"

#include <random>
#include <thread>

constexpr size_t capacity = 1024;

struct slot_value
{
    size_t value {};
};

TEMPLATE_TEST_CASE("spore::slot-map", "[spore::slot-map]", (spore::slot_map_st<spore::slot_key, slot_value, capacity>), (spore::slot_map_mt<spore::slot_key, slot_value, capacity>) )
{
    using namespace spore;

    using slot_map_t = TestType;

    SECTION("It should emplace correctly")
    {
        slot_map_t map;

        const std::optional<slot_key> key = map.try_emplace();

        REQUIRE(key.has_value());
    }

    SECTION("It should find correctly")
    {
        slot_map_t map;

        constexpr size_t expected = 42;

        const slot_key key = map.emplace(expected);
        const slot_value* value = map.find(key);

        REQUIRE(value != nullptr);
        REQUIRE(value->value == expected);
    }

    SECTION("It should erase correctly")
    {
        slot_map_t map;

        const slot_key key = map.emplace();
        const bool erased = map.erase(key);

        REQUIRE(erased);
        REQUIRE(map.find(key) == nullptr);
    }

    SECTION("It should recycle slots correctly")
    {
        slot_map_t map;

        std::ignore = map.emplace();

        const slot_key key = map.emplace();

        std::ignore = map.emplace();

        map.erase(key);

        const slot_key other_key = map.emplace();

        REQUIRE(key.index == other_key.index);
        REQUIRE(key.version != other_key.version);
    }

    SECTION("It should hold at to hold at least capacity slots")
    {
        slot_map_t map;

        for (size_t index = 0; index < capacity; ++index)
        {
            const std::optional<slot_key> key = map.try_emplace();

            REQUIRE(key.has_value());
        }
    }
}

TEST_CASE("spore::slot-map::concurrency", "[spore::slot-map]")
{
    using namespace spore;

    using slot_map_t = slot_map_mt<slot_key, slot_value, 1'048'576>;

    SECTION("It should stay consistent across multiple threads")
    {
        constexpr size_t parallelism = 8;
        constexpr size_t iteration = 4;
        constexpr size_t action_min = 1'000;
        constexpr size_t action_max = 30'000;
        constexpr size_t read_num = 3;

        std::atomic<bool> start;
        std::unique_ptr<slot_map_t> map = std::make_unique<slot_map_t>();

        std::vector<std::jthread> threads;
        threads.reserve(parallelism);

        std::mt19937 rng { 0 };
        std::uniform_int_distribution rng_distribution { action_min, action_max };

        std::vector<size_t> iteration_nums;
        iteration_nums.reserve(parallelism);

        for (size_t index = 0; index < parallelism; ++index)
        {
            iteration_nums.emplace_back(rng_distribution(rng));
        }

        for (size_t thread_index = 0; thread_index < parallelism; ++thread_index)
        {
            auto action = [&, thread_index] {
                std::vector<slot_key> keys;
                keys.reserve(std::ranges::max(iteration_nums));

                while (not start)
                {
                }

                for (size_t iteration_index = 0; iteration_index < iteration; ++iteration_index)
                {
                    const size_t iteration_num = iteration_nums[(thread_index + iteration_index) % iteration_nums.size()];
                    const auto expected_at = [&](const size_t index) { return thread_index * iteration_num + index; };

                    for (size_t index = 0; index < iteration_num; ++index)
                    {
                        const size_t expected = expected_at(index);
                        const slot_key key = map->emplace(expected);
                        keys.push_back(key);
                    }

                    for (size_t index_read = 0; index_read < read_num; ++index_read)
                    {
                        for (size_t index = 0; index < iteration_num; ++index)
                        {
                            const size_t expected = expected_at(index);
                            const slot_key key = keys[index];
                            const auto [value] = map->at(key);
                            REQUIRE(value == expected);
                        }
                    }

                    for (size_t index = 0; index < iteration_num; ++index)
                    {
                        const slot_key key = keys[index];
                        const bool erased = map->erase(key);
                        REQUIRE(erased);
                    }

                    keys.clear();
                }
            };

            threads.emplace_back(std::move(action));
        }

        using namespace std::chrono_literals;

        std::this_thread::sleep_for(200ms);

        start.store(true, std::memory_order_release);

        std::ranges::for_each(threads, &std::jthread::join);
    }
}