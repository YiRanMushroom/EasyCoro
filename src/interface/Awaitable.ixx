export module EasyCoro.Awaitable;

import std;
import EasyCoro.ThreadPool;
import <cassert>;
import <cstddef>;

namespace EasyCoro {
    export class ExecutionContext;

    export thread_local ExecutionContext *CurrentExecutionContext = nullptr;

    class ExecutionContext {
    public:
        ExecutionContext(size_t threadCount = std::jthread::hardware_concurrency() * 2)
            : m_ThreadPool(threadCount, [this] {
                CurrentExecutionContext = this;
            }, [] {
                CurrentExecutionContext = nullptr;
            }) {
        }

    public:
        void Schedule(std::shared_ptr<void> handle) {
            m_ThreadPool.Enqueue([weak = std::weak_ptr(handle)] {
                if (auto shared = weak.lock()) {
                    std::coroutine_handle<> coroHandle = std::coroutine_handle<>::from_address(shared.get());
                    if (!coroHandle.done()) {
                        coroHandle.resume();
                    }
                }
            });
        }

        void WaitAllTaskToFinish() {
            m_ThreadPool.WaitAllTaskToFinish();
        }

        auto BlockOn(auto &&awaitable);

    private:
        ThreadPool m_ThreadPool{};
    };

    export ExecutionContext &GetCurrentExecutionContext() {
        if (!CurrentExecutionContext) {
            throw std::runtime_error("No current execution context set");
        }

        return *CurrentExecutionContext;
    }


    export template<typename Ret>
    class SimpleAwaitable;

    template<typename TargetType = void>
    std::coroutine_handle<typename SimpleAwaitable<TargetType>::PromiseType>
    PointerToHandleCast(const std::shared_ptr<void>& ptr) {
        assert(ptr);
        return std::coroutine_handle<typename SimpleAwaitable<TargetType>::PromiseType>::from_address(
            ptr.get());
    }

    template<typename Ret>
    const std::shared_ptr<void>& HandleToPointerCast(
        std::coroutine_handle<typename SimpleAwaitable<Ret>::PromiseType> handle) {
        assert(handle);
        return handle.promise().Self;
    }

    template<typename TargetType = void>
    std::coroutine_handle<typename SimpleAwaitable<TargetType>::PromiseType>
    HandleReinterpretCast(std::coroutine_handle<> handle) {
        assert(handle);
        return std::coroutine_handle<typename SimpleAwaitable<TargetType>::PromiseType>::from_address(
            handle.address());
    }

    export template<>
    class SimpleAwaitable<void> {
    public:
        struct PromiseType {
            std::shared_ptr<void> Self;

            std::atomic_bool IsCancelled = false;
            std::function<void()> OnFinished = nullptr;

            std::mutex ResultProtectMutex;
            std::variant<std::monostate, std::exception_ptr> Result{std::monostate{}};

            void Cancel() {
                IsCancelled = true;
            }

            auto get_return_object() {
                return SimpleAwaitable{std::coroutine_handle<PromiseType>::from_promise(*this)};
            }

            constexpr static std::suspend_always initial_suspend() { return {}; }
            constexpr static std::suspend_always final_suspend() noexcept { return {}; }

            void return_void() {
                // Protect
                {
                    std::scoped_lock lock(ResultProtectMutex);
                    Result = std::monostate{};
                }

                if (OnFinished)
                    OnFinished();
            }

            void unhandled_exception() { Result = std::current_exception(); }
        };

        using promise_type = PromiseType;

        std::coroutine_handle<PromiseType> m_MyHandle;

    public:
        SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandle(
            handle) {
            handle.promise().Self =
                    std::shared_ptr<void>(handle.address(),
                                          [](void *ptr) {
                                              if (ptr) {
                                                  std::coroutine_handle<PromiseType>::from_address(ptr).
                                                          destroy();
                                              }
                                          });
        }

        SimpleAwaitable(const SimpleAwaitable &) = delete;

        SimpleAwaitable(SimpleAwaitable &&other) noexcept : m_MyHandle(other.m_MyHandle) {
            other.m_MyHandle = nullptr;
        }

