/**
 * Reaver Library Licence
 *
 * Copyright © 2016 Michał "Griwes" Dominiak
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation is required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 **/

#pragma once

#include <memory>
#include <exception>
#include <type_traits>

#include "exception.h"
#include "optional.h"
#include "overloads.h"
#include "tpl/rebind.h"
#include "tpl/filter.h"
#include "prelude/functor.h"
#include "function.h"

namespace reaver { inline namespace _v1
{
    constexpr struct ready_type {} ready = {};

    class executor
    {
    public:
        virtual ~executor() {}

        virtual void push(reaver::function<void ()> f) = 0;
    };

    template<typename T, typename... Args>
    std::shared_ptr<executor> make_executor(Args &&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    class broken_promise : public exception
    {
    public:
        broken_promise() : exception{ logger::fatal }
        {
            *this << "broken promise.";
        }
    };

    class multiple_noncopyable_continuations : public exception
    {
    public:
        multiple_noncopyable_continuations() : exception{ logger::error }
        {
            *this << "attempted to attach multiple continuations to a future promising a noncopyable object.";
        }
    };

    class multiple_exceptional_continuations : public exception
    {
    public:
        multiple_exceptional_continuations() : exception{ logger::error }
        {
            *this << "attempted to attach multiple exceptional continuations to a future.";
        }
    };

    template<typename T = void>
    class future;

    template<typename T>
    struct future_package_pair;

    template<typename F>
    auto package(F && f) -> future_package_pair<decltype(std::forward<F>(f)())>;

    namespace _detail
    {
        template<typename T>
        struct _shared_state;

        using _continuation_type = reaver::function<void ()>;

        template<typename T, typename F, typename std::enable_if<std::is_void<T>::value || std::is_copy_constructible<T>::value, int>::type = 0>
        void _add_continuation(_shared_state<T> & state, F && f)
        {
            state.continuations.emplace_back(_continuation_type{ std::forward<F>(f) });
        }

        template<typename T, typename F, typename std::enable_if<!std::is_void<T>::value && !std::is_copy_constructible<T>::value, int>::type = 0>
        void _add_continuation(_shared_state<T> & state, F && f)
        {
            if (state.continuations)
            {
                throw multiple_noncopyable_continuations{};
                return;
            }

            state.continuations = _continuation_type{ std::forward<F>(f) };
        }

        template<typename T, typename F>
        void _add_exceptional_continuation(_shared_state<T> & state, F && f)
        {
            if (state.exceptional_continuations)
            {
                throw multiple_exceptional_continuations{};
                return;
            }

            state.exceptional_continuations = _continuation_type{ std::forward<F>(f) };
        }

        template<typename T>
        bool _is_valid(_shared_state<T> & state)
        {
            return state.value.index() == 0 || state.function;
        }

        template<typename T>
        bool _is_pending(_shared_state<T> & state)
        {
            return state.value.index() == 2 && state.function;
        }

        template<typename... Args, typename std::enable_if<all_of<std::is_copy_constructible<Args>::value...>::value, int>::type = 0>
        auto _move_or_copy(variant<Args...> & v, bool move)
        {
            if (move)
            {
                auto ret = std::move(v);
                v = none;
                return ret;
            }

            return v;
        }

        template<typename... Args, typename std::enable_if<!all_of<std::is_copy_constructible<Args>::value...>::value, int>::type = 0>
        auto _move_or_copy(variant<Args...> & v, bool)
        {
            auto ret = std::move(v);
            v = none;
            return ret;
        }

        template<typename T>
        struct _shared_state : public std::enable_shared_from_this<_shared_state<T>>
        {
            using then_t = std::conditional_t<
                std::is_copy_constructible<T>::value,
                std::vector<_continuation_type>,
                optional<_continuation_type>
            >;
            using failed_then_t = optional<_continuation_type>;

            _shared_state(T t) : value{ std::move(t) }
            {
            }

            _shared_state(std::exception_ptr ptr) : value{ ptr }
            {
            }

            _shared_state() : value{ none }
            {
            }

            std::mutex lock;
            variant<T, std::exception_ptr, none_t> value;
            std::atomic<std::size_t> promise_count{ 0 };
            std::atomic<std::size_t> shared_count{ 0 };

            std::shared_ptr<executor> scheduler;
            then_t continuations;
            failed_then_t exceptional_continuations;

            // TODO: reaver::function
            optional<class function<T ()>> function;

            optional<T> try_get()
            {
                std::lock_guard<std::mutex> l(lock);
                return get<0>(fmap(_move_or_copy(value, shared_count == 1 && value.index() != 1), make_overload_set(
                    [&](T t) {
                        if (shared_count == 1)
                        {
                            value = none;
                        }
                        return make_optional(std::move(t));
                    },

                    [&](std::exception_ptr ptr) {
                        std::rethrow_exception(ptr);
                        return unit{};
                    },

                    [&](none_t) {
                        return optional<T>();
                    }
                )));
            }

        private:
            T _get()
            {
                assert(value.index() != 2);

                if (value.index() == 1)
                {
                    std::rethrow_exception(get<1>(value));
                }

                auto ret = get<0>(_move_or_copy(value, shared_count == 1 && value.index() != 1));
                if (shared_count == 1)
                {
                    value = none;
                }
                return ret;
            }

        public:
            template<typename F>
            auto then(std::shared_ptr<executor> provided_sched, F && f) -> future<decltype(std::forward<F>(f)(std::declval<T>()))>
            {
                std::lock_guard<std::mutex> l{ lock };

                if (!_is_valid(*this))
                {
                    assert(!"what do?");
                }

                ++shared_count;

                auto sched = [this, provided = std::move(provided_sched)]() {
                    if (provided)
                    {
                        return provided;
                    }

                    if (scheduler)
                    {
                        return scheduler;
                    }

                    assert(!"I really don't know what to do this time.");
                };

                return get<0>(fmap(value, make_overload_set(
                    [&](variant<const T &, std::exception_ptr>) {
                        auto pair = package([this, f = std::forward<F>(f)]() mutable {
                            return std::forward<F>(f)(_get());
                        });

                        // GCC is deeply confused when this is directly in the capture list
                        auto state = std::enable_shared_from_this<_shared_state>::shared_from_this();

                        sched()->push([sched, task = std::move(pair.packaged_task), state = std::move(state)](){ task(sched()); });
                        return std::move(pair.future);
                    },

                    [&](none_t) {
                        auto pair = package([this, f = std::forward<F>(f)]() mutable {
                            return std::forward<F>(f)(_get());
                        });

                        // GCC is deeply confused when this is directly in the capture list
                        auto state = std::enable_shared_from_this<_shared_state>::shared_from_this();

                        _add_continuation(*this, [sched, task = std::move(pair.packaged_task), state = std::move(state)]() mutable {
                            sched()->push([sched, task = std::move(task), state = std::move(state)](){ task(sched()); });
                        });
                        return std::move(pair.future);
                    }
                )));
            }

            template<typename F>
            auto on_error(std::shared_ptr<executor> provided_sched, F && f) -> future<decltype(std::forward<F>(f)(std::declval<std::exception_ptr>()))>
            {
                std::lock_guard<std::mutex> l{ lock };

                if (!_is_valid(*this))
                {
                    assert(!"what do?");
                }

                ++shared_count;

                auto sched = [this, provided = std::move(provided_sched)]() {
                    if (provided)
                    {
                        return provided;
                    }

                    if (scheduler)
                    {
                        return scheduler;
                    }

                    assert(!"what do again?");
                };

                return get<1>(fmap(value, make_overload_set(
                    [&](const T &) {
                        return unit{};
                    },

                    [&](std::exception_ptr ptr) {
                        auto pair = package([this, f = std::forward<F>(f), ptr]() mutable {
                            return std::forward<F>(f)(ptr);
                        });

                        // GCC is deeply confused when this is directly in the capture list
                        auto state = std::enable_shared_from_this<_shared_state>::shared_from_this();

                        sched()->push([sched, task = std::move(pair.packaged_task), state = std::move(state)](){ task(sched()); });
                        return std::move(pair.future);
                    },

                    [&](none_t) {
                        auto pair = package([this, f = std::forward<F>(f)]() mutable {
                            if (value.index() == 1)
                            {
                                auto ptr = get<1>(value);
                                return std::forward<F>(f)(ptr);
                            }
                            assert(!"or maybe make the resulting future exceptional? this would be a solution, though not a stellar one");
                            assert(!"I guess we should somehow propagate the value here, but there's no obvious way to do that");
                        });

                        // GCC is deeply confused when this is directly in the capture list
                        auto state = std::enable_shared_from_this<_shared_state>::shared_from_this();

                        _add_exceptional_continuation(*this, [sched, task = std::move(pair.packaged_task), state = std::move(state)]() mutable {
                            sched()->push([sched, task = std::move(task), state = std::move(state)](){ task(sched()); });
                        });
                        return std::move(pair.future);
                    }
                )));
            }

            template<typename F>
            auto then(F && f)
            {
                if (!scheduler)
                {
                    assert("here we need a default executor");
                }

                return then(scheduler, std::forward<F>(f));
            }

            template<typename F>
            auto on_error(F && f)
            {
                if (!scheduler)
                {
                    assert("here we need a default executor");
                }

                return on_error(scheduler, std::forward<F>(f));
            }
        };

        template<>
        struct _shared_state<void> : std::enable_shared_from_this<_shared_state<void>>
        {
            using then_t = std::vector<_continuation_type>;
            using failed_then_t = optional<_continuation_type>;

            _shared_state(ready_type) : value{ ready }
            {
            }

            _shared_state(std::exception_ptr ptr) : value{ ptr }
            {
            }

            _shared_state() : value{ none }
            {
            }

            mutable std::mutex lock;
            variant<ready_type, std::exception_ptr, none_t> value;
            std::atomic<std::size_t> promise_count{ 0 };
            std::atomic<std::size_t> shared_count{ 0 };

            std::shared_ptr<executor> scheduler;
            then_t continuations;
            failed_then_t exceptional_continuations;

            // TODO: reaver::function
            optional<class function<void ()>> function;

            bool try_get()
            {
                std::lock_guard<std::mutex> l(lock);
                return get<0>(fmap(value, make_overload_set(
                    [&](ready_type) {
                        if (shared_count == 1)
                        {
                            value = none;
                        }
                        return true;
                    },

                    [&](std::exception_ptr ptr) {
                        std::rethrow_exception(ptr);
                        return unit{};
                    },

                    [&](none_t) {
                        return false;
                    }
                )));
            }

        private:
            void _get()
            {
                assert(value.index() != 2);

                if (value.index() == 1)
                {
                    std::rethrow_exception(get<1>(value));
                }

                if (shared_count == 1)
                {
                    value = none;
                }
            }

        public:
            template<typename F>
            auto then(std::shared_ptr<executor> provided_sched, F && f) -> future<decltype(std::forward<F>(f)())>
            {
                std::lock_guard<std::mutex> l{ lock };

                if (!_is_valid(*this))
                {
                    assert(!"what do?");
                }

                auto sched = [this, provided = std::move(provided_sched)]() {
                    if (provided)
                    {
                        return provided;
                    }

                    if (scheduler)
                    {
                        return scheduler;
                    }

                    assert(!"I really don't know what to do this time.");
                };

                ++shared_count;

                return get<0>(fmap(value, make_overload_set(
                    [&](variant<ready_type, std::exception_ptr>) {
                        auto pair = package([this, f = std::forward<F>(f)]() mutable {
                            _get(); // will throw if in exceptional state
                            return std::forward<F>(f)();
                        });

                        // GCC ICEs when this is in capture list directly
                        auto state = shared_from_this();

                        sched()->push([sched, task = std::move(pair.packaged_task), state = std::move(state)](){ task(sched()); });
                        return std::move(pair.future);
                    },

                    [&](none_t) {
                        auto pair = package([this, f = std::forward<F>(f)]() mutable {
                            _get(); // will throw if in exceptional state
                            return std::forward<F>(f)();
                        });

                        // GCC ICEs when this is in capture list directly
                        auto state = shared_from_this();

                        _add_continuation(*this, [sched, task = std::move(pair.packaged_task), state = std::move(state)]() mutable {
                            sched()->push([sched, task = std::move(task), state = std::move(state)](){ task(sched()); });
                        });
                        return std::move(pair.future);
                    }
                )));
            }

            template<typename F>
            auto on_error(std::shared_ptr<executor> provided_sched, F && f) -> future<decltype(std::forward<F>(f)(std::declval<std::exception_ptr>()))>
            {
                std::lock_guard<std::mutex> l{ lock };

                if (!_is_valid(*this))
                {
                    assert(!"what do?");
                }

                ++shared_count;

                auto sched = [this, provided = std::move(provided_sched)]() {
                    if (provided)
                    {
                        return provided;
                    }

                    if (scheduler)
                    {
                        return scheduler;
                    }

                    assert(!"what do again?");
                };

                return get<1>(fmap(value, make_overload_set(
                    [&](ready_type) {
                        return unit{};
                    },

                    [&](std::exception_ptr ptr) {
                        auto pair = package([this, f = std::forward<F>(f), ptr]() mutable {
                            return std::forward<F>(f)(ptr);
                        });

                        // GCC ICEs when this is in capture list directly
                        auto state = shared_from_this();

                        sched()->push([sched, task = std::move(pair.packaged_task), state = std::move(state)](){ task(sched()); });
                        return std::move(pair.future);
                    },

                    [&](none_t) {
                        auto pair = package([this, f = std::forward<F>(f)]() mutable {
                            if (value.index() == 1)
                            {
                                auto ptr = get<1>(value);
                                return std::forward<F>(f)(ptr);
                            }
                            assert(!"or maybe make the resulting future exceptional? this would be a solution, though not a stellar one");
                            assert(!"I guess we should somehow propagate the value here, but there's no obvious way to do that");
                        });

                        // GCC ICEs when this is in capture list directly
                        auto state = shared_from_this();

                        _add_exceptional_continuation(*this, [sched, task = std::move(pair.packaged_task), state = std::move(state)]() mutable {
                            sched()->push([sched, task = std::move(task), state = std::move(state)](){ task(sched()); });
                        });
                        return std::move(pair.future);
                    }
                )));
            }

            template<typename F>
            auto then(F && f)
            {
                if (!scheduler)
                {
                    assert("here we need a default executor");
                }

                return then(scheduler, std::forward<F>(f));
            }

            template<typename F>
            auto on_error(F && f)
            {
                if (!scheduler)
                {
                    assert("here we need a default executor");
                }

                return on_error(scheduler, std::forward<F>(f));
            }
        };

        template<typename T>
        void _add_promise(_shared_state<T> & state)
        {
            ++state.promise_count;
        }

        template<typename T>
        void _remove_promise(_shared_state<T> & state)
        {
            if (--state.promise_count == 0)
            {
                std::lock_guard<std::mutex> lock{ state.lock };
                if (_is_pending(state))
                {
                    state.function = {};
                    state.value = std::make_exception_ptr(broken_promise{});
                }
            }
        }
    }

    template<typename T>
    class packaged_task
    {
        packaged_task(std::weak_ptr<_detail::_shared_state<T>> ptr) : _state{ std::move(ptr) }
        {
            _add_promise();
        }

        void _add_promise()
        {
            auto state = _state.lock();
            if (state)
            {
                _detail::_add_promise(*state);
            }
        }

        void _remove_promise()
        {
            auto state = _state.lock();
            if (state)
            {
                _detail::_remove_promise(*state);
            }
        }

    public:
        template<typename F>
        friend auto package(F && f) -> future_package_pair<decltype(std::forward<F>(f)())>;

        packaged_task(const packaged_task & other) : _state{ other._state }
        {
            _add_promise();
        }

        packaged_task(packaged_task && other) noexcept : _state{ other._state }
        {
            other._state = {};
        }

        packaged_task & operator=(const packaged_task & other)
        {
            _remove_promise();
            _state = other._state;
            _add_promise();
        }

        packaged_task & operator=(packaged_task && other) noexcept
        {
            _remove_promise();
            _state = other._state;
            other._state = {};
        }

        ~packaged_task()
        {
            _remove_promise();
        }

        void operator()(std::shared_ptr<executor> sched = nullptr) const
        {
            auto state = _state.lock();

            if (!state)
            {
                return;
            }

            std::lock_guard<std::mutex> lock{ state->lock };
            state->scheduler = std::move(sched);

            try
            {
                state->value = (*state->function)();
            }
            catch (...)
            {
                state->value = std::current_exception();
                if (state->exceptional_continuations)
                {
                    fmap(state->exceptional_continuations, [](auto && cont){ cont(); return unit{}; });
                }
            }

            fmap(state->continuations, [&](auto && cont) {
                if (state->value.index() != 2)
                {
                    cont();
                }
                return unit{};
            });

            state->function = {};
        }

    private:
        std::weak_ptr<_detail::_shared_state<T>> _state;
    };

    template<>
    inline void packaged_task<void>::operator()(std::shared_ptr<executor> sched) const
    {
        auto state = _state.lock();

        if (!state)
        {
            return;
        }

        std::lock_guard<std::mutex> lock{ state->lock };
        state->scheduler = std::move(sched);

        try
        {
            (*state->function)();
            state->value = ready;
        }
        catch (...)
        {
            state->value = std::current_exception();
            if (state->exceptional_continuations)
            {
                fmap(state->exceptional_continuations, [](auto && cont){ cont(); return unit{}; });
            }
        }

        fmap(state->continuations, [&](auto && cont) {
            if (state->value.index() != 2)
            {
                cont();
            }
            return unit{};
        });

        state->function = {};
    }

    template<typename T>
    class future
    {
        future(std::shared_ptr<_detail::_shared_state<T>> state) : _state{ std::move(state) }
        {
            _add_count();
        }

        void _add_count()
        {
            if (_state)
            {
                ++_state->shared_count;
            }
        }

        void _remove_count()
        {
            if (_state)
            {
                --_state->shared_count;
            }
        }

    public:
        using value_type = T;

        template<typename F>
        friend auto package(F && f) -> future_package_pair<decltype(std::forward<F>(f)())>;

        future(const future & other) : _state{ other._state }
        {
            _add_count();
        }

        future(future && other) noexcept : _state{ std::move(other._state) }
        {
            other._state = {};
        }

        future & operator=(const future & other)
        {
            _remove_count();
            _state = other._state;
            _add_count();

            return *this;
        }

        future & operator=(future && other) noexcept
        {
            _remove_count();
            _state = std::move(other._state);
            other._state = {};

            return *this;
        }

        explicit future(T value) : future{ std::make_shared<_detail::_shared_state<T>>(std::move(value)) }
        {
        }

        ~future()
        {
            _remove_count();
        }

        auto try_get()
        {
            return _state->try_get();
        };

        template<typename F>
        auto then(std::shared_ptr<executor> sched, F && f) -> future<decltype(std::forward<F>(f)(std::declval<T>()))>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->then(std::move(sched), std::forward<F>(f));
        }

        template<typename F>
        auto on_error(std::shared_ptr<executor> sched, F && f) -> future<decltype(std::forward<F>(f)(std::declval<std::exception_ptr>()))>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->on_error(std::move(sched), std::forward<F>(f));
        }

