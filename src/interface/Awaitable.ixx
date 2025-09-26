export module EasyCoro.Awaitable;

import std;
import EasyCoro.ThreadPool;
import <cassert>;
import <cstddef>;

namespace EasyCoro {
    export template<typename Ret>
    class SimpleAwaitable;

    template<typename Fn>
    struct Finally {
        Fn func;

        Finally(Fn f) : func(std::move(f)) {}

        ~Finally() {
            func();
        }
    };

    template<typename Fn>
    struct AwaitToDo : std::suspend_always {
        Fn func;

        AwaitToDo(Fn f) : func(std::move(f)) {}

        void await_suspend(std::coroutine_handle<> handle) {
            if constexpr (std::is_invocable_v<Fn>) {
                func();
            } else {
                func(handle);
            }
        }
    };

    export struct Unit {};

    export class ExecutionContext;

    export thread_local ExecutionContext *CurrentExecutionContext = nullptr;

    export std::atomic_size_t g_AllocCount = 0;
    export std::atomic_size_t g_DeallocCount = 0;

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

        void Schedule(std::shared_ptr<void> handle) {
            m_ThreadPool->Enqueue([weak = std::weak_ptr(handle)] {
                if (auto shared = weak.lock()) {
                    std::coroutine_handle<> coroHandle = std::coroutine_handle<>::from_address(shared.get());
                    if (!coroHandle.done()) {
                        coroHandle.resume();
                    }
                }
            });
        }

        // Not used. scheduler should never hold strong reference to coroutine, because we don't know the state of it,
        // and we don't know when to drop it.
        void ScheduleStrong(std::shared_ptr<void> handle) {
            m_ThreadPool->Enqueue([handle = std::move(handle)] {
                std::coroutine_handle<> coroHandle = std::coroutine_handle<>::from_address(handle.get());
                if (!coroHandle.done()) {
                    coroHandle.resume();
                }
            });
        }

        void WaitAllTaskToFinish() {
            m_ThreadPool->WaitAllTaskToFinish();
        }

        template<typename Ret>
        Ret BlockOn(SimpleAwaitable<Ret> awaitable);

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


    template<typename Ret>
    SimpleAwaitable<Ret> MakeAwaitableFromPromise(
        auto &&handle
    );

    struct PromiseBase {
        std::weak_ptr<void> Self{};
        std::atomic_bool IsCancelled = false;
        std::atomic_bool NotCancellable = false;
        std::function<void()> OnFinished = nullptr;

        std::optional<std::shared_ptr<void>> DetachedSelf = std::nullopt;
    };

    template<typename Ret>
    struct PromiseType : PromiseBase {
        std::mutex ResultProtectMutex;
        std::variant<std::monostate, Ret, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        auto get_return_object() {
            ++g_AllocCount;
            return MakeAwaitableFromPromise<Ret>(std::coroutine_handle<PromiseType>::from_promise(*this));
        }

        std::suspend_always initial_suspend() {
            return {};
        }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(Ret value) {
            // Protect
            {
                std::scoped_lock lock(ResultProtectMutex);
                Result = std::move(value);
                if (OnFinished) {
                    OnFinished();
                }
            }
            DetachedSelf.reset();
        }

        void return_value(Unit) {
            if (!IsCancelled) {
                throw std::runtime_error("Cannot return void from non-void coroutine which is not canceled");
            }
            DetachedSelf.reset();
        }

        void unhandled_exception() {
            // Protect
            {
                std::scoped_lock lock(ResultProtectMutex);
                Result = std::current_exception();
                if (OnFinished) {
                    OnFinished();
                }
            }
            DetachedSelf.reset();
        }
    };

    template<>
    struct PromiseType<void> : PromiseBase {
        std::mutex ResultProtectMutex;
        std::variant<std::monostate, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        SimpleAwaitable<void> get_return_object();

        std::suspend_always initial_suspend() {
            return {};
        }

        constexpr static std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() {
            // Protect
            {
                std::scoped_lock lock(ResultProtectMutex);
                Result = std::monostate{};
                if (OnFinished)
                    OnFinished();
            }
            DetachedSelf.reset();
        }

        void unhandled_exception() {
            // Protect
            {
                std::scoped_lock lock(ResultProtectMutex);
                Result = std::current_exception();
                if (OnFinished) {
                    OnFinished();
                }
            }
            DetachedSelf.reset();
        }
    };


    template<typename Promise = PromiseBase> requires std::is_base_of_v<PromiseBase, Promise>
    auto PointerToHandleCast(
        const std::shared_ptr<void> &ptr) -> std::coroutine_handle<Promise>;

    template<typename T>
    const std::weak_ptr<void> &HandleToPointerCast(
        std::coroutine_handle<T> handle);

