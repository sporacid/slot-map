#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

#ifndef SPORE_SLOT_MAP_ASSERT
#    include <cassert>
#    define SPORE_SLOT_MAP_ASSERT(Expr) assert(Expr)
#endif

#ifndef SPORE_SLOT_MAP_ASSERT_NOEXCEPT
#    define SPORE_SLOT_MAP_ASSERT_NOEXCEPT (true)
#endif

#ifndef SPORE_SLOT_MAP_MIN_PAGE_SIZE
#    define SPORE_SLOT_MAP_MIN_PAGE_SIZE 0xffff
#endif

#ifndef SPORE_SLOT_MAP_MIN_SLOT_NUM
#    define SPORE_SLOT_MAP_MIN_SLOT_NUM 32
#endif

#ifndef SPORE_SLOT_MAP_MAX_ROOT_WORD_NUM
#    define SPORE_SLOT_MAP_MAX_ROOT_WORD_NUM 8
#endif

namespace spore
{
    namespace detail
    {
        template <typename>
        struct is_bit_container : std::false_type
        {
        };

        template <std::unsigned_integral value_t>
        struct is_bit_container<value_t> : std::true_type
        {
        };

        template <std::unsigned_integral value_t>
        struct is_bit_container<std::atomic<value_t>> : std::true_type
        {
        };

        template <typename value_t>
        concept bit_container = is_bit_container<value_t>::value;

        template <bit_container value_t>
        consteval size_t bit_count()
        {
            constexpr size_t bits_per_byte = 8;
            return sizeof(value_t) * bits_per_byte;
        }

        template <bit_container value_t>
        consteval size_t reduce_word_count_once(const size_t size)
        {
            return (size + bit_count<value_t>() - 1) / bit_count<value_t>();
        }

        template <bit_container value_t>
        consteval size_t reduce_word_count_n(const size_t size, const size_t n)
        {
            size_t new_size = size;

            for (size_t index = 0; index < n; ++index)
            {
                new_size = reduce_word_count_once<value_t>(new_size);
            }

            return new_size;
        }

        template <typename value_t, size_t size_v, size_t root_size_v = SPORE_SLOT_MAP_MAX_ROOT_WORD_NUM>
        consteval size_t optimal_depth()
        {
            size_t depth = 0;
            size_t size = size_v;

            while (size > root_size_v)
            {
                size = reduce_word_count_once<value_t>(size);
                ++depth;
            }

            return depth;
        }

        template <bit_container value_t, size_t size_v, size_t depth_v>
        struct hierarchical_bits
        {
            static constexpr size_t num_words = reduce_word_count_once<value_t>(size_v);
            hierarchical_bits<value_t, num_words, depth_v - 1> parent;
            value_t words[num_words] {};
        };

        template <bit_container value_t, size_t size_v>
        struct hierarchical_bits<value_t, size_v, 0>
        {
            static constexpr size_t num_words = reduce_word_count_once<value_t>(size_v);
            value_t words[num_words] {};
        };

        template <bit_container value_t>
        struct hierarchical_bits_traits
        {
            using value_type = value_t;
            using word_type = value_t;

            template <size_t target_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr auto& get_words(hierarchical_bits<value_t, size_v, depth_v>& bits) noexcept
            {
                if constexpr (target_depth_v == depth_v)
                {
                    return bits.words;
                }
                else
                {
                    static_assert(target_depth_v < depth_v);
                    return get_words<target_depth_v>(bits.parent);
                }
            }

            template <size_t target_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr const auto& get_words(const hierarchical_bits<value_t, size_v, depth_v>& bits) noexcept
            {
                if constexpr (target_depth_v == depth_v)
                {
                    return bits.words;
                }
                else
                {
                    static_assert(target_depth_v < depth_v);
                    return get_words<target_depth_v>(bits.parent);
                }
            }