        template<typename F>
        auto then(F && f) -> future<decltype(std::forward<F>(f)(std::declval<T>()))>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->then(std::forward<F>(f));
        }

        template<typename F>
        auto on_error(F && f) -> future<decltype(std::forward<F>(f)(std::declval<std::exception_ptr>()))>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->on_error(std::forward<F>(f));
        }
    private:
        std::shared_ptr<_detail::_shared_state<T>> _state;
    };

    template<typename T>
    auto make_ready_future(T && t)
    {
        return future<T>{ std::forward<T>(t) };
    }

    template<>
    class future<void>
    {
        future(std::shared_ptr<_detail::_shared_state<void>> state) : _state{ std::move(state) }
        {
            _add_count();
        }

        void _add_count()
        {
            if (_state)
            {
                ++_state->shared_count;
            }
        }

        void _remove_count()
        {
            if (_state)
            {
                --_state->shared_count;
            }
        }

    public:
        using value_type = void;

        template<typename F>
        friend auto package(F && f) -> future_package_pair<decltype(std::forward<F>(f)())>;

        future(const future & other) : _state{ other._state }
        {
            _add_count();
        }

        future(future && other) noexcept : _state{ std::move(other._state) }
        {
            other._state = {};
        }

        future & operator=(const future & other)
        {
            _remove_count();
            _state = other._state;
            _add_count();

            return *this;
        }

        future & operator=(future && other) noexcept
        {
            _remove_count();
            _state = std::move(other._state);
            other._state = {};

            return *this;
        }

        explicit future(ready_type) : future{ std::make_shared<_detail::_shared_state<void>>(ready) }
        {
        }

        ~future()
        {
            _remove_count();
        }

        bool try_get()
        {
            return _state->try_get();
        };

        template<typename F>
        auto then(std::shared_ptr<executor> sched, F && f) -> future<decltype(std::forward<F>(f)())>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->then(std::move(sched), std::forward<F>(f));
        }

        template<typename F>
        auto on_error(std::shared_ptr<executor> sched, F && f) -> future<decltype(std::forward<F>(f)(std::declval<std::exception_ptr>()))>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->on_error(std::move(sched), std::forward<F>(f));
        }

        template<typename F>
        auto then(F && f) -> future<decltype(std::forward<F>(f)())>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->then(std::forward<F>(f));
        }

        template<typename F>
        auto on_error(F && f) -> future<decltype(std::forward<F>(f)(std::declval<std::exception_ptr>()))>
        {
            if (!_state)
            {
                assert(!"handle this somehow (new exception type!)");
            }

            return _state->on_error(std::forward<F>(f));
        }

    private:
        std::shared_ptr<_detail::_shared_state<void>> _state;
    };

    auto make_ready_future()
    {
        return future<>{ ready };
    }

    template<typename T>
    struct future_package_pair
    {
        class packaged_task<T> packaged_task;
        class future<T> future;
    };

    template<typename F>
    auto package(F && f) -> future_package_pair<decltype(std::forward<F>(f)())>
    {
        using T = decltype(std::forward<F>(f)());

        auto state = std::make_shared<_detail::_shared_state<T>>();
        state->function = std::forward<F>(f);

        return future_package_pair<T>{ { state }, { std::move(state) } };
    };

    namespace _detail
    {
        template<typename T>
        struct _is_not_void : std::integral_constant<bool, !std::is_void<T>::value> {};

        template<typename... Args>
        using _optional_tuple = std::tuple<optional<Args>...>;

        template<typename... Args, std::size_t... I>
        auto _remove_optional(std::tuple<optional<Args>...> & opt, std::index_sequence<I...>)
        {
            return std::tuple<Args...>{ std::move(*get<I>(opt))... };
        }

        template<std::size_t N>
        using _int = std::integral_constant<std::size_t, N>;
    }

    enum class exception_policy
    {
        aggregate,
        abort_on_first_failure
    };

    template<typename... Ts, typename std::enable_if<(sizeof...(Ts) > 0), int>::type = 0>
    auto when_all(exception_policy policy, future<Ts>... futures)
    {
        using nonvoid = tpl::filter<
            tpl::vector<Ts...>,
            _detail::_is_not_void
        >;

        using return_type = tpl::rebind<
            nonvoid,
            std::tuple
        >;

        using buffer_type = tpl::rebind<
            nonvoid,
            _detail::_optional_tuple
        >;

        struct internal_state
        {
            buffer_type buffer;
            std::atomic<std::size_t> remaining{ sizeof...(Ts) };
            optional<packaged_task<return_type>> task;
            std::vector<future<>> futures;
            exception_list exceptions;
        };

        auto state = std::make_shared<internal_state>();
        auto pair = package([state](){
            state->futures = {};

            if (state->exceptions.size())
            {
                throw std::move(state->exceptions);
            }

            return _detail::_remove_optional(state->buffer, std::make_index_sequence<nonvoid::size>());
        });
        state->task = std::move(pair.packaged_task);

        auto void_handler = [state]() {
            if (--state->remaining == 0)
            {
                (*state->task)();
            }
        };

        auto nonvoid_handler = [&](auto index) {
            return [state](auto value) {
                get<decltype(index)::value>(state->buffer) = std::move(value);

                if (--state->remaining == 0)
                {
                    (*state->task)();
                }
            };
        };

        auto on_error = [state, policy](auto exception_ptr) {
            switch (policy)
            {
                case exception_policy::aggregate:
                    state->exceptions.push_back(exception_ptr);
                    if (--state->remaining == 0)
                    {
                        (*state->task)();
                    }
                    break;

                case exception_policy::abort_on_first_failure:
                    assert(!"implement this shit");
                    break;
            }
        };

        auto handler = make_overload_set(
            [](auto /* self */, auto /* index */){},

            [&](auto self, auto index, future<void> & vf, auto &... rest) {
                state->futures.push_back(vf.then(void_handler));
                state->futures.push_back(vf.on_error(on_error));
                self(self, _detail::_int<index>(), rest...);
            },

            [&](auto self, auto index, auto & nvf, auto &... rest) {
                state->futures.push_back(nvf.then(nonvoid_handler(index)));
                state->futures.push_back(nvf.on_error(on_error));
                self(self, _detail::_int<index + 1>(), rest...);
            }
        );

        handler(handler, _detail::_int<0>(), futures...);

        return std::move(pair.future);
    }

    template<typename... Ts, typename std::enable_if<(sizeof...(Ts) > 0), int>::type = 0>
    auto when_all(future<Ts>... futures)
    {
        return when_all(exception_policy::aggregate, std::move(futures)...);
    }

    auto when_all(exception_policy = exception_policy::aggregate)
    {
        return make_ready_future();
    }

    template<typename T, typename F>
    auto fmap(future<T> fut, F && f)
    {
        return std::move(fut).then(std::forward<F>(f));
    }
}}

