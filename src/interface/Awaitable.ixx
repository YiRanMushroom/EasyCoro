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
            : m_ThreadPool{
                SharedThreadPool::Create(threadCount, [this] {
                    CurrentExecutionContext = this;
                }, [] {
                    CurrentExecutionContext = nullptr;
                })
            } {}

        void Schedule(const std::shared_ptr<void> &handle) {
            m_ThreadPool->Enqueue([weak = std::weak_ptr(handle)] {
                if (auto shared = weak.lock()) {
                    std::coroutine_handle<> coroHandle = std::coroutine_handle<>::from_address(shared.get());
                    if (!coroHandle.done()) {
                        coroHandle.resume();
                    }
                }
            });
        }

        void WaitAllTaskToFinish() {
            m_ThreadPool->WaitAllTaskToFinish();
        }

        auto BlockOn(auto awaitable);

        void Detach() {
            m_ThreadPool->DetachAll();
        }

    private:
        std::shared_ptr<SharedThreadPool> m_ThreadPool{};
    };

    export ExecutionContext &GetCurrentExecutionContext() {
        if (!CurrentExecutionContext) {
            throw std::runtime_error("No current execution context set");
        }

        return *CurrentExecutionContext;
    }


    export template<typename Ret>
    class SimpleAwaitable;

    template<typename Ret>
    SimpleAwaitable<Ret> MakeAwaitableFromPromise(
        auto &&handle
    );

    template<typename Ret>
    struct PromiseType {
        std::weak_ptr<void> Self;

        std::atomic_bool IsCancelled = false;
        std::function<void()> OnFinished = nullptr;

        std::mutex ResultProtectMutex;
        std::variant<std::monostate, Ret, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        auto get_return_object() {
            return MakeAwaitableFromPromise<Ret>(std::coroutine_handle<PromiseType>::from_promise(*this));
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

    template<>
    struct PromiseType<void> {
        std::weak_ptr<void> Self;

        std::atomic_bool IsCancelled = false;
        std::function<void()> OnFinished = nullptr;

        std::mutex ResultProtectMutex;
        std::variant<std::monostate, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        auto get_return_object();

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


    template<typename Promise = PromiseType<void>>
    auto PointerToHandleCast(
        const std::shared_ptr<void> &ptr) -> std::coroutine_handle<Promise>;

    template<typename Ret = void, typename T>
    const std::weak_ptr<void> &HandleToPointerCast(
        std::coroutine_handle<T> handle);

    template<typename Promise = PromiseType<void>>
    auto
    HandleReinterpretCast(
        std::coroutine_handle<> handle) -> std::coroutine_handle<Promise>;

    export struct Unit {};

    SimpleAwaitable<void> Sleep(auto duration);

    export template<>
    class SimpleAwaitable<void> {
    public:
        using ReturnType = Unit;

        using PromiseType = PromiseType<void>;
        using promise_type = PromiseType;

        std::shared_ptr<void> m_MyHandlePtr;

        const std::shared_ptr<void> &GetHandlePtr() const {
            return m_MyHandlePtr;
        }

        SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandlePtr(
            std::shared_ptr<void>(
                handle.address(),
                [](void *ptr) {
                    if (ptr) {
                        std::coroutine_handle<PromiseType>::from_address(ptr).
                                destroy();
                    }
                })) {
            handle.promise().Self = m_MyHandlePtr;
        }

    protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return PointerToHandleCast<PromiseType>(m_MyHandlePtr);
        }

    public:
        [[nodiscard]] std::coroutine_handle<> GetHandle() const {
            return GetMyHandle();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            if (auto self = GetMyHandle().promise().Self.lock()) {
                if (!GetMyHandle().promise().OnFinished) {
                    GetMyHandle().promise().OnFinished = [
                                parentHandle = HandleToPointerCast<void>(parentHandle)
                            ]() mutable {
                                if (auto copied = parentHandle.lock()) {
                                    parentHandle.reset();
                                    GetCurrentExecutionContext().Schedule(copied);
                                }
                            };
                }
                GetCurrentExecutionContext().Schedule(self);
            }
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

        Unit GetResult() {
            auto handle = GetMyHandle();
            assert(handle.done());
            await_resume();
            return Unit{};
        }

        std::optional<Unit> TryGetResult() {
            if (std::holds_alternative<std::monostate>(GetMyHandle().promise().Result)) {
                return Unit{};
            }
            return std::nullopt;
        }

        auto Then(this SimpleAwaitable &&self, auto &&func) -> std::invoke_result_t<decltype(func)> {
            co_await std::move(self);
            co_return co_await func();
        }

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this SimpleAwaitable &&self, Duration duration) -> SimpleAwaitable {
            co_return std::get(co_await AnyOf(
                std::move(self),
                Sleep(duration)
            ));
        }
    };

    template<typename Ret>
    class SimpleAwaitable {
    public:
        using ReturnType = Ret;

        using PromiseType = PromiseType<Ret>;
        using promise_type = PromiseType;

        std::shared_ptr<void> m_MyHandlePtr;

        const std::shared_ptr<void> &GetHandlePtr() const {
            return m_MyHandlePtr;
        }

    public:
        SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandlePtr(
            std::shared_ptr<void>(
                handle.address(),
                [](void *ptr) {
                    if (ptr) {
                        std::coroutine_handle<PromiseType>::from_address(ptr).
                                destroy();
                    }
                })
        ) {
            handle.promise().Self = m_MyHandlePtr;
        }

    protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return PointerToHandleCast<PromiseType>(m_MyHandlePtr);
        }

    public:
        ~SimpleAwaitable() {}

        [[nodiscard]] std::coroutine_handle<> GetHandle() const {
            return GetMyHandle();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            if (!GetMyHandle().promise().OnFinished) {
                GetMyHandle().promise().OnFinished = [
                            parentHandle = HandleToPointerCast<void>(parentHandle)
                        ]() mutable {
                            if (auto copied = parentHandle.lock()) {
                                parentHandle.reset();
                                GetCurrentExecutionContext().Schedule(copied);
                            }
                        };
            }
            GetCurrentExecutionContext().Schedule(GetHandlePtr());
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
                return std::get<Ret>(std::move(GetMyHandle().promise().Result));
            }
            return std::nullopt;
        }

        auto Then(this SimpleAwaitable &&self, auto &&func) -> std::invoke_result_t<decltype(func), Ret> {
            Ret value = co_await std::move(self);
            co_return co_await func(value);
        }

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this SimpleAwaitable &&self, Duration duration) -> SimpleAwaitable<std::optional<Ret>> {
            co_return std::get<0>(co_await AnyOf(
                std::move(self),
                Sleep(duration)
            ));
        }
    };

    auto ExecutionContext::BlockOn(auto awaitable) {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
        auto wrapper = [&awaitable, &mutex, &cv, &finished
                ]() -> SimpleAwaitable<decltype(awaitable.GetResult())> {
                    auto result = co_await awaitable;
                    // Protect
                    {
                        std::scoped_lock lock(mutex);
                        finished = true;
                    }
                    cv.notify_one();
                    co_return result;
                }();
        auto handlePtr = wrapper.GetHandlePtr();
        auto handle = wrapper.GetHandle();
        Schedule(handlePtr);
        // Wait
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&finished] {
                return finished;
            });
        }
        return wrapper.GetResult();
    }

    auto PromiseType<void>::get_return_object() {
        return MakeAwaitableFromPromise<void>(std::coroutine_handle<PromiseType>::from_promise(*this));
    }

    template<typename Ret>
    SimpleAwaitable<Ret> MakeAwaitableFromPromise(
        auto &&handle) {
        return SimpleAwaitable<Ret>{handle};
    }

    template<typename Promise>
    auto PointerToHandleCast(
        const std::shared_ptr<void> &ptr) -> std::coroutine_handle<Promise> {
        assert(ptr);
        return std::coroutine_handle<Promise>::from_address(
            ptr.get());
    }

    template<typename Ret, typename T>
    const std::weak_ptr<void> &HandleToPointerCast(
        std::coroutine_handle<T> handle) {
        assert(handle);
        return HandleReinterpretCast<PromiseType<Ret>>(handle).promise().Self;
    }

    template<typename Promise>
    auto HandleReinterpretCast(
        std::coroutine_handle<> handle) -> std::coroutine_handle<Promise> {
        assert(handle);
        return std::coroutine_handle<Promise>::from_address(
            handle.address());
    }

    struct AwaitToGetThisHandle {
        std::coroutine_handle<> parentHandle;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle) {
            parentHandle = handle;
            GetCurrentExecutionContext().Schedule(HandleToPointerCast<void>(handle).lock());
        }

        std::coroutine_handle<> await_resume() const noexcept {
            return parentHandle;
        }
    };

    export template<typename... Awaitables>
    SimpleAwaitable<std::tuple<typename Awaitables::ReturnType...>> AllOf(Awaitables... awaitables) {
        auto parentHandle = HandleToPointerCast(co_await AwaitToGetThisHandle{});

        std::shared_ptr<std::function<void()>> onChildSuspend = std::make_shared<std::function<
            void()>>(

            [remaining = std::make_shared<std::atomic_size_t>(sizeof...(Awaitables)), parentHandle]() mutable {
                if (--*remaining == 0) {
                    if (auto parent = parentHandle.lock()) {
                        GetCurrentExecutionContext().Schedule(parent);
                    }
                } else {
                    std::cout << *remaining << " awaitables remaining..." << std::endl;
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);

        (GetCurrentExecutionContext().Schedule(
            awaitables.GetHandlePtr()), ...);

        co_await std::suspend_always{};

        co_return std::make_tuple(awaitables.GetResult()...);
    }

    export template<typename... Awaitables>
    SimpleAwaitable<std::tuple<std::optional<typename Awaitables::ReturnType>...>> AnyOf(Awaitables... awaitables) {
        auto parentHandle = HandleToPointerCast(co_await AwaitToGetThisHandle{});

        std::shared_ptr<std::function<void()>> onChildSuspend = std::make_shared<std::function<
            void()>>(

            [Invoked = std::make_shared<std::atomic_bool>(false), parentHandle]() mutable {
                if (auto parent = parentHandle.lock()) {
                    bool expected = false;
                    if (Invoked->compare_exchange_strong(expected, true)) {
                        GetCurrentExecutionContext().Schedule(parent);
                    }
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);

        (GetCurrentExecutionContext().Schedule(
            awaitables.GetHandlePtr()), ...);

        co_await std::suspend_always{};

        co_return std::make_tuple(awaitables.TryGetResult()...);
    }


    export template<typename T>
    SimpleAwaitable<T> StartWith(T value) {
        co_return value;
    }

    export template<typename Func>
    auto AsynchronousOf(Func func) {
        return [func = std::forward<Func>(func)]<typename... Args>(
            Args &&... args) -> SimpleAwaitable<decltype(func(args...))> {
            co_return func(std::forward<Args>(args)...);
        };
    }

    SimpleAwaitable<void> Sleep(auto duration) {
        std::this_thread::sleep_for(duration);
        co_return;
    }

    export template<typename Func>
    auto TryUntilHasValue(Func func, std::chrono::milliseconds time_interval = std::chrono::milliseconds(100)) {
        return [func = std::forward<Func>(func), time_interval]<typename... Args>(
            Args &&... args) -> SimpleAwaitable<typename decltype(func(args...))::value_type> {
            while (true) {
                auto value = co_await AsynchronousOf(func)(std::forward<Args>(args)...);
                if (value) {
                    co_return value.value();
                }
                std::this_thread::sleep_for(time_interval);
            }
        };
    }

    export template<typename Func>
    auto Pull(Func func) {
        return func();
    }
}