            template <size_t target_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr value_t get_word(const hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t index) noexcept
            {
                const auto& word = get_words<target_depth_v>(bits);
                return word[index];
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr std::optional<size_t> pop_unset(hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t index_start, const size_t index_end) noexcept
            {
                auto& words = get_words<current_depth_v>(bits);

                for (size_t index = index_start; index < index_end; ++index)
                {
                    const word_type word = words[index];

                    const size_t bit_index = static_cast<size_t>(std::countr_one(word));

                    if (bit_index == bit_count<word_type>())
                    {
                        continue;
                    }

                    const size_t child_index = index * bit_count<word_type>() + bit_index;

                    if constexpr (current_depth_v == depth_v)
                    {
                        const word_type new_word = word | (static_cast<word_type>(1) << bit_index);

                        words[index] = new_word;

                        pop_unset_propagate<depth_v>(bits, index, new_word);

                        return child_index;
                    }
                    else
                    {
                        constexpr size_t child_size = reduce_word_count_n<word_type>(size_v, depth_v - current_depth_v - 1);

                        if (child_index < child_size)
                        {
                            if (const std::optional<size_t> result = pop_unset<current_depth_v + 1>(bits, child_index, child_index + 1))
                            {
                                return result;
                            }

                            words[index] = word | (static_cast<word_type>(1) << bit_index);
                        }
                    }
                }

                return std::nullopt;
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            static constexpr void pop_unset_propagate(hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t word_index, const word_type word_value) noexcept
            {
                if (word_value != std::numeric_limits<word_type>::max())
                {
                    return;
                }

                if constexpr (current_depth_v > 0)
                {
                    const size_t parent_word_index = word_index / bit_count<word_type>();
                    const size_t parent_bit_index = word_index % bit_count<word_type>();

                    auto& parent_words = get_words<current_depth_v - 1>(bits);

                    parent_words[parent_word_index] = parent_words[parent_word_index] | (static_cast<word_type>(1) << parent_bit_index);

                    pop_unset_propagate<current_depth_v - 1>(bits, parent_word_index, parent_words[parent_word_index]);
                }
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            static constexpr void reset(hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t index) noexcept
            {
                auto& words = get_words<current_depth_v>(bits);

                const size_t word_index = index / bit_count<word_type>();
                const size_t bit_index = index % bit_count<word_type>();

                const word_type bit_mask = ~(static_cast<word_type>(1) << bit_index);

                words[word_index] = words[word_index] & bit_mask;

                if constexpr (current_depth_v > 0)
                {
                    reset<current_depth_v - 1>(bits, word_index);
                }
            }

            template <size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr bool is_set(const hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t index) noexcept
            {
                const size_t word_index = index / bit_count<word_type>();
                const size_t bit_index = index % bit_count<word_type>();

                const auto& words = get_words<depth_v>(bits);

                const word_type word = words[word_index];

                return (word >> bit_index) & static_cast<word_type>(1);
            }
        };

        template <bit_container value_t>
        struct hierarchical_bits_traits<std::atomic<value_t>>
        {
            using value_type = std::atomic<value_t>;
            using word_type = value_t;

            template <size_t target_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr auto& get_words(hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits) noexcept
            {
                if constexpr (target_depth_v == depth_v)
                {
                    return bits.words;
                }
                else
                {
                    static_assert(target_depth_v < depth_v);
                    return get_words<target_depth_v>(bits.parent);
                }
            }

            template <size_t target_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr const auto& get_words(const hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits) noexcept
            {
                if constexpr (target_depth_v == depth_v)
                {
                    return bits.words;
                }
                else
                {
                    static_assert(target_depth_v < depth_v);
                    return get_words<target_depth_v>(bits.parent);
                }
            }

            template <size_t target_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr value_t get_word(const hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits, const size_t index) noexcept
            {
                const auto& word = get_words<target_depth_v>(bits);
                return word[index].load(std::memory_order_acquire);
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr std::optional<size_t> pop_unset(hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits, const size_t index_start, const size_t index_end) noexcept
            {
                auto& words = get_words<current_depth_v>(bits);

                for (size_t index = index_start; index < index_end; ++index)
                {
                    word_type word = words[index].load(std::memory_order_relaxed);

                    while (true)
                    {
                        const size_t bit_index = static_cast<size_t>(std::countr_one(word));

                        if (bit_index == bit_count<word_type>())
                        {
                            break;
                        }

                        const size_t child_index = index * bit_count<word_type>() + bit_index;

                        if constexpr (current_depth_v == depth_v)
                        {
                            const word_type new_word = word | (static_cast<word_type>(1) << bit_index);

                            if (words[index].compare_exchange_strong(word, new_word, std::memory_order_release, std::memory_order_relaxed))
                            {
                                pop_unset_propagate<depth_v>(bits, index, new_word);

                                return child_index;
                            }
                        }
                        else
                        {
                            constexpr size_t child_size = reduce_word_count_n<word_type>(size_v, depth_v - current_depth_v - 1);

                            if (child_index < child_size)
                            {
                                if (const std::optional<size_t> result = pop_unset<current_depth_v + 1>(bits, child_index, child_index + 1))
                                {
                                    return result;
                                }

                                const word_type new_word = word | (static_cast<word_type>(1) << bit_index);

                                words[index].compare_exchange_strong(word, new_word, std::memory_order_relaxed, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                return std::nullopt;
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            static constexpr void pop_unset_propagate(hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits, const size_t word_index, const word_type word_value) noexcept
            {
                if (word_value != std::numeric_limits<word_type>::max())
                {
                    return;
                }

                if constexpr (current_depth_v > 0)
                {
                    const size_t parent_word_index = word_index / bit_count<word_type>();
                    const size_t parent_bit_index = word_index % bit_count<word_type>();

                    auto& parent_words = get_words<current_depth_v - 1>(bits);

                    word_type parent_word = parent_words[parent_word_index].load(std::memory_order_relaxed);
                    const word_type new_parent_word = parent_word | (static_cast<word_type>(1) << parent_bit_index);

                    std::ignore = parent_words[parent_word_index].compare_exchange_strong(parent_word, new_parent_word, std::memory_order_relaxed, std::memory_order_relaxed);

                    pop_unset_propagate<current_depth_v - 1>(bits, parent_word_index, new_parent_word);
                }
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            static constexpr void reset(hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits, const size_t index) noexcept
            {
                auto& words = get_words<current_depth_v>(bits);

                const size_t word_index = index / bit_count<word_type>();
                const size_t bit_index = index % bit_count<word_type>();

                const word_type bit_mask = ~(static_cast<word_type>(1) << bit_index);

                constexpr std::memory_order memory_order = current_depth_v == depth_v ? std::memory_order_release : std::memory_order_relaxed;

                std::ignore = words[word_index].fetch_and(bit_mask, memory_order);

                if constexpr (current_depth_v > 0)
                {
                    reset<current_depth_v - 1>(bits, word_index);
                }
            }

            template <size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr bool is_set(const hierarchical_bits<std::atomic<value_t>, size_v, depth_v>& bits, const size_t index) noexcept
            {
                const size_t word_index = index / bit_count<word_type>();
                const size_t bit_index = index % bit_count<word_type>();

                const auto& words = get_words<depth_v>(bits);

                const word_type word = words[word_index].load(std::memory_order_acquire);

                return (word >> bit_index) & static_cast<word_type>(1);
            }
        };

        template <bit_container value_t, size_t size_v, size_t max_depth_v>
        struct hierarchical_bitset
        {
            using traits_type = hierarchical_bits_traits<value_t>;
            using word_type = traits_type::word_type;

            hierarchical_bits<value_t, size_v, max_depth_v - 1> bits;

            template <bool set_v>
            struct iterator_impl
            {
                constexpr explicit iterator_impl(const hierarchical_bitset& self) noexcept
                    : _self(&self)
                {
                    load_word();

                    if (_current_word == 0)
                    {
                        next_word();
                    }
                }

                constexpr explicit iterator_impl(const hierarchical_bitset& self, std::nullptr_t) noexcept
                    : _self(&self),
                      _word_index(reduce_word_count_once<value_t>(size_v))
                {
                }

                constexpr size_t operator*() const noexcept
                {
                    return _word_index * bit_count<value_t>() + static_cast<size_t>(std::countr_zero(_current_word));
                }

                constexpr iterator_impl& operator++() noexcept
                {
                    _current_word &= _current_word - 1;

                    if (_current_word == 0)
                    {
                        next_word();
                    }

                    return *this;
                }

                constexpr iterator_impl operator++(int) noexcept
                {
                    iterator_impl copy = *this;
                    operator++();
                    return copy;
                }

                constexpr bool operator==(const iterator_impl& other) const noexcept
                {
                    return std::tie(_word_index, _current_word) == std::tie(other._word_index, other._current_word);
                }

                constexpr bool operator!=(const iterator_impl& other) const noexcept
                {
                    return std::tie(_word_index, _current_word) != std::tie(other._word_index, other._current_word);
                }

              private:
                const hierarchical_bitset* _self = nullptr;
                size_t _word_index = 0;
                word_type _current_word = 0;

                constexpr void load_word() noexcept
                {
                    const word_type word = traits_type::get_word<max_depth_v - 1>(_self->bits, _word_index);

                    if constexpr (set_v)
                    {
                        _current_word = word;
                    }
                    else
                    {
                        _current_word = ~word;
                    }
                }

                constexpr void next_word() noexcept
                {
                    while (++_word_index < reduce_word_count_once<value_t>(size_v))
                    {
                        load_word();

                        if (_current_word != 0)
                        {
                            break;
                        }
                    }
                }
            };

            template <bool set_v>
            struct view_impl
            {
                using iterator = iterator_impl<set_v>;

                const hierarchical_bitset& self;

                [[nodiscard]] constexpr iterator begin() const noexcept
                {
                    return iterator { self };
                }

                [[nodiscard]] constexpr iterator end() const noexcept
                {
                    return iterator { self, nullptr };
                }
            };

            using set_view = view_impl<true>;
            using unset_view = view_impl<false>;

            [[nodiscard]] constexpr set_view as_view() const noexcept
            {
                return set_view { *this };
            }

            [[nodiscard]] constexpr unset_view as_unset_view() const noexcept
            {
                return unset_view { *this };
            }

            [[nodiscard]] constexpr bool is_set(const size_t index) const noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
            {
                SPORE_SLOT_MAP_ASSERT(index < size_v);
                return traits_type::is_set(bits, index);
            }

            [[nodiscard]] constexpr bool is_unset(const size_t index) const noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
            {
                SPORE_SLOT_MAP_ASSERT(index < size_v);
                return not traits_type::is_set(bits, index);
            }

            constexpr void reset(const size_t index) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
            {
                SPORE_SLOT_MAP_ASSERT(index < size_v);
                traits_type::template reset<max_depth_v - 1>(bits, index);
            }

            [[nodiscard]] constexpr std::optional<size_t> pop_unset() noexcept
            {
                constexpr size_t root_size = reduce_word_count_n<word_type>(size_v, max_depth_v - 1);
                return traits_type::template pop_unset<0>(bits, 0, root_size);
            }
        };

        struct empty_mutex
        {
            static constexpr void lock() noexcept {}
            static constexpr void unlock() noexcept {}
        };
    }

    struct slot_key
    {
        uint32_t index = 0;
        uint32_t version = 0;

        constexpr auto operator<=>(const slot_key& other) const = default;
    };

    template <typename key_t>
    struct slot_key_traits
    {
        using index_type = decltype(std::declval<key_t>().index);
        using version_type = decltype(std::declval<key_t>().version);

        static constexpr key_t make_key(const index_type index, const version_type version)
        {
            return key_t { index, version };
        }

        static constexpr index_type index(const key_t& key)
        {
            return key.index;
        }

        static constexpr version_type version(const key_t& key)
        {
            return key.version;
        }
    };

    template <typename value_t>
    struct slot_value
    {
        alignas(value_t) uint8_t bytes[sizeof(value_t)];

        template <typename... args_t>
        void construct(args_t&&... args)
        {
            value_t* ptr = reinterpret_cast<value_t*>(std::addressof(bytes));
            std::construct_at(ptr, std::forward<args_t>(args)...);
        }

        void destroy()
        {
            value_t* ptr = reinterpret_cast<value_t*>(std::addressof(bytes));
            std::destroy_at(ptr);
        }

        value_t& get()
        {
            value_t* ptr = reinterpret_cast<value_t*>(std::addressof(bytes));
            return *ptr;
        }
    };

    template <typename value_t, typename version_t, size_t size_v>
    struct slot_storage_static
    {
        static_assert(size_v > 0);

        constexpr std::tuple<slot_value<value_t>&, version_t&> at(const size_t index) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            SPORE_SLOT_MAP_ASSERT(index < size_v);

            return std::tie(_slots[index], _versions[index]);
        }

        constexpr std::tuple<slot_value<value_t>*, version_t*> try_at(const size_t index) noexcept
        {
            if (index < size_v) [[likely]]
            {
                return std::tuple { &_slots[index], &_versions[index] };
            }

            return std::tuple { nullptr, nullptr };
        }

        static consteval size_t size()
        {
            return size_v;
        }

      private:
        slot_value<value_t> _slots[size_v] {};
        version_t _versions[size_v] {};
    };

    template <typename value_t, typename version_t, size_t size_v, bool concurrent_v>
    struct slot_storage_dynamic
    {
        static_assert(size_v > 0);

        static constexpr size_t num_slot = std::clamp<size_t>(SPORE_SLOT_MAP_MIN_PAGE_SIZE / sizeof(value_t), SPORE_SLOT_MAP_MIN_SLOT_NUM, size_v);
        static constexpr size_t num_block = size_v / num_slot;

        constexpr std::tuple<slot_value<value_t>&, version_t&> at(const size_t index) noexcept
        {
            const size_t block_index = index / num_slot;
            const size_t slot_index = index % num_slot;

            block& block = get_block(block_index);

            return std::tie(block.slots[slot_index], block.versions[slot_index]);
        }

        constexpr std::tuple<slot_value<value_t>*, version_t*> try_at(const size_t index) const noexcept
        {
            const size_t block_index = index / num_slot;
            const size_t slot_index = index % num_slot;

            if (block* block = try_get_block(block_index))
            {
                return std::tuple { &block->slots[slot_index], &block->versions[slot_index] };
            }

            return std::tuple { nullptr, nullptr };
        }

        static consteval size_t size()
        {
            return size_v;
        }

      private:
        using mutex_t = std::conditional_t<concurrent_v, std::mutex, detail::empty_mutex>;

        struct block
        {
            slot_value<value_t> slots[num_slot] {};
            version_t versions[num_slot] {};
        };

        mutable mutex_t _mutex;
        std::unique_ptr<block> _blocks[num_block] {};

        [[nodiscard]] constexpr block& get_block(const size_t index) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            SPORE_SLOT_MAP_ASSERT(index < num_block);

            if (_blocks[index] == nullptr) [[unlikely]]
            {
                if constexpr (concurrent_v)
                {
                    std::unique_lock lock { _mutex };

                    if (_blocks[index] == nullptr)
                    {
                        _blocks[index] = std::make_unique<block>();
                    }
                }
                else
                {
                    _blocks[index] = std::make_unique<block>();
                }
            }

            return *_blocks[index];
        }

        [[nodiscard]] constexpr block* try_get_block(const size_t index) const noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            SPORE_SLOT_MAP_ASSERT(index < num_block);

            if (_blocks[index] == nullptr) [[unlikely]]
            {
                if constexpr (concurrent_v)
                {
                    std::unique_lock lock { _mutex };

                    if (_blocks[index] == nullptr)
                    {
                        return nullptr;
                    }
                }
                else
                {
                    return nullptr;
                }
            }

            return _blocks[index].get();
        }
    };

    template <typename key_t, typename value_t, typename key_traits_t, typename storage_t, typename bitset_t, bool concurrent_v>
    struct basic_slot_map
    {
        using storage_type = storage_t;
        using index_type = key_traits_t::index_type;
        using version_type = key_traits_t::version_type;

        template <bool const_v>
        struct iterator_impl
        {
            using this_type = std::conditional_t<const_v, const basic_slot_map, basic_slot_map>;
            using value_type = std::conditional_t<const_v, const value_t, value_t>;
            using bit_iterator = bitset_t::set_view::iterator;

            constexpr iterator_impl(this_type& self, const bit_iterator bit_it) noexcept
                : _self(&self),
                  _bit_it(bit_it)
            {
            }

            constexpr bool operator==(const iterator_impl& other) const noexcept
            {
                return _bit_it == other._bit_it;
            }

            constexpr bool operator!=(const iterator_impl& other) const noexcept
            {
                return _bit_it != other._bit_it;
            }

            constexpr value_type& operator*() const noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
            {
                SPORE_SLOT_MAP_ASSERT(_self != nullptr);

                const size_t index = *_bit_it;

                if constexpr (concurrent_v)
                {
                    std::atomic_thread_fence(std::memory_order_acquire);
                }

                value_t& slot = _self->data->slots[index];

                return slot;
            }

            constexpr value_type* operator->() const noexcept
            {
                return &operator*();
            }

            constexpr iterator_impl& operator++() noexcept
            {
                _bit_it.operator++();
                return *this;
            }

            constexpr iterator_impl& operator++(int) noexcept
            {
                _bit_it.operator++(0);
                return *this;
            }

            constexpr iterator_impl& operator+=(const size_t num) noexcept
            {
                _bit_it.operator++(num);
                return *this;
            }

            constexpr iterator_impl operator+(const size_t num) const noexcept
            {
                iterator_impl copy = *this;

                for (size_t index = 0; index < num; ++index)
                {
                    copy.operator++();
                }

                return copy;
            }

          private:
            this_type* _self = nullptr;
            bit_iterator _bit_it;
        };

        using key_type = key_t;
        using mapped_type = value_t;
        using iterator = iterator_impl<false>;
        using const_iterator = iterator_impl<true>;

        constexpr basic_slot_map()
            : _data(std::make_unique<data>())
        {
        }

        constexpr ~basic_slot_map() noexcept(std::is_nothrow_destructible_v<value_t>)
        {
            for (size_t index = 0; index < storage_t::size(); ++index)
            {
                if (_data->bitset.is_set(index))
                {
                    auto [slot, _] = _data->storage.at(index);
                    slot.destroy();
                }
            }
        }

        [[nodiscard]] constexpr iterator begin() noexcept
        {
            return iterator { *this };
        }

        [[nodiscard]] constexpr iterator end() noexcept
        {
            return iterator { *this, nullptr };
        }

        [[nodiscard]] constexpr const_iterator begin() const noexcept
        {
            return const_iterator { *this };
        }

        [[nodiscard]] constexpr const_iterator end() const noexcept
        {
            return const_iterator { *this, nullptr };
        }

        template <typename... args_t>
        [[nodiscard]] constexpr std::optional<key_t> try_emplace(args_t&&... args) noexcept(std::is_nothrow_constructible_v<value_t, args_t&&...>)
        {
            const std::optional<size_t> maybe_index = _data->bitset.pop_unset();

            if (not maybe_index.has_value()) [[unlikely]]
            {
                return std::nullopt;
            }

            const index_type index = maybe_index.value();

            auto [slot, version] = _data->storage.at(index);

            slot.construct(std::forward<args_t>(args)...);

            if constexpr (concurrent_v)
            {
                std::atomic_thread_fence(std::memory_order_release);
            }

            return key_traits_t::make_key(index, version);
        }

        template <typename... args_t>
        [[nodiscard]] constexpr key_t emplace(args_t&&... args) noexcept(std::is_nothrow_constructible_v<value_t, args_t&&...> and SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            const std::optional<key_t> key = try_emplace(std::forward<args_t>(args)...);
            SPORE_SLOT_MAP_ASSERT(key.has_value());
            return key.value();
        }

        [[nodiscard]] constexpr value_t* try_at(const key_t& key) noexcept
        {
            const index_type index = key_traits_t::index(key);

            if (_data->bitset.is_unset(index)) [[unlikely]]
            {
                return nullptr;
            }

            auto [slot, slot_version] = _data->storage.try_at(index);

            if (slot == nullptr)
            {
                return nullptr;
            }

            const version_type version = key_traits_t::version(key);

            if constexpr (concurrent_v)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
            }

            if (version != *slot_version) [[unlikely]]
            {
                return nullptr;
            }

            return &slot->get();
        }

        [[nodiscard]] constexpr const value_t* try_at(const key_t& key) const noexcept
        {
            return const_cast<basic_slot_map&>(*this).try_at(key);
        }

        [[nodiscard]] constexpr value_t& at(const key_t& key) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            const index_type index = key_traits_t::index(key);

            SPORE_SLOT_MAP_ASSERT(_data->bitset.is_set(index));

            auto [slot, slot_version] = _data->storage.at(index);

            if constexpr (concurrent_v)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
            }

            SPORE_SLOT_MAP_ASSERT(key_traits_t::version(key) == slot_version);

            return slot.get();
        }

        [[nodiscard]] constexpr const value_t& at(const key_t& key) const noexcept
        {
            return const_cast<basic_slot_map&>(*this).at(key);
        }

        constexpr bool erase(const key_t& key) noexcept(std::is_nothrow_destructible_v<value_t>)
        {
            const index_type index = key_traits_t::index(key);

            if (_data->bitset.is_unset(index))
            {
                return false;
            }

            auto [slot, slot_version] = _data->storage.at(index);

            const version_type version = key_traits_t::version(key);

            if constexpr (concurrent_v)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
            }

            if (version != slot_version) [[unlikely]]
            {
                return false;
            }

            slot.destroy();

            ++slot_version;

            if constexpr (concurrent_v)
            {
                std::atomic_thread_fence(std::memory_order_release);
            }

            _data->bitset.reset(index);

            return true;
        }

        constexpr value_t& operator[](const key_t& key) noexcept
        {
            return at(key);
        }

        constexpr const value_t& operator[](const key_t& key) const noexcept
        {
            return at(key);
        }

      private:
        struct data
        {
            bitset_t bitset;
            storage_t storage;
        };

        std::unique_ptr<data> _data;
    };

    template <typename key_t, typename value_t, size_t size_v>
    using slot_map_st = basic_slot_map<
        key_t,
        value_t,
        slot_key_traits<key_t>,
        slot_storage_dynamic<value_t, typename slot_key_traits<key_t>::version_type, size_v, false>,
        detail::hierarchical_bitset<size_t, size_v, detail::optimal_depth<size_t, size_v>()>,
        false>;

    template <typename key_t, typename value_t, size_t size_v>
    using slot_map_mt = basic_slot_map<
        key_t,
        value_t,
        slot_key_traits<key_t>,
        slot_storage_dynamic<value_t, typename slot_key_traits<key_t>::version_type, size_v, true>,
        detail::hierarchical_bitset<std::atomic<size_t>, size_v, detail::optimal_depth<size_t, size_v>()>,
        true>;

    template <typename key_t, typename value_t, size_t size_v>
    using static_slot_map_st = basic_slot_map<
        key_t,
        value_t,
        slot_key_traits<key_t>,
        slot_storage_static<value_t, typename slot_key_traits<key_t>::version_type, size_v>,
        detail::hierarchical_bitset<size_t, size_v, detail::optimal_depth<size_t, size_v>()>,
        false>;

    template <typename key_t, typename value_t, size_t size_v>
    using static_slot_map_mt = basic_slot_map<
        key_t,
        value_t,
        slot_key_traits<key_t>,
        slot_storage_static<value_t, typename slot_key_traits<key_t>::version_type, size_v>,
        detail::hierarchical_bitset<std::atomic<size_t>, size_v, detail::optimal_depth<size_t, size_v>()>,
        true>;
}