    template<typename Promise = PromiseBase> requires std::is_base_of_v<PromiseBase, Promise>
    auto
    HandleReinterpretCast(
        std::coroutine_handle<> handle) -> std::coroutine_handle<Promise>;


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
                        ++g_DeallocCount;
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

        void Reset() {
            m_MyHandlePtr.reset();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            if (auto self = GetMyHandle().promise().Self.lock()) {
                if (!GetMyHandle().promise().OnFinished) {
                    std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);

                    auto parent = HandleReinterpretCast<PromiseBase>(parentHandle);
                    if (parent.promise().NotCancellable) {
                        GetMyHandle().promise().OnFinished = [
                                    strongParent = HandleToPointerCast(parentHandle).lock()
                                ] mutable {
                                    GetCurrentExecutionContext().Schedule(std::exchange(strongParent, nullptr));
                                };
                    } else {
                        GetMyHandle().promise().OnFinished = [
                            parentHandle = HandleToPointerCast<void>(parentHandle)
                        ] {
                            if (auto copied = parentHandle.lock()) {
                                GetCurrentExecutionContext().Schedule(copied);
                            }
                        };
                    }
                }
                GetCurrentExecutionContext().Schedule(self);
            } else {
                __debugbreak();
            }
        }

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        SimpleAwaitable Cancellable(this SimpleAwaitable self, bool value) {
            self.GetMyHandle().promise().NotCancellable = !value;
            if (value == false) {
                self.GetMyHandle().promise().DetachedSelf = self.GetMyHandle().promise().Self.lock();
            }
            return self;
        }

        bool IsCancellable() const {
            return !GetMyHandle().promise().NotCancellable;
        }

        void SetOnFinished(auto &&callback) {
            std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);
            GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
        }

        void await_resume() {
            auto handle = GetMyHandle();

            std::lock_guard lock(handle.promise().ResultProtectMutex);
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
            // assert(handle.done());
            await_resume();
            return Unit{};
        }

        std::optional<Unit> TryGetResult() {
            if (std::holds_alternative<std::monostate>(GetMyHandle().promise().Result)) {
                return Unit{};
            }
            if (std::holds_alternative<std::exception_ptr>(GetMyHandle().promise().Result)) {
                std::rethrow_exception(std::get<std::exception_ptr>(GetMyHandle().promise().Result));
            }
            return std::nullopt;
        }

        auto Then(this SimpleAwaitable self, auto &&func) -> std::invoke_result_t<decltype(func)> {
            co_await self;
            co_return co_await func();
        }

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this SimpleAwaitable self, Duration duration) -> SimpleAwaitable {
            co_await AnyOf(
                self,
                Sleep(duration)
            );
        }
    };

    template<typename Ret>
    class InjectUnwraps {};

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    class InjectUnwraps<Ret> {
    public:
        auto UnwrapOrCancel(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
            auto value = co_await self;
            if (value) {
                co_return *value;
            }

            AwaitToDo awaiter([](std::coroutine_handle<> thisHandle) {
                auto myHandle = HandleReinterpretCast<PromiseType<Ret>>(thisHandle);
                myHandle.promise().Cancel();
                GetCurrentExecutionContext().Schedule(HandleToPointerCast<void>(thisHandle).lock());
            });

            co_await awaiter;

            co_return Unit{};
        }

        auto UnWrapOr(this SimpleAwaitable<Ret> self, auto provider) -> SimpleAwaitable<typename Ret::value_type> {
            auto value = co_await self;
            if (value) {
                co_return *value;
            }

            if constexpr (std::convertible_to<decltype(provider), typename Ret::value_type>) {
                co_return static_cast<Ret::value_type>(provider);
            } else if constexpr (std::is_same_v<std::invoke_result_t<decltype(provider)>, typename Ret::value_type>) {
                co_return provider();
            } else if constexpr (std::is_same_v<std::invoke_result_t<decltype(provider)>,
                SimpleAwaitable<typename Ret::value_type>>) {
                co_return co_await provider();
            } else {
                static_assert(
                    [] { return false; }(),
                    "Provider must be a callable returning the value type or an awaitable of the value type, "
                    "or the value type itself.");
            }
        }

        auto UnwrapOrDefault(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
            auto value = co_await self;
            if (value) {
                co_return *value;
            }
            co_return typename Ret::value_type{};
        }

        auto Unwrap(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
            co_return *co_await self;
        }

        auto UnwrapOrThrow(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
            auto value = co_await self;
            if (value) {
                co_return *value;
            }
            throw std::runtime_error(std::format("Attempted to unwrap an empty {} in SimpleAwaitable",
                                                 typeid(Ret).name()));
        }
    };

    template<typename Ret>
    class SimpleAwaitable : public InjectUnwraps<Ret> {
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
                        ++g_DeallocCount;
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
        ~SimpleAwaitable() = default;

        [[nodiscard]] std::coroutine_handle<> GetHandle() const {
            return GetMyHandle();
        }

        void Reset() {
            m_MyHandlePtr.reset();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            if (auto self = GetMyHandle().promise().Self.lock()) {
                if (!GetMyHandle().promise().OnFinished) {
                    std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);

                    auto parent = HandleReinterpretCast<PromiseBase>(parentHandle);
                    if (parent.promise().NotCancellable) {
                        GetMyHandle().promise().OnFinished = [
                                    strongParent = HandleToPointerCast(parentHandle).lock()
                                ] mutable {
                                    GetCurrentExecutionContext().Schedule(std::exchange(strongParent, nullptr));
                                };
                    } else {
                        GetMyHandle().promise().OnFinished = [
                            parentHandle = HandleToPointerCast<void>(parentHandle)
                        ] {
                            if (auto copied = parentHandle.lock()) {
                                GetCurrentExecutionContext().Schedule(copied);
                            }
                        };
                    }
                }
                GetCurrentExecutionContext().Schedule(self);
            } else {
                __debugbreak();
            }
        }

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        SimpleAwaitable Cancellable(this SimpleAwaitable self, bool value) {
            self.GetMyHandle().promise().NotCancellable = !value;
            if (value == false) {
                self.GetMyHandle().promise().DetachedSelf = self.GetMyHandle().promise().Self.lock();
            }
            return self;
        }

        bool IsCancellable() const {
            return !GetMyHandle().promise().NotCancellable;
        }

        void SetOnFinished(auto &&callback) {
            std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);
            GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
        }

        Ret await_resume() {
            auto handle = GetMyHandle();

            std::lock_guard lock(handle.promise().ResultProtectMutex);
            auto &variant = handle.promise().Result;
            switch (variant.index()) {
                case 0:
                    throw std::runtime_error("Coroutine did not return a value");
                case 1:
                    return std::move(std::get<Ret>(variant));
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
            // assert(handle.done());

            std::lock_guard lock(handle.promise().ResultProtectMutex);
            switch (handle.promise().Result.index()) {
                case 0:
                    throw std::runtime_error("Coroutine did not return a value");
                case 1:
                    return std::move(std::get<Ret>(handle.promise().Result));
                case 2:
                    std::rethrow_exception(std::get<std::exception_ptr>(handle.promise().Result));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }

        std::optional<Ret> TryGetResult() {
            if (std::holds_alternative<Ret>(GetMyHandle().promise().Result)) {
                return std::get<Ret>(std::move(GetMyHandle().promise().Result));
            }
            if (std::holds_alternative<std::exception_ptr>(GetMyHandle().promise().Result)) {
                std::rethrow_exception(std::get<std::exception_ptr>(GetMyHandle().promise().Result));
            }
            return std::nullopt;
        }

        auto Then(this SimpleAwaitable self, auto &&func) -> std::invoke_result_t<decltype(func), Ret> {
            auto value = co_await self;
            co_return co_await func(value);
        }

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this SimpleAwaitable self, Duration duration) -> SimpleAwaitable<std::optional<Ret>> {
            co_return std::get<0>(co_await AnyOf(
                self,
                Sleep(duration)
            ));
        }
    };


    template<typename Ret>
    Ret ExecutionContext::BlockOn(SimpleAwaitable<Ret> awaitable) {
        std::binary_semaphore semaphore(0);
        auto wrapper = [&awaitable
                ] mutable -> SimpleAwaitable<Ret> {
                    auto result = co_await awaitable;
                    co_return result;
                }();

        wrapper.SetOnFinished([&semaphore] {
            semaphore.release();
        });

        auto handlePtr = wrapper.GetHandlePtr();
        Schedule(handlePtr);

        semaphore.acquire();

        return wrapper.GetResult();
    }

    template<>
    void ExecutionContext::BlockOn(SimpleAwaitable<void> awaitable) {
        std::binary_semaphore semaphore(0);
        auto wrapper = [&awaitable
                ] mutable -> SimpleAwaitable<void> {
                    co_await awaitable;
                    co_return;
                }();

        wrapper.SetOnFinished([&semaphore] {
            semaphore.release();
        });

        auto handlePtr = wrapper.GetHandlePtr();
        Schedule(handlePtr);

        semaphore.acquire();

        wrapper.GetResult();
    }

    auto PromiseType<void>::get_return_object() -> SimpleAwaitable<void> {
        ++g_AllocCount;
        return MakeAwaitableFromPromise<void>(std::coroutine_handle<PromiseType>::from_promise(*this));
    }

    template<typename Ret>
    SimpleAwaitable<Ret> MakeAwaitableFromPromise(
        auto &&handle) {
        return SimpleAwaitable<Ret>{handle};
    }

    template<typename Promise> requires std::is_base_of_v<PromiseBase, Promise>
    auto PointerToHandleCast(
        const std::shared_ptr<void> &ptr) -> std::coroutine_handle<Promise> {
        assert(ptr);
        return std::coroutine_handle<Promise>::from_address(
            ptr.get());
    }

    template<typename T>
    const std::weak_ptr<void> &HandleToPointerCast(
        std::coroutine_handle<T> handle) {
        assert(handle);
        return HandleReinterpretCast(handle).promise().Self;
    }

    template<typename Promise> requires std::is_base_of_v<PromiseBase, Promise>
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
        Finally finally([&] {
            (awaitables.Reset(), ...);
        });

        auto parentHandle = HandleToPointerCast(co_await AwaitToGetThisHandle{}).lock();

        std::shared_ptr<std::function<void()>> onChildSuspend = std::make_shared<std::function<
            void()>>(

            [remaining = std::make_shared<std::atomic_size_t>(sizeof...(Awaitables)), parentHandle
            ]() mutable {
                if (auto thisRemaining = --*remaining; thisRemaining == 0) {
                    GetCurrentExecutionContext().Schedule(std::exchange(parentHandle, nullptr));
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);

        AwaitToDo awaiter([&] {
            (GetCurrentExecutionContext().Schedule(
                awaitables.GetHandlePtr()), ...);
        });

        co_await awaiter;

        auto result = std::make_tuple(std::move(awaitables.GetResult())...);
        co_return result;
    }

    export template<typename... Awaitables>
    SimpleAwaitable<std::tuple<std::optional<typename Awaitables::ReturnType>...>> AnyOf(Awaitables... awaitables) {
        Finally finally([&] {
            (awaitables.Reset(), ...);
        });

        auto parentHandle = HandleToPointerCast(co_await AwaitToGetThisHandle{}).lock();

        std::binary_semaphore startSemaphore(0);

        auto onChildSuspend = std::make_shared<std::function<
            void()>>(

            [Invoked = std::make_shared<std::atomic_bool>(false), parentHandle, &startSemaphore
            ]() mutable {
                bool expected = false;
                if (Invoked->compare_exchange_strong(expected, true)) {
                    startSemaphore.acquire();
                    GetCurrentExecutionContext().Schedule(std::exchange(parentHandle, nullptr));
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);


        AwaitToDo awaiter([&] {
            // (GetCurrentExecutionContext().Schedule(
            //     awaitables.GetHandlePtr()), ...);
            auto applier = [](auto &awaitable) {
                auto &context = GetCurrentExecutionContext();
                if (awaitable.IsCancellable()) {
                    context.Schedule(awaitable.GetHandlePtr());
                } else {
                    context.Schedule(awaitable.GetHandlePtr());
                }
            };

            (applier(awaitables), ...);

            startSemaphore.release(1);
        });

        co_await awaiter;

        auto result = std::make_tuple(std::move(awaitables.TryGetResult())...);
        co_return result;
    }


    export template<typename T>
    SimpleAwaitable<T> StartWith(T value) {
        co_return value;
    }

    export template<typename Func>
    auto AsynchronousOf(Func func) {
        return [func]<typename... Args>(
            Args &&... args) -> SimpleAwaitable<decltype(func(args...))> {
            co_return func(std::forward<Args>(args)...);
        };
    }

    SimpleAwaitable<void> Sleep(auto duration) {
        std::this_thread::sleep_for(duration);
        co_return;
    }

    export template<typename Func>
    auto TryUntilHasValue(Func func, std::chrono::milliseconds timeInterval = std::chrono::milliseconds(0)) {
        return [capturedFunc = std::move(func), capturedInterval = timeInterval]<typename... Args>(
            Args &&... args) mutable -> SimpleAwaitable<typename decltype(func(args...))::value_type> {
            auto func = std::move(capturedFunc);
            auto timeInterval = std::move(capturedInterval);
            while (true) {
                auto value = co_await [](
                    Func &func, Args &&... innerArgs) -> SimpleAwaitable<decltype(func(innerArgs...))> {
                            co_return func(std::forward<Args>(innerArgs)...);
                        }(func, std::forward<Args>(args)...);
                if (value) {
                    co_return value.value();
                }
                if (timeInterval.count() > 0)
                    std::this_thread::sleep_for(timeInterval);
            }
        };
    }

    export template<typename Func>
    auto Pull(Func func) {
        return func();
    }
}
