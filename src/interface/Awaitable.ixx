export module EasyCoro.Awaitable;

import std;
import EasyCoro.ThreadPool;
import <cassert>;
import <cstddef>;

namespace EasyCoro {
    export template<typename Ret>
    class SimpleAwaitable;

    export template<typename Fn>
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
            func(handle);
        }
    };

    template<typename Fn>
    struct AwaitToDoImmediate : std::suspend_never {
        Fn func;

        AwaitToDoImmediate(Fn f) : func(std::move(f)) {}
    };

    export struct Unit {};

    export class ExecutionContext;

    export std::atomic_size_t g_AllocCount = 0;
    export std::atomic_size_t g_DeallocCount = 0;

    export struct UseStandardExecutionContext {};

    class ExecutionContext {
    public:
        ExecutionContext(size_t threadCount = std::jthread::hardware_concurrency() * 2)
            : m_ThreadPool{
                SharedThreadPool::Create(threadCount, [this] {}, [] {})
            } {}

        ExecutionContext(UseStandardExecutionContext) : m_ThreadPool{
            StandardThreadPool::Create()
        } {}

        void Schedule(std::shared_ptr<void> handle);

        // Not used. scheduler should never hold strong reference to coroutine, because we don't know the state of it,
        // and we don't know when to drop it.
        void ScheduleStrong(std::shared_ptr<void> handle);

        template<typename Ret>
        Ret BlockOn(SimpleAwaitable<Ret> awaitable);

    private:
        std::shared_ptr<IThreadPool> m_ThreadPool{};
    };

    void ExecutionContext::Schedule(std::shared_ptr<void> handle) {
        // std::cout << std::format("Scheduling {}\n", (void *) handle.get()) << std::flush;
        m_ThreadPool->Enqueue([weak = std::weak_ptr(handle)] {
            // std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (auto shared = weak.lock()) {
                std::coroutine_handle<> coroHandle = std::coroutine_handle<>::from_address(shared.get());
                if (!coroHandle.done()) {
                    coroHandle.resume();
                }
            }
        });
    }

    void ExecutionContext::ScheduleStrong(std::shared_ptr<void> handle) {
        m_ThreadPool->Enqueue([handle = std::move(handle)] {
            std::coroutine_handle<> coroHandle = std::coroutine_handle<>::from_address(handle.get());
            if (!coroHandle.done()) {
                coroHandle.resume();
            }
        });
    }

    struct PromiseBase {
        std::weak_ptr<void> Self{};
        std::atomic_bool IsCancelled = false;
        std::atomic_bool NotCancellable = false;
        // std::atomic_bool InitialSuspended = true;
        std::function<void()> OnFinished = nullptr;
        ExecutionContext *Context = nullptr;

        std::optional<std::shared_ptr<void>> DetachedSelf = std::nullopt;

        void Schedule() const;
    };

    void PromiseBase::Schedule() const {
        if (auto self = Self.lock()) {
            if (Context) {
                Context->Schedule(self);
            } else {
                std::cout << "No execution context set in coroutine promise\n" << std::flush;
                __debugbreak();
                throw std::runtime_error("No execution context set in coroutine promise");
            }
        }
    }

    template<typename Promise = PromiseBase> requires std::is_base_of_v<PromiseBase, Promise>
    auto PointerToHandleCast(
        std::shared_ptr<void> ptr) -> std::coroutine_handle<Promise>;

    template<typename T>
    const std::weak_ptr<void> &HandleToPointerCast(
        std::coroutine_handle<T> handle);

    template<typename Promise = PromiseBase> requires std::is_base_of_v<PromiseBase, Promise>
    auto
    HandleReinterpretCast(
        std::coroutine_handle<> handle) -> std::coroutine_handle<Promise>;

    template<typename Ret>
    struct PromiseType : PromiseBase {
        std::mutex ResultProtectMutex;
        std::variant<std::monostate, Ret, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        SimpleAwaitable<Ret> get_return_object();

        auto initial_suspend() noexcept {
            return std::suspend_always{};
        }

        static std::suspend_always final_suspend() noexcept {
            return {};
        }

        template<typename T>
        decltype(auto) await_transform(T &&awaiter) {
            return std::forward<decltype(awaiter)>(awaiter);
        }

        template<typename T>
        SimpleAwaitable<T> await_transform(SimpleAwaitable<T> awaitable);

        template<class Fn>
        std::suspend_never await_transform(AwaitToDoImmediate<Fn> &&awaiter);

        void return_value(Ret value);

        void return_value(Unit);

        void unhandled_exception();
    };

    template<>
    struct PromiseType<void> : PromiseBase {
        std::mutex ResultProtectMutex;
        std::variant<std::monostate, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        SimpleAwaitable<void> get_return_object();

        auto initial_suspend() {
            return std::suspend_always{};
        }

        constexpr static std::suspend_always final_suspend() noexcept { return {}; }

        template<typename T>
        decltype(auto) await_transform(T &&awaiter) {
            return std::forward<decltype(awaiter)>(awaiter);
        }

        template<typename T>
        SimpleAwaitable<T> await_transform(SimpleAwaitable<T> awaitable);

        template<class Fn>
        std::suspend_never await_transform(AwaitToDoImmediate<Fn> &&awaiter);


        void return_void();

        void unhandled_exception();
    };

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

        SimpleAwaitable(std::coroutine_handle<PromiseType> handle);

        // protected:
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

        [[nodiscard]] ExecutionContext *GetContext() const;

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        SimpleAwaitable Cancellable(this SimpleAwaitable self, bool value);

        bool IsCancellable() const {
            return !GetMyHandle().promise().NotCancellable;
        }

        void SetOnFinished(auto &&callback) {
            std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);
            GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
        }

        void SetContext(ExecutionContext *context) {
            GetMyHandle().promise().Context = context;
        }

        void await_resume();

        void await_suspend(std::coroutine_handle<> parentHandle) {
            GetMyHandle().promise().Schedule();
        }

        bool IsCancelled() const {
            return GetMyHandle().promise().IsCancelled;
        }

        Unit GetResult();

        std::optional<Unit> TryGetResult();

        template<typename Func>
        inline auto Then(this SimpleAwaitable self, Func &&func) -> std::invoke_result_t<Func>;

        template<typename Duration = std::chrono::milliseconds>
        inline auto WithTimeOut(this SimpleAwaitable self, Duration duration) -> SimpleAwaitable;

        // SimpleAwaitable CaptureEnabled(this SimpleAwaitable self);
    };

    template<typename Ret>
    class InjectUnwraps {};

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    class InjectUnwraps<Ret> {
    public:
        auto UnwrapOrCancel(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type>;

        template<typename Provider>
        auto UnWrapOr(this SimpleAwaitable<Ret> self, Provider provider) -> SimpleAwaitable<typename Ret::value_type>;

        auto UnwrapOrDefault(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type>;

        auto Unwrap(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type>;

        auto UnwrapOrThrow(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type>;
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
        SimpleAwaitable(std::coroutine_handle<PromiseType> handle);

        // protected:
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
            GetMyHandle().promise().Schedule();
        }

        ExecutionContext *GetContext() const {
            return GetMyHandle().promise().Context;
        }

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        SimpleAwaitable Cancellable(this SimpleAwaitable self, bool value);

        bool IsCancellable() const {
            return !GetMyHandle().promise().NotCancellable;
        }

        void SetOnFinished(auto &&callback);

        void SetContext(ExecutionContext *context) {
            GetMyHandle().promise().Context = context;
        }

        Ret await_resume();

        bool IsCancelled() const {
            return GetMyHandle().promise().IsCancelled;
        }

        Ret GetResult();

        std::optional<Ret> TryGetResult();

        template<typename Func>
        auto Then(this SimpleAwaitable self, Func &&func) -> std::invoke_result_t<Func, Ret>;

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this SimpleAwaitable self, Duration duration) -> SimpleAwaitable<std::optional<Ret>>;

        // SimpleAwaitable CaptureEnabled(this SimpleAwaitable self);
    };

    template<typename Ret>
    Ret ExecutionContext::BlockOn(SimpleAwaitable<Ret> awaitable) {
        std::binary_semaphore semaphore(0);
        auto wrapper = [awaitable]<typename Self>(this Self self) mutable -> SimpleAwaitable<Ret> {
            auto copied = self.awaitable; // Make a copy to avoid dangling reference
            auto result = co_await std::move(copied);
            co_return result;
        }();

        wrapper.SetContext(this);
        wrapper.SetOnFinished([&semaphore] {
            semaphore.release();
        });

        auto handlePtr = wrapper.GetHandlePtr();

        wrapper.GetMyHandle().promise().Schedule();

        semaphore.acquire();

        return wrapper.GetResult();
    }


    template<>
    void ExecutionContext::BlockOn(SimpleAwaitable<void> awaitable) {
        std::binary_semaphore semaphore(0);
        auto wrapper = [awaitable]<typename Self>(this Self self)mutable -> SimpleAwaitable<void> {
            co_await self.awaitable;
            co_return;
        }();

        wrapper.SetContext(this);
        wrapper.SetOnFinished([&semaphore] {
            semaphore.release();
        });

        auto handlePtr = wrapper.GetHandlePtr();

        wrapper.GetMyHandle().promise().Schedule();

        semaphore.acquire();

        wrapper.GetResult();
    }

    template<typename Promise> requires std::is_base_of_v<PromiseBase, Promise>
    auto PointerToHandleCast(
        std::shared_ptr<void> ptr) -> std::coroutine_handle<Promise> {
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

    template<typename Ret>
    SimpleAwaitable<Ret> PromiseType<Ret>::get_return_object() {
        ++g_AllocCount;
        return SimpleAwaitable<Ret>(std::coroutine_handle<PromiseType>::from_promise(*this));
    }

    template<typename Ret>
    template<typename T>
    SimpleAwaitable<T> PromiseType<Ret>::await_transform(SimpleAwaitable<T> awaitable) {
        awaitable.SetContext(Context);
        auto parentHandle = Self.lock();
        assert(parentHandle);
        if (NotCancellable) {
            awaitable.SetOnFinished([strongParent = parentHandle]mutable {
                PointerToHandleCast<PromiseBase>(std::exchange(strongParent, nullptr)).promise().
                        Schedule();
            });
        } else {
            awaitable.SetOnFinished([parentHandle = Self]mutable {
                if (auto copied = parentHandle.lock()) {
                    PointerToHandleCast<PromiseBase>(copied).promise().Schedule();
                }
            });
        }

        return awaitable;
    }

    template<typename Ret>
    template<typename Fn>
    std::suspend_never PromiseType<Ret>::await_transform(AwaitToDoImmediate<Fn> &&awaiter) {
        awaiter.func(std::coroutine_handle<PromiseType>::from_promise(*this));
        return {};
    }

    template<typename Ret>
    void PromiseType<Ret>::return_value(Ret value) {
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

    template<typename Ret>
    void PromiseType<Ret>::return_value(Unit) {
        if (!IsCancelled) {
            throw std::runtime_error("Cannot return void from non-void coroutine which is not canceled");
        }
        DetachedSelf.reset();
    }

    template<typename Ret>
    void PromiseType<Ret>::unhandled_exception() {
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

    SimpleAwaitable<void> PromiseType<void>::get_return_object() {
        ++g_AllocCount;
        return SimpleAwaitable(std::coroutine_handle<PromiseType>::from_promise(*this));
    }

    template<typename T>
    SimpleAwaitable<T> PromiseType<void>::await_transform(SimpleAwaitable<T> awaitable) {
        awaitable.SetContext(Context);
        auto parentHandle = Self.lock();
        assert(parentHandle);
        if (NotCancellable) {
            awaitable.SetOnFinished([strongParent = parentHandle] mutable {
                PointerToHandleCast<PromiseBase>(std::exchange(strongParent, nullptr)).promise().
                        Schedule();
            });
        } else {
            awaitable.SetOnFinished([parentHandle = Self] mutable {
                if (auto copied = parentHandle.lock()) {
                    PointerToHandleCast<PromiseBase>(copied).promise().Schedule();
                }
            });
        }

        return awaitable;
    }

    template<typename Fn>
    std::suspend_never PromiseType<void>::await_transform(AwaitToDoImmediate<Fn> &&awaiter) {
        awaiter.func(std::coroutine_handle<PromiseType>::from_promise(*this));
        return {};
    }

    void PromiseType<void>::return_void() {
        // Protect
        {
            std::scoped_lock lock(ResultProtectMutex);
            Result = std::monostate{};
            if (OnFinished)
                OnFinished();
        }
        DetachedSelf.reset();
    }

    void PromiseType<void>::unhandled_exception() {
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

    SimpleAwaitable<void>::SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandlePtr(
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

    ExecutionContext *SimpleAwaitable<void>::GetContext() const {
        return GetMyHandle().promise().Context;
    }

    SimpleAwaitable<void> SimpleAwaitable<void>::Cancellable(this SimpleAwaitable<void> self, bool value) {
        self.GetMyHandle().promise().NotCancellable = !value;
        if (value == false) {
            self.GetMyHandle().promise().DetachedSelf = self.GetMyHandle().promise().Self.lock();
        }
        return self;
    }

    void SimpleAwaitable<void>::await_resume() {
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

    Unit SimpleAwaitable<void>::GetResult() {
        auto handle = GetMyHandle();
        // assert(handle.done());
        await_resume();
        return Unit{};
    }

    std::optional<Unit> SimpleAwaitable<void>::TryGetResult() {
        if (std::holds_alternative<std::monostate>(GetMyHandle().promise().Result)) {
            return Unit{};
        }
        if (std::holds_alternative<std::exception_ptr>(GetMyHandle().promise().Result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(GetMyHandle().promise().Result));
        }
        return std::nullopt;
    }

    export template<typename... Tps>
    SimpleAwaitable<std::tuple<typename SimpleAwaitable<Tps>::ReturnType...>>
    AllOf(SimpleAwaitable<Tps>... awaitables) {
        std::shared_ptr<void> parentHandle{};
        co_await AwaitToDoImmediate{
            [&parentHandle](std::coroutine_handle<> handle) {
                parentHandle = HandleToPointerCast(handle).lock();
            }
        };

        // std::cout << std::format("My handle is {}\n", (void *) parentHandle.get()) << std::flush;

        std::atomic_size_t remaining = sizeof...(awaitables);
        std::binary_semaphore semaphore(0);

        std::shared_ptr<std::function<void()>> onChildSuspend = std::make_shared<std::function<
            void()>>(
            [&remaining, parentHandle, &semaphore]() mutable {
                if (auto thisRemaining = --remaining; thisRemaining == 0) {
                    semaphore.acquire();
                    // GetCurrentExecutionContext().Schedule(std::exchange(parentHandle, nullptr));
                    // std::cout << std::format("Resuming {}", (void *) parentHandle.get()) << std::endl;
                    PointerToHandleCast<PromiseBase>(std::exchange(parentHandle, nullptr)).promise().Schedule();
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend]() mutable {
            (*onChildSuspend)();
        })), ...);

        ExecutionContext *context = PointerToHandleCast<PromiseBase>(parentHandle).promise().Context;
        ((awaitables.SetContext(context)), ...);

        AwaitToDo awaiter([&](std::coroutine_handle<>) {
            (awaitables.GetMyHandle().promise().Schedule(), ...);

            semaphore.release(1);
        });

        co_await awaiter;

        auto result = std::make_tuple(std::move(awaitables.GetResult())...);

        (awaitables.Reset(), ...);

        co_return result;
    }

    export template<typename... Tps>
    SimpleAwaitable<std::tuple<std::optional<typename SimpleAwaitable<Tps>::ReturnType>...>> AnyOf(
        SimpleAwaitable<Tps>... awaitables) {
        std::shared_ptr<void> parentHandle{};

        co_await AwaitToDoImmediate{
            [&parentHandle](std::coroutine_handle<> handle) {
                parentHandle = HandleToPointerCast(handle).lock();
            }
        };

        std::atomic_bool invoked = false;
        std::binary_semaphore semaphore(0);

        auto onChildSuspend = std::make_shared<std::function<
            void()>>(
            [&invoked, parentHandle, &semaphore] mutable {
                bool expected = false;
                if (invoked.compare_exchange_strong(expected, true)) {
                    semaphore.acquire();
                    PointerToHandleCast<PromiseBase>(parentHandle).promise().Schedule();
                }
            }
        );

        ((awaitables.SetOnFinished([onChildSuspend] mutable {
            (*onChildSuspend)();
        })), ...);

        ExecutionContext *context = PointerToHandleCast<PromiseBase>(parentHandle).promise().Context;
        ((awaitables.SetContext(context)), ...);

        AwaitToDo awaiter([&](std::coroutine_handle<>) {
            auto applier = [](auto &awaitable) {
                if (awaitable.IsCancellable()) {
                    awaitable.GetMyHandle().promise().Schedule();
                } else {
                    awaitable.GetMyHandle().promise().Schedule();
                }
            };

            (applier(awaitables), ...);

            semaphore.release(1);
        });

        co_await awaiter;

        auto result = std::make_tuple(std::move(awaitables.TryGetResult())...);

        (awaitables.Reset(), ...);

        co_return result;
    }


    export template<typename T>
    SimpleAwaitable<T> StartWith(T value) {
        co_return value;
    }

    export template<typename Func>
    auto AsynchronousOf(Func func) {
        return [func = std::move(func)]<typename Self, typename... Args>(
            this Self self,
            Args &&... args) mutable -> SimpleAwaitable<decltype(func(args...))> {
                co_return self.func(std::forward<Args>(args)...);
        };
    }


    SimpleAwaitable<void> Sleep(auto duration) {
        std::this_thread::sleep_for(duration);
        co_return;
    }

    template<typename Func>
    auto SimpleAwaitable<void>::Then(this SimpleAwaitable self,
                                     Func &&func) -> std::invoke_result_t<Func> {
        co_await self;
        co_return co_await func();
    }

    template<typename Duration>
    auto SimpleAwaitable<void>::
    WithTimeOut(this SimpleAwaitable self,
                Duration duration) -> SimpleAwaitable {
        co_await AnyOf(
            self,
            Sleep(duration)
        );
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>; {
            *ret
        } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::
    UnwrapOrCancel(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
        auto value = co_await self;
        if (value) {
            co_return *value;
        }

        AwaitToDo awaiter([](std::coroutine_handle<> thisHandle) {
            auto myHandle = HandleReinterpretCast<PromiseType<Ret>>(thisHandle);
            myHandle.promise().Cancel();
            HandleReinterpretCast<PromiseBase>(thisHandle).promise().Schedule();
        });

        co_await awaiter;

        co_return Unit{};
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>; {
            *ret
        } -> std::convertible_to<typename Ret::value_type>;
    }
    template<typename Provider>
    auto InjectUnwraps<Ret>::UnWrapOr(this SimpleAwaitable<Ret> self,
                                      Provider provider) -> SimpleAwaitable<typename Ret::value_type> {
        auto value = co_await self;
        if (value) {
            co_return *value;
        }

        if constexpr (std::convertible_to<Provider, typename Ret::value_type>) {
            co_return static_cast<Ret::value_type>(provider);
        } else if constexpr (std::convertible_to<std::invoke_result_t<Provider>, typename
            Ret::value_type>) {
            co_return provider();
        } else if constexpr (std::is_same_v<std::invoke_result_t<Provider>,
            SimpleAwaitable<typename Ret::value_type>>) {
            co_return co_await provider();
        } else {
            static_assert(
                [] { return false; }(),
                "Provider must be a callable returning the value type or an awaitable of the value type, "
                "or the value type itself.");
        }
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>; {
            *ret
        } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::UnwrapOrDefault(
        this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
        auto value = co_await self;
        if (value) {
            co_return *value;
        }
        co_return typename Ret::value_type{};
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>; {
            *ret
        } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::Unwrap(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
        co_return *co_await self;
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>; {
            *ret
        } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::
    UnwrapOrThrow(this SimpleAwaitable<Ret> self) -> SimpleAwaitable<typename Ret::value_type> {
        auto value = co_await self;
        if (value) {
            co_return *value;
        }
        throw std::runtime_error(std::format("Attempted to unwrap an empty {} in SimpleAwaitable",
                                             typeid(Ret).name()));
    }

    template<typename Ret>
    SimpleAwaitable<Ret>::SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandlePtr(
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

    template<typename Ret>
    SimpleAwaitable<Ret> SimpleAwaitable<Ret>::Cancellable(this SimpleAwaitable self, bool value) {
        self.GetMyHandle().promise().NotCancellable = !value;
        if (value == false) {
            self.GetMyHandle().promise().DetachedSelf = self.GetMyHandle().promise().Self.lock();
        } else {
            self.GetMyHandle().promise().DetachedSelf.reset();
        }
        return self;
    }

    template<typename Ret>
    void SimpleAwaitable<Ret>::SetOnFinished(auto &&callback) {
        std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);
        GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
    }

    template<typename Ret>
    Ret SimpleAwaitable<Ret>::await_resume() {
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

    template<typename Ret>
    Ret SimpleAwaitable<Ret>::GetResult() {
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

    template<typename Ret>
    std::optional<Ret> SimpleAwaitable<Ret>::TryGetResult() {
        if (std::holds_alternative<Ret>(GetMyHandle().promise().Result)) {
            return std::get<Ret>(std::move(GetMyHandle().promise().Result));
        }
        if (std::holds_alternative<std::exception_ptr>(GetMyHandle().promise().Result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(GetMyHandle().promise().Result));
        }
        return std::nullopt;
    }

    template<typename Ret>
    template<typename Func>
    auto SimpleAwaitable<Ret>::Then(this SimpleAwaitable self, Func &&func) -> std::invoke_result_t<Func, Ret> {
        auto value = co_await self;
        co_return co_await func(value);
    }

    template<typename Ret>
    template<typename Duration>
    auto SimpleAwaitable<Ret>::WithTimeOut(this SimpleAwaitable self,
                                           Duration duration) -> SimpleAwaitable<std::optional<Ret>> {
        co_return std::get<0>(co_await AnyOf(
            self,
            Sleep(duration)
        ));
    }

    export template<typename Func>
    auto TryUntilHasValue(Func func, std::chrono::milliseconds timeInterval = std::chrono::milliseconds(0)) {
        return [func = std::move(func), timeInterval]<typename Self, typename... Args>(
            this Self self,
            Args &&... args) mutable -> SimpleAwaitable<typename std::invoke_result_t<Func, Args...>::value_type> {
            while (true) {
                auto value = co_await [func = self.func]<typename InnerSelf>(this InnerSelf self, Args &&... args)
                    -> SimpleAwaitable<std::invoke_result_t<Func, Args...>> {
                            co_return self.func(std::forward<Args>(args)...);
                        }(std::forward<Args>(args)...);
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
