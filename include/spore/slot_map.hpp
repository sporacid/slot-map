#pragma once

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
#    define SPORE_SLOT_MAP_MAX_ROOT_WORD_NUM 1
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
            return 8 * sizeof(value_t);
        }

        template <bit_container value_t, size_t size_v>
        consteval size_t word_count()
        {
            return (size_v + bit_count<value_t>() - 1) / bit_count<value_t>();
        }

        template <bit_container value_t, size_t size_v, size_t depth_v>
        struct hierarchical_bits
        {
            static constexpr size_t parent_size = word_count<value_t, size_v>();
            static constexpr size_t size = size_v;

            hierarchical_bits<value_t, parent_size, depth_v - 1> parent;
            value_t words[size] {};
        };

        template <bit_container value_t, size_t size_v>
        struct hierarchical_bits<value_t, size_v, 0>
        {
            static constexpr size_t size = size_v;
            value_t words[size] {};
        };

        template <bit_container value_t>
        struct word_accessor
        {
            using word_type = value_t;

            [[nodiscard]] static constexpr value_t load(const value_t word, const std::memory_order) noexcept
            {
                return word;
            }

            static constexpr void fetch_and(value_t& word, const value_t mask, const std::memory_order) noexcept
            {
                word &= mask;
            }

            static constexpr void fetch_or(value_t& word, const value_t mask, const std::memory_order) noexcept
            {
                word |= mask;
            }

            static constexpr bool compare_exchange(value_t& word, value_t&, const value_t desired, const std::memory_order, const std::memory_order) noexcept
            {
                word = desired;
                return true;
            }
        };

        template <bit_container value_t>
        struct word_accessor<std::atomic<value_t>>
        {
            using word_type = value_t;

            [[nodiscard]] static value_t load(const std::atomic<value_t>& word, const std::memory_order memory_order) noexcept
            {
                return word.load(memory_order);
            }

            static void fetch_and(std::atomic<value_t>& word, const value_t mask, const std::memory_order memory_order) noexcept
            {
                std::ignore = word.fetch_and(mask, memory_order);
            }

            static void fetch_or(std::atomic<value_t>& word, const value_t mask, const std::memory_order memory_order) noexcept
            {
                std::ignore = word.fetch_or(mask, memory_order);
            }

            static bool compare_exchange(std::atomic<value_t>& word, value_t& expected, const value_t desired, const std::memory_order memory_order_success, const std::memory_order memory_order_failure) noexcept
            {
                return word.compare_exchange_strong(expected, desired, memory_order_success, memory_order_failure);
            }
        };

        template <bit_container value_t>
        struct hierarchical_bits_traits
        {
            using value_type = value_t;
            using accessor_type = word_accessor<value_t>;
            using word_type = accessor_type::word_type;

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

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            [[nodiscard]] static constexpr std::optional<size_t> pop_unset(hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t index_start, const size_t index_end) noexcept
            {
                auto& words = get_words<current_depth_v>(bits);

                for (size_t index = index_start; index < index_end; ++index)
                {
                    word_type word = accessor_type::load(words[index], std::memory_order_relaxed);

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

                            if (accessor_type::compare_exchange(words[index], word, new_word, std::memory_order_release, std::memory_order_relaxed))
                            {
                                pop_unset_propagate<depth_v>(bits, index, new_word);

                                return child_index;
                            }
                        }
                        else
                        {
                            const auto& child_words = get_words<current_depth_v + 1>(bits);

                            if (child_index >= std::size(child_words))
                            {
                                break;
                            }

                            if (const std::optional<size_t> result = pop_unset<current_depth_v + 1>(bits, child_index, child_index + 1))
                            {
                                return result;
                            }

                            const word_type new_word = word | (static_cast<word_type>(1) << bit_index);

                            accessor_type::compare_exchange(words[index], word, new_word, std::memory_order_relaxed, std::memory_order_relaxed);
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

                    word_type parent_word = accessor_type::load(parent_words[parent_word_index], std::memory_order_relaxed);
                    const word_type new_parent_word = parent_word | (static_cast<word_type>(1) << parent_bit_index);

                    std::ignore = accessor_type::compare_exchange(parent_words[parent_word_index], parent_word, new_parent_word, std::memory_order_relaxed, std::memory_order_relaxed);

                    pop_unset_propagate<current_depth_v - 1>(bits, parent_word_index, new_parent_word);
                }
            }

            template <size_t current_depth_v, size_t size_v, size_t depth_v>
            static constexpr void reset(hierarchical_bits<value_t, size_v, depth_v>& bits, const size_t index) noexcept
            {
                auto& words = get_words<current_depth_v>(bits);

                const size_t word_index = index / bit_count<word_type>();
                const size_t bit_index = index % bit_count<word_type>();

                const word_type bit_mask = ~(static_cast<word_type>(1) << bit_index);

                constexpr std::memory_order memory_order = current_depth_v == depth_v ? std::memory_order_release : std::memory_order_relaxed;

                accessor_type::fetch_and(words[word_index], bit_mask, memory_order);

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

                const word_type word = accessor_type::load(words[word_index], std::memory_order_acquire);

                return (word >> bit_index) & static_cast<word_type>(1);
            }
        };

        template <bit_container value_t, size_t size_v, size_t max_depth_v>
        struct hierarchical_bitset
        {
            [[nodiscard]] static consteval size_t capacity()
            {
                return word_count<value_t, size_v>();
            }

            using accessor_type = word_accessor<value_t>;
            using traits_type = hierarchical_bits_traits<value_t>;
            using word_type = accessor_type::word_type;

            hierarchical_bits<value_t, capacity(), max_depth_v - 1> bits;

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
                      _word_index(capacity())
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
                    const auto& words = traits_type::template get_words<max_depth_v - 1>(_self->bits);

                    const word_type word = accessor_type::load(words[_word_index], std::memory_order_relaxed);

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
                    while (++_word_index < capacity())
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

            [[nodiscard]] constexpr bool is_set(const size_t index) const noexcept
            {
                return traits_type::is_set(bits, index);
            }

            [[nodiscard]] constexpr bool is_unset(const size_t index) const noexcept
            {
                return not traits_type::is_set(bits, index);
            }

            constexpr void reset(const size_t index) noexcept
            {
                traits_type::template reset<max_depth_v - 1>(bits, index);
            }

            [[nodiscard]] constexpr std::optional<size_t> pop_unset() noexcept
            {
                return traits_type::template pop_unset<0>(bits, 0, size_v);
            }
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

    struct slot_map_opts
    {
        size_t block_num = 0;
        size_t slot_num = 0;
        size_t level_num = 0;
    };

    struct thread_safe;
    struct thread_unsafe;

    template <typename tag_t>
    struct slot_map_traits;

    template <>
    struct slot_map_traits<thread_safe>
    {
        using bits_type = std::atomic<size_t>;
        using mutex_type = std::mutex;

        static void acquire_fence()
        {
            std::atomic_thread_fence(std::memory_order_acquire);
        }

        static void release_fence()
        {
            std::atomic_thread_fence(std::memory_order_release);
        }
    };

    template <>
    struct slot_map_traits<thread_unsafe>
    {
        struct no_mutex
        {
            static constexpr void lock() noexcept {}
            static constexpr void unlock() noexcept {}
        };

        using bits_type = size_t;
        using mutex_type = no_mutex;

        static constexpr void acquire_fence() {}
        static constexpr void release_fence() {}
    };

#if 0
    template <typename key_t, typename value_t, typename key_traits_t, typename map_traits_t, slot_map_opts opts_v>
    struct basic_slot_map
    {
        static_assert(opts_v.block_num > 0);
        static_assert(opts_v.slot_num > 0);
        static_assert(opts_v.level_num > 0);

        static consteval size_t capacity()
        {
            return opts_v.block_num * opts_v.slot_num;
        }

        using index_type = key_traits_t::index_type;
        using version_type = key_traits_t::version_type;
        using bits_type = map_traits_t::bits_type;
        using bitset_type = detail::hierarchical_bitset<bits_type, capacity(), opts_v.level_num>;

        template <bool const_v>
        struct iterator_impl
        {
            using this_type = std::conditional_t<const_v, const basic_slot_map, basic_slot_map>;
            using value_type = std::conditional_t<const_v, const value_t, value_t>;
            using bit_iterator = bitset_type::set_view::iterator;

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

                const size_t index_ = *_bit_it;

                const index_type block_index = static_cast<index_type>(index_ / opts_v.slot_num);
                const index_type slot_index = static_cast<index_type>(index_ % opts_v.slot_num);

                block* block = _self->try_get_block(block_index);

                SPORE_SLOT_MAP_ASSERT(block != nullptr);

                map_traits_t::acuire_fence();

                value_t& slot = block->slots[slot_index];

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
            : _control(std::make_unique<control>())
        {
        }

        constexpr ~basic_slot_map() noexcept(std::is_nothrow_destructible_v<value_t>)
        {
            std::unique_lock lock { _control->mutex };

            for (size_t block_index = 0; block_index < opts_v.block_num; ++block_index)
            {
                if (const std::unique_ptr<block>& block = _blocks[block_index])
                {
                    for (size_t slot_index = 0; slot_index < opts_v.slot_num; ++slot_index)
                    {
                        if (const size_t index = block_index * opts_v.slot_num + slot_index; _control->bitset.is_set(index))
                        {
                            value_t& slot = block->slots[slot_index];

                            std::destroy_at(&slot);
                        }
                    }
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
            const std::optional<size_t> maybe_index = _control->bitset.pop_unset();

            if (not maybe_index.has_value()) [[unlikely]]
            {
                return std::nullopt;
            }

            const index_type index = maybe_index.value();
            const index_type block_index = index / opts_v.slot_num;
            const index_type slot_index = index % opts_v.slot_num;

            block& block = get_block(block_index);
            value_t& slot = block.slots[slot_index];

            const version_type version = block.versions[slot_index];

            std::construct_at(&slot, std::forward<args_t>(args)...);

            map_traits_t::release_fence();

            return key_traits_t::make_key(index, version);
        }

        template <typename... args_t>
        [[nodiscard]] constexpr key_t emplace(args_t&&... args) noexcept(std::is_nothrow_constructible_v<value_t, args_t&&...> and SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            const std::optional<key_t> key = try_emplace(std::forward<args_t>(args)...);
            SPORE_SLOT_MAP_ASSERT(key.has_value());
            return key.value();
        }

        [[nodiscard]] constexpr value_t* find(const key_t& key) noexcept
        {
            const index_type index = key_traits_t::index(key);

            if (_control->bitset.is_unset(index)) [[unlikely]]
            {
                return nullptr;
            }

            const index_type block_index = index / opts_v.slot_num;
            const index_type slot_index = index % opts_v.slot_num;

            block* block = try_get_block(block_index);

            if (block == nullptr) [[unlikely]]
            {
                return nullptr;
            }

            map_traits_t::acquire_fence();

            value_t& slot = block->slots[slot_index];

            const version_type version = key_traits_t::version(key);
            const version_type expected_version = block->versions[slot_index];

            if (version != expected_version) [[unlikely]]
            {
                return nullptr;
            }

            return &slot;
        }

        [[nodiscard]] constexpr const value_t* find(const key_t& key) const noexcept
        {
            return const_cast<basic_slot_map&>(*this).find(key);
        }

        [[nodiscard]] constexpr value_t& at(const key_t& key) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            const index_type index = key_traits_t::index(key);

            SPORE_SLOT_MAP_ASSERT(_control->bitset.is_set(index));

            const index_type block_index = index / opts_v.slot_num;
            const index_type slot_index = index % opts_v.slot_num;

            block* block = try_get_block(block_index);

            SPORE_SLOT_MAP_ASSERT(block != nullptr);

            map_traits_t::acquire_fence();

            value_t& slot = block->slots[slot_index];

            SPORE_SLOT_MAP_ASSERT(key_traits_t::version(key) == block->versions[slot_index]);

            return slot;
        }

        [[nodiscard]] constexpr const value_t& at(const key_t& key) const noexcept
        {
            return const_cast<basic_slot_map&>(*this).at(key);
        }

        constexpr bool erase(const key_t& key) noexcept(std::is_nothrow_destructible_v<value_t>)
        {
            const index_type index = key_traits_t::index(key);

            if (_control->bitset.is_unset(index))
            {
                return false;
            }

            const index_type block_index = index / opts_v.slot_num;
            const index_type slot_index = index % opts_v.slot_num;

            block* block = try_get_block(block_index);

            if (block == nullptr) [[unlikely]]
            {
                return false;
            }

            map_traits_t::acquire_fence();

            value_t& slot = block->slots[slot_index];

            const version_type version = key_traits_t::version(key);
            version_type& current_version = block->versions[slot_index];

            if (version != current_version) [[unlikely]]
            {
                return false;
            }

            std::destroy_at(&slot);

            ++current_version;

            map_traits_t::release_fence();

            _control->bitset.reset(index);

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
        using mutex_type = map_traits_t::mutex_type;

        struct control
        {
            mutable mutex_type mutex;
            bitset_type bitset;
        };

        struct block
        {
            value_t slots[opts_v.slot_num];
            version_type versions[opts_v.slot_num] {};
        };

        std::unique_ptr<control> _control;
        std::unique_ptr<block> _blocks[opts_v.block_num] {};

        [[nodiscard]] constexpr block* try_get_block(const index_type index) const noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            SPORE_SLOT_MAP_ASSERT(index < opts_v.block_num);

            if (_blocks[index] == nullptr)
            {
                std::unique_lock lock { _control->mutex };

                if (_blocks[index] == nullptr)
                {
                    return nullptr;
                }
            }

            return _blocks[index].get();
        }

        [[nodiscard]] constexpr block& get_block(const index_type index) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            SPORE_SLOT_MAP_ASSERT(index < opts_v.block_num);

            if (_blocks[index] == nullptr)
            {
                std::unique_lock lock { _control->mutex };

                if (_blocks[index] == nullptr)
                {
                    _blocks[index] = std::make_unique<block>();
                }
            }

            return *_blocks[index];
        }
    };
#else
    template <typename key_t, typename value_t, typename key_traits_t, typename map_traits_t, slot_map_opts opts_v>
    struct basic_slot_map
    {
        static_assert(opts_v.block_num > 0);
        static_assert(opts_v.slot_num > 0);
        static_assert(opts_v.level_num > 0);

        static consteval size_t capacity()
        {
            return opts_v.block_num * opts_v.slot_num;
        }

        using index_type = key_traits_t::index_type;
        using version_type = key_traits_t::version_type;
        using bits_type = map_traits_t::bits_type;
        using bitset_type = detail::hierarchical_bitset<bits_type, capacity(), opts_v.level_num>;

        template <bool const_v>
        struct iterator_impl
        {
            using this_type = std::conditional_t<const_v, const basic_slot_map, basic_slot_map>;
            using value_type = std::conditional_t<const_v, const value_t, value_t>;
            using bit_iterator = bitset_type::set_view::iterator;

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

                map_traits_t::acquire_fence();

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
            for (size_t index = 0; index < opts_v.slot_num * opts_v.block_num; ++index)
            {
                if (_data->bitset.is_set(index))
                {
                    value_t& slot = _data->slots[index];

                    if constexpr (not std::is_trivially_destructible_v<value_t>)
                    {
                        std::destroy_at(&slot);
                    }
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
            const version_type version = _data->versions[index];

            value_t& slot = _data->slots[index];

            std::construct_at(&slot, std::forward<args_t>(args)...);

            map_traits_t::release_fence();

            return key_traits_t::make_key(index, version);
        }

        template <typename... args_t>
        [[nodiscard]] constexpr key_t emplace(args_t&&... args) noexcept(std::is_nothrow_constructible_v<value_t, args_t&&...> and SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            const std::optional<key_t> key = try_emplace(std::forward<args_t>(args)...);
            SPORE_SLOT_MAP_ASSERT(key.has_value());
            return key.value();
        }

        [[nodiscard]] constexpr value_t* find(const key_t& key) noexcept
        {
            const index_type index = key_traits_t::index(key);

            if (_data->control->bitset.is_unset(index)) [[unlikely]]
            {
                return nullptr;
            }

            map_traits_t::acquire_fence();

            value_t& slot = _data->slots[index];

            const version_type version = key_traits_t::version(key);
            const version_type expected_version = _data->versions[index];

            if (version != expected_version) [[unlikely]]
            {
                return nullptr;
            }

            return &slot;
        }

        [[nodiscard]] constexpr const value_t* find(const key_t& key) const noexcept
        {
            return const_cast<basic_slot_map&>(*this).find(key);
        }

        [[nodiscard]] constexpr value_t& at(const key_t& key) noexcept(SPORE_SLOT_MAP_ASSERT_NOEXCEPT)
        {
            const index_type index = key_traits_t::index(key);

            SPORE_SLOT_MAP_ASSERT(_data->bitset.is_set(index));

            map_traits_t::acquire_fence();

            value_t& slot = _data->slots[index];

            SPORE_SLOT_MAP_ASSERT(key_traits_t::version(key) == _data->versions[index]);

            return slot;
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

            map_traits_t::acquire_fence();

            value_t& slot = _data->slots[index];

            const version_type version = key_traits_t::version(key);
            const version_type expected_version = _data->versions[index];

            if (version != expected_version) [[unlikely]]
            {
                return false;
            }

            if constexpr (not std::is_trivially_destructible_v<value_t>)
            {
                std::destroy_at(&slot);
            }

            ++_data->versions[index];

            map_traits_t::release_fence();

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
            bitset_type bitset;
            value_t slots[opts_v.slot_num * opts_v.block_num];
            version_type versions[opts_v.slot_num * opts_v.block_num] {};
        };

        std::unique_ptr<data> _data;
    };
#endif

    template <typename value_t, size_t capacity_v>
    [[nodiscard]] consteval slot_map_opts default_slot_map_opts()
    {
        static_assert(capacity_v > 0);

        constexpr size_t min_page_size = SPORE_SLOT_MAP_MIN_PAGE_SIZE;
        constexpr size_t min_slot_num = SPORE_SLOT_MAP_MIN_SLOT_NUM;
        constexpr size_t max_root_word_num = SPORE_SLOT_MAP_MAX_ROOT_WORD_NUM;

        size_t page_size = min_page_size;
        size_t slot_num = page_size / sizeof(value_t);

        if ((page_size % sizeof(value_t)) > 0)
        {
            ++slot_num;
        }

        while (slot_num < min_slot_num)
        {
            page_size = (page_size << 1) + 1;
            slot_num = page_size / sizeof(value_t);
        }

        size_t block_num = capacity_v / slot_num;

        if ((capacity_v % slot_num) > 0)
        {
            ++block_num;
        }

        size_t level_num = 1;
        size_t word_num = capacity_v / detail::bit_count<size_t>();

        while (word_num > max_root_word_num)
        {
            ++level_num;
            word_num = word_num / detail::bit_count<size_t>();
        }

        return slot_map_opts { block_num, slot_num, level_num };
    }

    template <typename key_t, typename value_t, size_t capacity_v>
    using slot_map_st = basic_slot_map<key_t, value_t, slot_key_traits<key_t>, slot_map_traits<thread_unsafe>, default_slot_map_opts<value_t, capacity_v>()>;

    template <typename key_t, typename value_t, size_t capacity_v>
    using slot_map_mt = basic_slot_map<key_t, value_t, slot_key_traits<key_t>, slot_map_traits<thread_safe>, default_slot_map_opts<value_t, capacity_v>()>;
}