        SimpleAwaitable &operator=(const SimpleAwaitable &) = delete;

        SimpleAwaitable &operator=(SimpleAwaitable &&other) noexcept {
            std::swap(m_MyHandle, other.m_MyHandle);
            return *this;
        }

    protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return m_MyHandle;
        }

    public:
        [[nodiscard]] std::coroutine_handle<> GetHandle() const {
            return GetMyHandle();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        std::function<void()> OnFinishedCallback = nullptr;

        void await_suspend(std::coroutine_handle<> parentHandle) {
            if (!GetMyHandle().promise().OnFinished) {
                GetMyHandle().promise().OnFinished = [parentHandle = std::weak_ptr(
                            std::coroutine_handle<PromiseType>::from_address(
                                parentHandle.address()).promise().Self)]() mutable {
                            if (auto copied = parentHandle.lock()) {
                                parentHandle.reset();
                                GetCurrentExecutionContext().Schedule(copied);
                            }
                        };
            }
            GetCurrentExecutionContext().Schedule(GetMyHandle().promise().Self);
        }

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        void SetOnFinished(auto &&callback) {
            GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
        }

        void await_resume() {
            auto handle = GetMyHandle();

            auto &variant = handle.promise().Result;
            switch (variant.index()) {
                case 0:
                    return;
                case 1:
                    std::rethrow_exception(std::get<std::exception_ptr>(variant));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }

        bool IsCancelled() const {
            return GetMyHandle().promise().IsCancelled;
        }

        void GetResult() {
            auto handle = GetMyHandle();
            assert(handle.done());
            await_resume();
        }
    };

    template<typename Ret>
    class SimpleAwaitable {
    public:
        using ReturnType = Ret;

        struct PromiseType {
            std::shared_ptr<void> Self;

            std::atomic_bool IsCancelled = false;
            std::function<void()> OnFinished = nullptr;

            std::mutex ResultProtectMutex;
            std::variant<std::monostate, Ret, std::exception_ptr> Result{std::monostate{}};

            void Cancel() {
                IsCancelled = true;
            }

            auto get_return_object() {
                return SimpleAwaitable{std::coroutine_handle<PromiseType>::from_promise(*this)};
            }

            constexpr static std::suspend_always initial_suspend() { return {}; }
            constexpr static std::suspend_always final_suspend() noexcept { return {}; }

            void return_value(Ret value) {
                // Protect
                {
                    std::scoped_lock lock(ResultProtectMutex);
                    Result = std::move(value);
                }
                if (OnFinished) {
                    OnFinished();
                }
            }

            void unhandled_exception() { Result = std::current_exception(); }
        };

        using promise_type = PromiseType;

        std::coroutine_handle<PromiseType> m_MyHandle;

    public:
        SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandle(
            handle) {
            handle.promise().Self =
                    std::shared_ptr<void>(handle.address(),
                                          [](void *ptr) {
                                              if (ptr) {
                                                  std::coroutine_handle<PromiseType>::from_address(ptr).
                                                          destroy();
                                              }
                                          });
        }

        SimpleAwaitable(const SimpleAwaitable &) = delete;

        SimpleAwaitable(SimpleAwaitable &&other) noexcept : m_MyHandle(other.m_MyHandle) {
            other.m_MyHandle = nullptr;
        }

        SimpleAwaitable &operator=(const SimpleAwaitable &) = delete;

        SimpleAwaitable &operator=(SimpleAwaitable &&other) noexcept {
            std::swap(m_MyHandle, other.m_MyHandle);
        }

    protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return m_MyHandle;
        }

    public:
        ~SimpleAwaitable() {
        }

        [[nodiscard]] std::coroutine_handle<> GetHandle() const {
            return GetMyHandle();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            if (!GetMyHandle().promise().OnFinished) {
                GetMyHandle().promise().OnFinished = [parentHandle = std::weak_ptr(
                                std::coroutine_handle<PromiseType>::from_address(
                                    parentHandle.address()).promise().Self)
                        ]() mutable {
                            if (auto copied = parentHandle.lock()) {
                                parentHandle.reset();
                                GetCurrentExecutionContext().Schedule(copied);
                            }
                        };
            }
            GetCurrentExecutionContext().Schedule(GetMyHandle().promise().Self);
        }

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        void SetOnFinished(auto &&callback) {
            GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
        }

        Ret await_resume() {
            auto handle = GetMyHandle();

            auto &variant = handle.promise().Result;
            switch (variant.index()) {
                case 0:
                    throw std::runtime_error("Coroutine did not return a value");
                case 1:
                    return std::get<Ret>(std::move(variant));
                case 2:
                    std::rethrow_exception(std::get<std::exception_ptr>(variant));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }

        bool IsCancelled() const {
            return GetMyHandle().promise().IsCancelled;
        }

        Ret GetResult() {
            auto handle = GetMyHandle();
            assert(handle.done());
            return await_resume();
        }

        std::optional<Ret> TryGetResult() {
            if (std::holds_alternative<Ret>(GetMyHandle().promise().Result)) {
                return std::get<Ret>(GetMyHandle().promise().Result);
            }
            return std::nullopt;
        }
    };

    auto ExecutionContext::BlockOn(auto &&awaitable) {
        auto handle = awaitable.GetHandle();
        std::shared_ptr handlePtr = std::coroutine_handle<SimpleAwaitable<
            void>::PromiseType>::from_address(handle.address()).promise().Self;
        if (!handle.done()) {
            Schedule(handlePtr);
        }
        while (!handle.done()) {
            WaitAllTaskToFinish();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return awaitable.GetResult();
    }

    struct AwaitToGetThisHandle {
        std::coroutine_handle<> parentHandle;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle) {
            parentHandle = handle;
            GetCurrentExecutionContext().Schedule(
                std::coroutine_handle<SimpleAwaitable<void>::PromiseType>::from_address(
                    handle.address()).promise().Self);
        }

        std::coroutine_handle<> await_resume() const noexcept {
            return parentHandle;
        }
    };

    export template<typename... Awaitables>
    SimpleAwaitable<std::tuple<typename Awaitables::ReturnType...>> AllOf(Awaitables... awaitables) {
        auto parentHandle = std::coroutine_handle<SimpleAwaitable<void>::PromiseType>::from_address(
            (co_await AwaitToGetThisHandle{}).address()).promise().Self;

        std::shared_ptr<std::function<void()>> onChildSuspend = std::make_shared<std::function<
            void()>>(

            [remaining = std::make_shared<std::atomic_size_t>(sizeof...(Awaitables)), parentHandle]() mutable {
                if (--*remaining == 0) {
                    GetCurrentExecutionContext().Schedule(parentHandle);
                } else {
                    std::cout << *remaining << " awaitables remaining..." << std::endl;
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);

        (GetCurrentExecutionContext().Schedule(
            std::coroutine_handle<SimpleAwaitable<void>::PromiseType>::from_address(
                awaitables.GetHandle().address()).promise().Self), ...);

        co_await std::suspend_always{};

        co_return std::make_tuple(awaitables.GetResult()...);
    }

    export template<typename... Awaitables>
    SimpleAwaitable<std::tuple<std::optional<typename Awaitables::ReturnType>...>> AnyOf(Awaitables... awaitables) {
        auto parentHandle = std::coroutine_handle<SimpleAwaitable<void>::PromiseType>::from_address(
            (co_await AwaitToGetThisHandle{}).address()).promise().Self;

        std::shared_ptr<std::function<void()>> onChildSuspend = std::make_shared<std::function<
            void()>>(

            [Invoked = std::make_shared<std::atomic_bool>(false), parentHandle]() mutable {
                bool expected = false;
                if (Invoked->compare_exchange_strong(expected, true)) {
                    GetCurrentExecutionContext().Schedule(parentHandle);
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);

        (GetCurrentExecutionContext().Schedule(
            std::coroutine_handle<SimpleAwaitable<void>::PromiseType>::from_address(
                awaitables.GetHandle().address()).promise().Self), ...);

        co_await std::suspend_always{};

        co_return std::make_tuple(awaitables.TryGetResult()...);
    }
}
