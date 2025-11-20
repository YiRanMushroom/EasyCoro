export module EasyCoro.Awaitable;

import std;
import EasyCoro.ThreadPool;
import <cassert>;

namespace EasyCoro {
    struct IntoType {
    };

    export constexpr IntoType Into{};

    export template<typename Ret>
    class Awaitable;

    template<typename T>
    struct IsAwaitableImpl {
        static constexpr bool value = false;
    };

    template<typename Ret>
    struct IsAwaitableImpl<Awaitable<Ret>> {
        static constexpr bool value = true;
    };

    export template<typename T>
    concept IsAwaitable = IsAwaitableImpl<T>::value;

    export template<typename T>
    concept CanInto = requires(T t) {
        std::move(t) >> Into;
    };

    template<typename Fn>
    struct AwaitToDo : std::suspend_always {
        Fn func;

        AwaitToDo(const AwaitToDo &) = delete;

        AwaitToDo &operator=(const AwaitToDo &) = delete;

        AwaitToDo(Fn f) : func(std::move(f)) {
        }

        void await_suspend(std::coroutine_handle<> handle) {
            func(handle);
        }
    };

    template<typename Fn>
    struct AwaitToDoImmediate : std::suspend_never {
        Fn func;

        AwaitToDoImmediate(const AwaitToDoImmediate &) = delete;

        AwaitToDoImmediate &operator=(const AwaitToDoImmediate &) = delete;

        AwaitToDoImmediate(Fn f) : func(std::move(f)) {
        }
    };

    export class ExecutionContext;

    export std::atomic_size_t g_AllocCount = 0;
    export std::atomic_size_t g_DeallocCount = 0;

    export struct UseStandardExecutionContext {
    };

    class ExecutionContext {
    public:
        ExecutionContext(size_t threadCount = std::jthread::hardware_concurrency() * 2)
            : m_ThreadPool{
                SharedThreadPool::Create(threadCount, [this] {
                }, [] {
                })
            } {
        }

        ExecutionContext(UseStandardExecutionContext) : m_ThreadPool{
            StandardThreadPool::Create()
        } {
        }

        void Schedule(std::shared_ptr<void> handle);

        // Not used. scheduler should never hold strong reference to coroutine, because we don't know the state of it,
        // and we don't know when to drop it.
        void ScheduleStrong(std::shared_ptr<void> handle);

        template<typename Ret>
        Ret BlockOn(Awaitable<Ret> awaitable);

        template<typename T> requires (CanInto<T>)
        auto Async(T canInto);

        template<typename T> requires (CanInto<T> && !IsAwaitable<T>)
        void BlockOn(T canIntoType);

        void Join() {
            m_ThreadPool->Join();
        }

    private:
        std::shared_ptr<IThreadPool> m_ThreadPool{};
    };

    void ExecutionContext::Schedule(std::shared_ptr<void> handle) {
        m_ThreadPool->Enqueue([weak = std::weak_ptr(handle)] {
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

    template<typename... Tps>
    struct AllOfType;

    template<typename... Tps>
    struct AnyOfType;

    struct PromiseBase {
        std::weak_ptr<void> Self{};
        std::atomic_bool IsCancelled = false;
        std::atomic_bool NotCancellable = false;
        std::function<void()> OnFinished = nullptr;
        ExecutionContext *Context = nullptr;

        std::optional<std::shared_ptr<void>> DetachedSelf = std::nullopt;

        void Schedule() const;

        template<typename T> requires (!IsAwaitable<std::remove_cvref_t<T>>)
        decltype(auto) await_transform(T &&awaiter) {
            return std::forward<decltype(awaiter)>(awaiter);
        }

        template<typename... Tps>
        auto await_transform(AllOfType<Tps...>);

        template<typename... Tps>
        auto await_transform(AnyOfType<Tps...>);

        template<typename T>
        const Awaitable<T> &await_transform(const Awaitable<T> &awaitable);

        template<typename T>
        Awaitable<T> &&await_transform(Awaitable<T> &&awaitable);

        template<class Fn>
        std::suspend_never await_transform(AwaitToDoImmediate<Fn> &&awaiter);
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
        std::mutex ResultProtectMutex{};
        std::variant<std::monostate, Ret, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        Awaitable<Ret> get_return_object();

        auto initial_suspend() noexcept {
            return std::suspend_always{};
        }

        static std::suspend_always final_suspend() noexcept {
            return {};
        }

        // co_return can have implicit move
        template<std::convertible_to<Ret> T>
        void return_value(T &&value);

        void return_value(void);

        template<std::convertible_to<Ret> T>
        std::suspend_always yield_value(T &&value);

        void unhandled_exception();

        Ret GetResultValue() {
            std::lock_guard lock(ResultProtectMutex);
            auto &variant = Result;
            switch (variant.index()) {
                case 1:
                    return std::get<Ret>(std::move(variant));
                case 2:
                    std::rethrow_exception(std::get<std::exception_ptr>(std::move(variant)));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }

        std::optional<Ret> TryGetResultValue() {
            std::lock_guard lock(ResultProtectMutex);
            auto &variant = Result;
            switch (variant.index()) {
                case 1:
                    return std::get<Ret>(std::move(variant));
                case 2:
                    std::rethrow_exception(std::get<std::exception_ptr>(std::move(variant)));
                default:
                    return std::nullopt;
            }
        }
    };

    template<typename Ret>
    struct PromiseType<Ret &> : PromiseBase {
        std::mutex ResultProtectMutex{};
        std::variant<std::monostate, Ret *, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        Awaitable<Ret &> get_return_object();

        auto initial_suspend() noexcept {
            return std::suspend_always{};
        }

        static std::suspend_always final_suspend() noexcept {
            return {};
        }

        // co_return can have implicit move
        template<typename T> requires std::convertible_to<Ret &, T &>
        void return_value(T &value);

        void return_value(void);

        template<typename T> requires std::convertible_to<Ret &, T &>
        std::suspend_always yield_value(T &value);

        void unhandled_exception();

        Ret &GetResultValue() {
            std::lock_guard lock(ResultProtectMutex);
            auto &variant = Result;
            switch (variant.index()) {
                case 1:
                    return *std::get<Ret *>(std::move(variant));
                case 2:
                    std::rethrow_exception(std::get<std::exception_ptr>(std::move(variant)));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }

        std::optional<std::reference_wrapper<Ret>> TryGetResultValue() {
            std::lock_guard lock(ResultProtectMutex);
            auto &variant = Result;
            switch (variant.index()) {
                case 1:
                    return *std::get<Ret *>(std::move(variant));
                case 2:
                    std::rethrow_exception(std::get<std::exception_ptr>(std::move(variant)));
                default:
                    return nullptr;
            }
        }
    };

    template<typename Ret>
    struct PromiseType<Ret &&> {
        static_assert(false, "Returning an rvalue reference is extremely dangerous, return a value instead");
    };

    template<>
    struct PromiseType<void> : PromiseBase {
        std::mutex ResultProtectMutex;
        std::variant<std::monostate, std::exception_ptr> Result{std::monostate{}};

        void Cancel() {
            IsCancelled = true;
        }

        Awaitable<void> get_return_object();

        auto initial_suspend() {
            return std::suspend_always{};
        }

        constexpr static std::suspend_always final_suspend() noexcept { return {}; }

        void return_void();

        void unhandled_exception();

        std::monostate GetResultValue() {
            std::lock_guard lock(ResultProtectMutex);
            auto &variant = Result;
            switch (variant.index()) {
                case 0:
                    return {};
                case 1:
                    std::rethrow_exception(std::get<std::exception_ptr>(variant));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }

        std::optional<std::monostate> TryGetResultValue() {
            std::lock_guard lock(ResultProtectMutex);
            auto &variant = Result;
            switch (variant.index()) {
                case 0:
                    return std::monostate{};
                case 1:
                    std::rethrow_exception(std::get<std::exception_ptr>(variant));
                default:
                    throw std::runtime_error("Invalid state in coroutine result");
            }
        }
    };

    Awaitable<void> Sleep(auto duration);

    namespace TypeTraits {
        template<typename>
        struct FunctionTraitInfo {
            template<size_t Idx, typename Default>
            using ArgumentTypeOrDefault = Default;

            template<typename Default>
            using ReturnTypeOrDefault = Default;
        };

        template<typename Fn> requires requires { &Fn::operator(); }
        struct FunctionTraitInfo<Fn> : FunctionTraitInfo<decltype(&Fn::operator())> {
        };

        template<typename Fn> requires std::is_function_v<Fn>
        struct FunctionTraitInfo<Fn> : FunctionTraitInfo<std::add_pointer_t<Fn>> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) const> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...)> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) volatile> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) const volatile> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) &> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) const &> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) volatile &> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) const volatile &> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) &&> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) const &&> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) volatile &&> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename T, typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(T::*)(Args...) const volatile &&> : FunctionTraitInfo<Ret(*)(Args...)> {
        };

        template<typename Ret, typename... Args>
        struct FunctionTraitInfo<Ret(*)(Args...)> {
        private:
            using MyReturnType = Ret;
            using MyArgumentTypes = std::tuple<Args...>;
            constexpr static size_t MyArgumentCount = sizeof...(Args);

        public:
            template<size_t Idx> requires (Idx < MyArgumentCount)
            using ArgumentType = std::tuple_element_t<Idx, MyArgumentTypes>;

            template<size_t Idx, typename Default>
            using ArgumentTypeOrDefault = std::conditional_t<
                Idx < MyArgumentCount,
                std::tuple_element_t<Idx, MyArgumentTypes>,
                Default>;

            template<typename Default>
            using ReturnType = MyReturnType;

            template<typename Default>
            using ReturnTypeOrDefault = MyReturnType;
        };
    }

    template<typename Fn, typename Exception>
    using InvokeResultOfException = std::invoke_result_t<Fn, std::conditional_t<std::is_same_v<std::exception,
            std::remove_cvref_t<Exception>>,
        typename TypeTraits::FunctionTraitInfo<Fn>::template ArgumentTypeOrDefault<0, std::exception &>,
        Exception &>>;

    template<typename Ret>
    struct InjectBase {
        template<typename E = std::exception, typename Fn>
            requires !std::invocable<Fn>
        auto Catch(this Awaitable<Ret> self,
                   Fn catchFunction) ->
            Awaitable<
                std::conditional_t<
                    std::is_same_v<Ret, void>,
                    std::conditional_t<
                        std::is_same_v<void, InvokeResultOfException<Fn, E>>,
                        void,
                        std::optional<InvokeResultOfException<Fn, E>>>,
                    std::conditional_t<
                        std::is_same_v<Ret, InvokeResultOfException<Fn, E>>,
                        Ret,
                        std::conditional_t<
                            std::is_same_v<InvokeResultOfException<Fn, E>, void>,
                            std::optional<Ret>,
                            std::variant<Ret, InvokeResultOfException<Fn, E>>>>>>;

        template<std::invocable<> Fn>
        auto Catch(this Awaitable<Ret> self, Fn catchAnyFunction) ->
            Awaitable<
                std::conditional_t<
                    std::is_same_v<Ret, void>,
                    std::conditional_t<
                        std::is_same_v<void, std::invoke_result_t<Fn>>,
                        void,
                        std::optional<std::invoke_result_t<Fn>>>,
                    std::conditional_t<
                        std::is_same_v<Ret, std::invoke_result_t<Fn>>,
                        Ret,
                        std::conditional_t<
                            std::is_same_v<std::invoke_result_t<Fn>, void>,
                            std::optional<Ret>,
                            std::variant<Ret, std::invoke_result_t<Fn>>>>>>;
    };

    template<typename Ret>
    struct InjectUnwraps : InjectBase<Ret> {
    };

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    struct InjectUnwraps<Ret> : InjectBase<Ret> {
    public:
        auto UnwrapOrCancel(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type>;

        template<typename Provider>
        auto UnwrapOr(this Awaitable<Ret> self, Provider provider) -> Awaitable<typename Ret::value_type>;

        auto UnwrapOrDefault(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type>;

        auto Unwrap(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type>;

        auto UnwrapOrThrow(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type>;
    };

    template<typename T>
    struct WrapAwaitable {
        using Type = Awaitable<T>;

        static auto Wrap(auto func) {
            return [f = std::move(func)]<typename... Args>(Args &&... args) -> Awaitable<T> {
                co_return f(std::forward<Args>(args)...);
            };
        }
    };

    template<typename T>
    struct WrapAwaitable<Awaitable<T>> {
        using Type = Awaitable<T>;

        static auto Wrap(auto func) {
            return func;
        }
    };

    export template<>
    class Awaitable<void> : public InjectUnwraps<void> {
    public:
        using ReturnType = std::monostate;

        using PromiseType = PromiseType<void>;
        using promise_type = PromiseType;

        using OptionalType = std::optional<std::monostate>;

        std::shared_ptr<void> m_MyHandlePtr;

        const std::shared_ptr<void> &GetHandlePtr() const {
            return m_MyHandlePtr;
        }

        Awaitable(std::coroutine_handle<PromiseType> handle);

        // protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return PointerToHandleCast<PromiseType>(m_MyHandlePtr);
        }

    public:
        Awaitable(const Awaitable &) = delete;

        Awaitable &operator=(const Awaitable &) = delete;

        Awaitable(Awaitable &&) = default;

        Awaitable &operator=(Awaitable &&) = default;

        Awaitable(std::weak_ptr<void> handlePtr) : m_MyHandlePtr(handlePtr.lock()) {
        }

        Awaitable Clone() const {
            return {m_MyHandlePtr};
        }

        Awaitable &&Move() {
            return std::move(*this);
        }

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

        Awaitable Cancellable(this Awaitable self, bool value);

        bool IsCancellable() const {
            return !GetMyHandle().promise().NotCancellable;
        }

        void SetOnFinished(auto &&callback) const {
            std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);
            GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
        }

        void SetContext(ExecutionContext *context) const {
            GetMyHandle().promise().Context = context;
        }

        void await_resume() const;

        void await_suspend(std::coroutine_handle<> parentHandle) const {
            GetMyHandle().promise().Schedule();
        }

        bool IsCancelled() const {
            return GetMyHandle().promise().IsCancelled;
        }

        std::monostate GetResult();

        std::optional<std::monostate> TryGetResult();

        template<typename Func>
        auto Then(this Awaitable self, Func &&func) -> WrapAwaitable<
            std::invoke_result_t<Func>>::Type;

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this Awaitable self, Duration duration) -> Awaitable;

        // Awaitable CaptureEnabled(this Awaitable self);
    };

    template<typename Ret>
    class Awaitable : public InjectUnwraps<Ret> {
    public:
        using ReturnType = Ret;

        using PromiseType = PromiseType<Ret>;
        using promise_type = PromiseType;

        using OptionalType = std::conditional_t<
            std::is_reference_v<Ret>,
            std::optional<std::reference_wrapper<std::remove_reference_t<Ret>>>,
            std::optional<Ret>>;

        std::shared_ptr<void> m_MyHandlePtr;

        const std::shared_ptr<void> &GetHandlePtr() const {
            return m_MyHandlePtr;
        }

    public:
        Awaitable(std::coroutine_handle<PromiseType> handle);

        // protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return PointerToHandleCast<PromiseType>(m_MyHandlePtr);
        }

    public:
        Awaitable(const Awaitable &) = delete;

        Awaitable &operator=(const Awaitable &) = delete;

        Awaitable(Awaitable &&) = default;

        Awaitable &operator=(Awaitable &&) = default;

        Awaitable(std::weak_ptr<void> handlePtr) : m_MyHandlePtr(handlePtr.lock()) {
        }

        Awaitable Clone() const {
            return {m_MyHandlePtr};
        }

        Awaitable &&Move() {
            return std::move(*this);
        }

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

        void await_suspend(std::coroutine_handle<> parentHandle) const {
            GetMyHandle().promise().Schedule();
        }

        ExecutionContext *GetContext() const {
            return GetMyHandle().promise().Context;
        }

        void Cancel() {
            GetMyHandle().promise().Cancel();
        }

        Awaitable Cancellable(this Awaitable self, bool value);

        bool IsCancellable() const {
            return !GetMyHandle().promise().NotCancellable;
        }

        void SetOnFinished(auto &&callback) const;

        void SetContext(ExecutionContext *context) const {
            GetMyHandle().promise().Context = context;
        }

        Ret await_resume() const;

        bool IsCancelled() const {
            return GetMyHandle().promise().IsCancelled;
        }

        Ret GetResult();

        OptionalType TryGetResult();

        template<typename Func>
        auto Then(this Awaitable self, Func &&func) -> WrapAwaitable<
            std::invoke_result_t<Func, Ret>>::Type;

        template<typename Duration = std::chrono::milliseconds>
        auto WithTimeOut(this Awaitable self, Duration duration) -> Awaitable<OptionalType>;

        // Awaitable CaptureEnabled(this Awaitable self);
    };

    template<typename Ret>
    Ret ExecutionContext::BlockOn(Awaitable<Ret> awaitable) {
        std::binary_semaphore semaphore(0);
        auto wrapper = [awaitable = awaitable.Move()]<typename Self>(this Self self) mutable -> Awaitable<Ret> {
            co_return co_await self.awaitable.Move();
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

    template<typename T> requires (CanInto<T>)
    auto ExecutionContext::Async(T canInto) {
        auto future = std::async(std::launch::async, [this, awaitable = canInto.Move().Into()]() mutable {
            return BlockOn(awaitable.Move());
        });

        return future;
    }

    template<typename T> requires (CanInto<T> && !IsAwaitable<T>)
    void ExecutionContext::BlockOn(T canIntoType) {
        BlockOn(std::move(canIntoType) >> Into);
    }


    template<>
    void ExecutionContext::BlockOn(Awaitable<void> awaitable) {
        std::binary_semaphore semaphore(0);
        auto wrapper = [awaitable = awaitable.Move()]<typename Self>(this Self self)mutable -> Awaitable<void> {
            co_await self.awaitable.Move();
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
    Awaitable<Ret> PromiseType<Ret>::get_return_object() {
        ++g_AllocCount;
        return {std::coroutine_handle<PromiseType>::from_promise(*this)};
    }


    template<typename T>
    const Awaitable<T> &PromiseBase::await_transform(const Awaitable<T> &awaitable) {
        awaitable.SetContext(Context);
        auto parentHandle = Self.lock();
        assert(parentHandle);
        awaitable.SetOnFinished([parentHandle = Self]mutable {
            if (auto copied = parentHandle.lock()) {
                PointerToHandleCast<PromiseBase>(copied).promise().Schedule();
            }
        });

        return awaitable;
    }

    template<typename T>
    Awaitable<T> &&PromiseBase::await_transform(Awaitable<T> &&awaitable) {
        awaitable.SetContext(Context);
        auto parentHandle = Self.lock();
        assert(parentHandle);
        awaitable.SetOnFinished([parentHandle = Self]mutable {
            if (auto copied = parentHandle.lock()) {
                PointerToHandleCast<PromiseBase>(copied).promise().Schedule();
            }
        });

        return std::forward<Awaitable<T> &&>(awaitable);
    }

    template<typename Fn>
    std::suspend_never PromiseBase::await_transform(AwaitToDoImmediate<Fn> &&awaiter) {
        awaiter.func(std::coroutine_handle<PromiseBase>::from_promise(*this));
        return {};
    }

    template<typename Ret>
    template<std::convertible_to<Ret> T>
    void PromiseType<Ret>::return_value(T &&value) {
        // Protect
        {
            std::scoped_lock lock(ResultProtectMutex);
            Result = Ret(std::move(value));
            if (OnFinished) {
                OnFinished();
                OnFinished = nullptr;
                Context = nullptr;
            }
        }
        DetachedSelf.reset();
    }

    template<typename Ret>
    template<std::convertible_to<Ret> T>
    std::suspend_always PromiseType<Ret>::yield_value(T &&value) {
        if (DetachedSelf) {
            DetachedSelf.reset();
            throw std::runtime_error("A generator coroutine must be cancellable");
        }

        // Protect
        {
            std::scoped_lock lock(ResultProtectMutex);
            Result = Ret(std::move(value));
            if (OnFinished) {
                OnFinished();
                OnFinished = nullptr;
                Context = nullptr;
            }
        }

        return {};
    }

    template<typename Ret>
    void PromiseType<Ret>::return_value(void) {
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

    template<typename Ret>
    Awaitable<Ret &> PromiseType<Ret &>::get_return_object() {
        ++g_AllocCount;
        return {std::coroutine_handle<PromiseType>::from_promise(*this)};
    }

    template<typename Ret>
    template<typename T> requires std::convertible_to<Ret &, T &>
    void PromiseType<Ret &>::return_value(T &value) {
        // Protect
        {
            std::scoped_lock lock(ResultProtectMutex);
            Result = &value;
            if (OnFinished) {
                OnFinished();
                OnFinished = nullptr;
                Context = nullptr;
            }
        }
        DetachedSelf.reset();
    }

    template<typename Ret>
    void PromiseType<Ret &>::return_value(void) {
        if (!IsCancelled) {
            throw std::runtime_error("Cannot return void from non-void coroutine which is not canceled");
        }
        DetachedSelf.reset();
    }

    template<typename Ret>
    template<typename T> requires std::convertible_to<Ret &, T &>
    std::suspend_always PromiseType<Ret &>::yield_value(T &value) {
        if (DetachedSelf) {
            DetachedSelf.reset();
            throw std::runtime_error("A generator coroutine must be cancellable");
        }

        // Protect
        {
            std::scoped_lock lock(ResultProtectMutex);
            Result = &value;
            if (OnFinished) {
                OnFinished();
                OnFinished = nullptr;
                Context = nullptr;
            }
        }

        return {};
    }

    template<typename Ret>
    void PromiseType<Ret &>::unhandled_exception() {
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

    Awaitable<void> PromiseType<void>::get_return_object() {
        ++g_AllocCount;
        return {std::coroutine_handle<PromiseType>::from_promise(*this)};
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

    Awaitable<void>::Awaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandlePtr(
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

    ExecutionContext *Awaitable<void>::GetContext() const {
        return GetMyHandle().promise().Context;
    }

    Awaitable<void> Awaitable<void>::Cancellable(this Awaitable self, bool value) {
        self.GetMyHandle().promise().NotCancellable = !value;
        if (value == false) {
            self.GetMyHandle().promise().DetachedSelf = self.GetMyHandle().promise().Self.lock();
        }
        return self.Move();
    }

    void Awaitable<void>::await_resume() const {
        auto handle = GetMyHandle();

        // std::lock_guard lock(handle.promise().ResultProtectMutex);
        // auto &variant = handle.promise().Result;
        // switch (variant.index()) {
        //     case 0:
        //         return;
        //     case 1:
        //         std::rethrow_exception(std::get<std::exception_ptr>(variant));
        //     default:
        //         throw std::runtime_error("Invalid state in coroutine result");
        // }
        handle.promise().GetResultValue();
    }

    std::monostate Awaitable<void>::GetResult() {
        auto handle = GetMyHandle();

        return handle.promise().GetResultValue();
    }

    std::optional<std::monostate> Awaitable<void>::TryGetResult() {
        // if (std::holds_alternative<std::monostate>(GetMyHandle().promise().Result)) {
        //     return std::make_optional<std::monostate>();
        // }
        // if (std::holds_alternative<std::exception_ptr>(GetMyHandle().promise().Result)) {
        //     std::rethrow_exception(std::get<std::exception_ptr>(GetMyHandle().promise().Result));
        // }
        // return std::nullopt;

        return GetMyHandle().promise().TryGetResultValue();
    }

    export template<typename... Tps>
    Awaitable<std::tuple<typename Awaitable<Tps>::ReturnType...>>
    AllOf(Awaitable<Tps>... awaitables) {
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

        co_await AwaitToDo{
            [&](std::coroutine_handle<>) {
                (awaitables.GetMyHandle().promise().Schedule(), ...);

                semaphore.release(1);
            }
        };

        auto result = std::make_tuple(std::move(awaitables.GetResult())...);

        (awaitables.Reset(), ...);

        co_return std::move(result);
    }

    export template<typename... Tps>
    Awaitable<std::tuple<typename Awaitable<Tps>::OptionalType...>> AnyOf(
        Awaitable<Tps>... awaitables) {
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


        co_await AwaitToDo{
            [&](std::coroutine_handle<>) {
                auto applier = [](auto &awaitable) {
                    awaitable.GetMyHandle().promise().Schedule();
                };

                (applier(awaitables), ...);

                semaphore.release(1);
            }
        };

        auto result = std::make_tuple(std::move(awaitables.TryGetResult())...);

        (awaitables.Reset(), ...);

        co_return std::move(result);
    }


    export template<typename T>
    Awaitable<T> StartWith(T value) {
        co_return std::move(value);
    }

    export template<typename Func>
    auto AsynchronousOf(Func func) {
        return [func = std::move(func)]<typename Self, typename... Args>(
            this Self self,
            Args &&... args) mutable -> Awaitable<decltype(func(args...))> {
            co_return self.func(std::forward<Args>(args)...);
        };
    }


    Awaitable<void> Sleep(auto duration) {
        std::this_thread::sleep_for(duration);
        co_return;
    }

    template<typename Func>
    auto Awaitable<void>::Then(this Awaitable self,
                               Func &&func) -> WrapAwaitable<
        std::invoke_result_t<Func>>::Type {
        co_await self.Move();
        // co_return co_await WrapAwaitable<std::invoke_result_t<Func>>::Wrap(std::forward<Func>(func))();
        if constexpr (std::is_same_v<Awaitable<void>, typename WrapAwaitable<std::invoke_result_t<Func>>::Type>) {
            co_await WrapAwaitable<std::invoke_result_t<Func>>::Wrap(std::forward<Func>(func))();
            co_return;
        } else {
            co_return co_await WrapAwaitable<std::invoke_result_t<Func>>::Wrap(std::forward<Func>(func))();
        }
    }

    template<typename Duration>
    auto Awaitable<void>::
    WithTimeOut(this Awaitable self,
                Duration duration) -> Awaitable {
        co_await AnyOf(
            self.Move(),
            Sleep(duration)
        );
    }

    template<typename Ret>
    template<typename E, typename Fn>
        requires !std::invocable<Fn>
    auto InjectBase<Ret>::Catch(this Awaitable<Ret> self,
                                Fn catchFunction) ->
        Awaitable<
            std::conditional_t<
                std::is_same_v<Ret, void>,
                std::conditional_t<
                    std::is_same_v<void, InvokeResultOfException<Fn, E>>,
                    void,
                    std::optional<InvokeResultOfException<Fn, E>>>,
                std::conditional_t<
                    std::is_same_v<Ret, InvokeResultOfException<Fn, E>>,
                    Ret,
                    std::conditional_t<
                        std::is_same_v<InvokeResultOfException<Fn, E>, void>,
                        std::optional<Ret>,
                        std::variant<Ret, InvokeResultOfException<Fn, E>>>>>> {
        using ReturnType = std::conditional_t<
            std::is_same_v<Ret, void>,
            std::conditional_t<
                std::is_same_v<void, InvokeResultOfException<Fn, E>>,
                void,
                std::optional<InvokeResultOfException<Fn, E>>>,
            std::conditional_t<
                std::is_same_v<Ret, InvokeResultOfException<Fn, E>>,
                Ret,
                std::conditional_t<
                    std::is_same_v<InvokeResultOfException<Fn, E>, void>,
                    std::optional<Ret>,
                    std::variant<Ret, InvokeResultOfException<Fn, E>>>>>;

        /*
        using ReturnType =
                std::conditional_t<
                    std::is_same_v<Ret, void>,
                    std::conditional_t<
                        std::is_same_v<void, std::invoke_result_t<Fn,
                            std::conditional_t<
                                std::is_same_v<std::exception, std::remove_cvref_t<E>>,
                                typename TypeTraits::FunctionTraitInfo<Fn>::
                                template ArgumentTypeOrDefault<0, std::exception &>, E &>>>,
                        void,
                        std::optional<std::invoke_result_t<Fn,
                            std::conditional_t<
                                std::is_same_v<std::exception, std::remove_cvref_t<E>>,
                                typename TypeTraits::FunctionTraitInfo<Fn>::
                                template ArgumentTypeOrDefault<0, std::exception &>, E &>>>>,
                    std::conditional_t<
                        std::is_same_v<Ret, std::invoke_result_t<Fn,
                            std::conditional_t<
                                std::is_same_v<std::exception, std::remove_cvref_t<E>>,
                                typename TypeTraits::FunctionTraitInfo<Fn>::
                                template ArgumentTypeOrDefault<0, std::exception &>, E &>>>,
                        Ret,
                        std::conditional_t<
                            std::is_same_v<std::invoke_result_t<Fn,
                                std::conditional_t<
                                    std::is_same_v<std::exception, std::remove_cvref_t<E>>,
                                    typename TypeTraits::FunctionTraitInfo<Fn>::
                                    template ArgumentTypeOrDefault<0, std::exception &>, E &>>, void>,
                            std::optional<Ret>,
                            std::variant<Ret, std::invoke_result_t<Fn,
                                std::conditional_t<
                                    std::is_same_v<std::exception, std::remove_cvref_t<E>>,
                                    typename TypeTraits::FunctionTraitInfo<Fn>::
                                    template ArgumentTypeOrDefault<0, std::exception &>, E &>>>>>>;
         */

        using ActualCatchReferenceType =
                std::conditional_t<std::is_same_v<std::exception, std::remove_cvref_t<E>>,
                    typename TypeTraits::FunctionTraitInfo<Fn>::
                    template ArgumentTypeOrDefault<0, std::exception &>, E &>;

        static_assert(std::invocable<Fn, ActualCatchReferenceType>,
                      "Template parameter of Catch, which may have been defaulted to std::exception&, "
                      "cannot actually be invoked with that type.");

        try {
            if constexpr (std::is_same_v<Ret, void>) {
                co_await self.Move();
                co_return ReturnType{};
            } else {
                co_return ReturnType{co_await self.Move()};
            }
        } catch (ActualCatchReferenceType e) {
            if constexpr (std::is_same_v<Ret, InvokeResultOfException<Fn, E>>) {
                if constexpr (std::is_same_v<void, InvokeResultOfException<Fn, E>>) {
                    catchFunction(e);
                    co_return ReturnType{};
                } else {
                    co_return ReturnType{catchFunction(e)};
                }
            } else {
                if constexpr (std::is_same_v<void, InvokeResultOfException<Fn, E>>) {
                    catchFunction(e);
                    co_return ReturnType{std::nullopt};
                } else {
                    co_return ReturnType{catchFunction(e)};
                }
            }
        }
    }

    template<typename Ret>
    template<std::invocable<> Fn>
    auto InjectBase<Ret>::Catch(this Awaitable<Ret> self,
                                Fn catchAnyFunction) ->
        Awaitable<
            std::conditional_t<
                std::is_same_v<Ret, void>,
                std::conditional_t<
                    std::is_same_v<void, std::invoke_result_t<Fn>>,
                    void,
                    std::optional<std::invoke_result_t<Fn>>>,
                std::conditional_t<
                    std::is_same_v<Ret, std::invoke_result_t<Fn>>,
                    Ret,
                    std::conditional_t<
                        std::is_same_v<std::invoke_result_t<Fn>, void>,
                        std::optional<Ret>,
                        std::variant<Ret, std::invoke_result_t<Fn>>>>>> {
        using ReturnType = std::conditional_t<
            std::is_same_v<Ret, void>,
            std::conditional_t<
                std::is_same_v<void, std::invoke_result_t<Fn>>,
                void,
                std::optional<std::invoke_result_t<Fn>>>,
            std::conditional_t<
                std::is_same_v<Ret, std::invoke_result_t<Fn>>,
                Ret,
                std::conditional_t<
                    std::is_same_v<std::invoke_result_t<Fn>, void>,
                    std::optional<Ret>,
                    std::variant<Ret, std::invoke_result_t<Fn>>>>>;

        try {
            if constexpr (std::is_same_v<Ret, void>) {
                co_await self.Move();
                co_return ReturnType{};
            } else {
                co_return ReturnType{co_await self.Move()};
            }
        } catch (...) {
            if constexpr (std::is_same_v<Ret, std::invoke_result_t<Fn>>) {
                if constexpr (std::is_same_v<void, std::invoke_result_t<Fn>>) {
                    catchAnyFunction();
                    co_return ReturnType{};
                } else {
                    co_return ReturnType{catchAnyFunction()};
                }
            } else {
                if constexpr (std::is_same_v<void, std::invoke_result_t<Fn>>) {
                    catchAnyFunction();
                    co_return ReturnType{std::nullopt};
                } else {
                    co_return ReturnType{catchAnyFunction()};
                }
            }
        }
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::
    UnwrapOrCancel(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type> {
        auto value = co_await self.Move();
        if (value) {
            co_return *value;
        }

        co_await AwaitToDo{
            [](std::coroutine_handle<> thisHandle) {
                auto myHandle = HandleReinterpretCast<PromiseType<Ret>>(thisHandle);
                myHandle.promise().Cancel();
                HandleReinterpretCast<PromiseBase>(thisHandle).promise().Schedule();
            }
        };

        co_return;
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    template<typename Provider>
    auto InjectUnwraps<Ret>::UnwrapOr(this Awaitable<Ret> self,
                                      Provider provider) -> Awaitable<typename Ret::value_type> {
        auto value = co_await self.Move();
        if (value) {
            co_return *value;
        }

        if constexpr (std::convertible_to<Provider, typename Ret::value_type>) {
            co_return std::move(provider);
        } else if constexpr (std::convertible_to<std::invoke_result_t<Provider>, typename
            Ret::value_type>) {
            co_return provider();
        } else if constexpr (std::is_same_v<std::invoke_result_t<Provider>,
            Awaitable<typename Ret::value_type>>) {
            co_return co_await provider();
        } else {
            static_assert(
                [] { return false; }(),
                "Provider must be a callable returning the value type or an awaitable of the value type, "
                "or the value type itself.");
        }
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::UnwrapOrDefault(
        this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type> {
        auto value = co_await self.Move();
        if (value) {
            co_return *value;
        }
        co_return typename Ret::value_type{};
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::Unwrap(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type> {
        co_return *co_await self.Move();
    }

    template<typename Ret> requires requires(Ret ret) {
        { static_cast<bool>(ret) } -> std::convertible_to<bool>;
        { *ret } -> std::convertible_to<typename Ret::value_type>;
    }
    auto InjectUnwraps<Ret>::
    UnwrapOrThrow(this Awaitable<Ret> self) -> Awaitable<typename Ret::value_type> {
        auto value = co_await self.Move();
        if (value) {
            co_return *value;
        }
        throw std::runtime_error(std::format("Attempted to unwrap an empty {} in Awaitable",
                                             typeid(Ret).name()));
    }


    template<typename Ret>
    Awaitable<Ret>::Awaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandlePtr(
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
    Awaitable<Ret> Awaitable<Ret>::Cancellable(this Awaitable self, bool value) {
        self.GetMyHandle().promise().NotCancellable = !value;
        if (value == false) {
            self.GetMyHandle().promise().DetachedSelf = self.GetMyHandle().promise().Self.lock();
        } else {
            self.GetMyHandle().promise().DetachedSelf.reset();
        }
        return self.Move();
    }

    template<typename Ret>
    void Awaitable<Ret>::SetOnFinished(auto &&callback) const {
        std::lock_guard lock(GetMyHandle().promise().ResultProtectMutex);
        GetMyHandle().promise().OnFinished = std::forward<decltype(callback)>(callback);
    }

    template<typename Ret>
    Ret Awaitable<Ret>::await_resume() const {
        auto handle = GetMyHandle();

        // std::lock_guard lock(handle.promise().ResultProtectMutex);
        // auto &variant = handle.promise().Result;
        // switch (variant.index()) {
        //     case 0:
        //         throw std::runtime_error("Coroutine did not return a value");
        //     case 1:
        //         return std::move(std::get<Ret>(variant));
        //     case 2:
        //         std::rethrow_exception(std::get<std::exception_ptr>(variant));
        //     default:
        //         throw std::runtime_error("Invalid state in coroutine result");
        // }
        return handle.promise().GetResultValue();
    }

    template<typename Ret>
    Ret Awaitable<Ret>::GetResult() {
        auto handle = GetMyHandle();

        return handle.promise().GetResultValue();
    }

    template<typename Ret>
    Awaitable<Ret>::OptionalType Awaitable<Ret>::TryGetResult() {
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
    auto Awaitable<Ret>::Then(this Awaitable self, Func &&func)
        ->
        WrapAwaitable<
            std::invoke_result_t<Func, Ret>>::Type {
        if constexpr (std::is_same_v<typename WrapAwaitable<std::invoke_result_t<Func, Ret>>::Type, Awaitable<void>>) {
            co_await WrapAwaitable<std::invoke_result_t<Func, Ret>>::Wrap(std::forward<Func>(func))(
                co_await self.Move());
            co_return;
        } else {
            co_return co_await WrapAwaitable<std::invoke_result_t<Func, Ret>>::Wrap(std::forward<Func>(func))(
                co_await self.Move());
        }
    }

    template<typename Ret>
    template<typename Duration>
    auto Awaitable<Ret>::WithTimeOut(this Awaitable self, Duration duration) -> Awaitable<OptionalType> {
        co_return std::get<0>(co_await AnyOf(
            self.Move(),
            Sleep(duration)
        ));
    }

    export template<typename Func>
    auto TryUntilHasValue(Func func, std::chrono::milliseconds timeInterval = std::chrono::milliseconds(0)) {
        return [func = std::move(func), timeInterval]<typename Self, typename... Args>(
            this Self self,
            Args &&... args) mutable -> Awaitable<typename std::invoke_result_t<Func, Args...>::value_type> {
            while (true) {
                if constexpr (requires(const Func func) {
                    func(std::forward<Args>(args)...);
                }) {
                    // Function can be called by const, always move function to reduce copy, this works because const
                    // function can be called multiple times
                    auto [value, func] = co_await [](const Func func, Args &&... args)
                        -> Awaitable<std::pair<std::invoke_result_t<Func, Args...>, Func>> {
                                auto result = func(std::forward<Args>(args)...);
                                co_return std::make_pair(std::move(result), std::move(func));
                            }(std::move(self.func), std::forward<Args>(args)...);

                    if (value) {
                        co_return std::move(*value);
                    }

                    self.func = std::move(func);
                } else {
                    // Function cannot be called by const, we need to always copy the function because function which
                    // cannot be called by const may modify its state and may only be called once. This is rare though,
                    // most C++ functions can be called by const.
                    auto value = co_await [](Func func, Args &&... args)
                        -> Awaitable<std::invoke_result_t<Func, Args...>> {
                                co_return func(std::forward<Args>(args)...);
                            }(self.func, std::forward<Args>(args)...);

                    if (value) {
                        co_return std::move(*value);
                    }
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

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::IntoType);

export template<typename... Tps>
auto operator>>(EasyCoro::AllOfType<Tps...> allOfType, EasyCoro::IntoType);

export template<typename... Tps>
auto operator>>(EasyCoro::AnyOfType<Tps...> AnyOfType, EasyCoro::IntoType);

namespace EasyCoro {
    template<typename... Tps>
    struct AllOfType {
        std::tuple<Tps...> values;

        auto Into(this AllOfType self) {
            return std::apply(
                []<typename... T>(T &&... args) {
                    return AllOf((std::forward<T>(args) >> EasyCoro::Into)...);
                },
                std::move(self.values));
        }

        AllOfType Move() {
            return std::move(*this);
        }
    };

    template<typename... Tps>
    struct AnyOfType {
        std::tuple<Tps...> values;

        auto Into(this AnyOfType self) {
            return std::apply(
                []<typename... T>(T &&... args) {
                    return AnyOf((std::forward<T>(args) >> EasyCoro::Into)...);
                },
                std::move(self.values));
        }

        AnyOfType Move() {
            return std::move(*this);
        }
    };

    template<typename... Tps>
    auto PromiseBase::await_transform(AllOfType<Tps...> allOfType) {
        return await_transform(std::move(allOfType).Into());
    }

    template<typename... Tps>
    auto PromiseBase::await_transform(AnyOfType<Tps...> anyOfType) {
        return await_transform(std::move(anyOfType).Into());
    }
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::IntoType) {
    return awaitable.Move();
}

export template<typename... Tps>
auto operator>>(EasyCoro::AllOfType<Tps...> allOfType, EasyCoro::IntoType) {
    return allOfType.Move().Into();
}

export template<typename... Tps>
auto operator>>(EasyCoro::AnyOfType<Tps...> anyOfType, EasyCoro::IntoType) {
    return anyOfType.Move().Into();
}

export template<typename... Tps, typename Ret>
auto operator&&(EasyCoro::AllOfType<Tps...> allOfType, EasyCoro::Awaitable<Ret> awaitable) {
    return EasyCoro::AllOfType<Tps..., EasyCoro::Awaitable<Ret>>{
        std::tuple_cat(std::move(allOfType.values), std::make_tuple(awaitable.Move()))
    };
}

export template<typename... Tps, typename Ret>
auto operator&&(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::AllOfType<Tps...> allOfType) {
    return EasyCoro::AllOfType<EasyCoro::Awaitable<Ret>, Tps...>{
        std::tuple_cat(std::make_tuple(awaitable.Move()), std::move(allOfType.values))
    };
}

export template<typename... Tps1, typename... Tps2>
auto operator&&(EasyCoro::AllOfType<Tps1...> allOfType1, EasyCoro::AllOfType<Tps2...> allOfType2) {
    return EasyCoro::AllOfType<Tps1..., Tps2...>{
        std::tuple_cat(std::move(allOfType1.values), std::move(allOfType2.values))
    };
}

export template<typename Ret1, typename Ret2>
auto operator&&(EasyCoro::Awaitable<Ret1> awaitable1, EasyCoro::Awaitable<Ret2> awaitable2) {
    return EasyCoro::AllOfType<EasyCoro::Awaitable<Ret1>, EasyCoro::Awaitable<Ret2>>{
        std::make_tuple(awaitable1.Move(), awaitable2.Move())
    };
}

export template<typename... Tps, typename Ret>
auto operator||(EasyCoro::AnyOfType<Tps...> anyOfType, EasyCoro::Awaitable<Ret> awaitable) {
    return EasyCoro::AnyOfType<Tps..., EasyCoro::Awaitable<Ret>>{
        std::tuple_cat(std::move(anyOfType.values), std::make_tuple(awaitable.Move()))
    };
}

export template<typename... Tps, typename Ret>
auto operator||(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::AnyOfType<Tps...> anyOfType) {
    return EasyCoro::AnyOfType<EasyCoro::Awaitable<Ret>, Tps...>{
        std::tuple_cat(std::make_tuple(awaitable.Move()), std::move(anyOfType.values))
    };
}

export template<typename... Tps1, typename... Tps2>
auto operator||(EasyCoro::AnyOfType<Tps1...> anyOfType1, EasyCoro::AnyOfType<Tps2...> anyOfType2) {
    return EasyCoro::AnyOfType<Tps1..., Tps2...>{
        std::tuple_cat(std::move(anyOfType1.values), std::move(anyOfType2.values))
    };
}

export template<typename Ret1, typename Ret2>
auto operator||(EasyCoro::Awaitable<Ret1> awaitable1, EasyCoro::Awaitable<Ret2> awaitable2) {
    return EasyCoro::AnyOfType<EasyCoro::Awaitable<Ret1>, EasyCoro::Awaitable<Ret2>>{
        std::make_tuple(awaitable1.Move(), awaitable2.Move())
    };
}

export template<typename... Tps1, typename... Tps2>
auto operator&&(EasyCoro::AllOfType<Tps1...> allOfType, EasyCoro::AnyOfType<Tps2...> anyOfType) {
    return allOfType.Move().Into() && anyOfType.Move().Into();
}

export template<typename... Tps1, typename... Tps2>
auto operator&&(EasyCoro::AnyOfType<Tps1...> anyOfType, EasyCoro::AllOfType<Tps2...> allOfType) {
    return anyOfType.Move().Into() && allOfType.Move().Into();
}

export template<typename Ret, typename... Tps>
auto operator&&(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::AnyOfType<Tps...> anyOfType) {
    return awaitable.Move() && anyOfType.Move().Into();
}

export template<typename Ret, typename... Tps>
auto operator&&(EasyCoro::AnyOfType<Tps...> anyOfType, EasyCoro::Awaitable<Ret> awaitable) {
    return anyOfType.Move().Into() && awaitable.Move();
}

export template<typename... Tps1, typename... Tps2>
auto operator||(EasyCoro::AllOfType<Tps1...> allOfType, EasyCoro::AnyOfType<Tps2...> anyOfType) {
    return allOfType.Move().Into() || anyOfType.Move().Into();
}

export template<typename... Tps1, typename... Tps2>
auto operator||(EasyCoro::AnyOfType<Tps1...> anyOfType, EasyCoro::AllOfType<Tps2...> allOfType) {
    return anyOfType.Move().Into() || allOfType.Move().Into();
}

export template<typename Ret, typename... Tps>
auto operator||(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::AllOfType<Tps...> allOfType) {
    return awaitable.Move() || allOfType.Move().Into();
}

export template<typename Ret, typename... Tps>
auto operator||(EasyCoro::AllOfType<Tps...> allOfType, EasyCoro::Awaitable<Ret> awaitable) {
    return allOfType.Move().Into() || awaitable.Move();
}

export template<typename Ret, typename Func>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, Func &&func) {
    return awaitable.Move().Then(std::forward<Func>(func));
}

namespace EasyCoro {
    struct CancellableType {
        bool IsCancellable;
    };

    export constexpr CancellableType Cancellable(bool value) {
        return CancellableType{value};
    }

    struct UnwrapType {
    };

    export constexpr UnwrapType Unwrap() {
        return UnwrapType{};
    }

    struct UnwrapOrCancelType {
    };

    export constexpr UnwrapOrCancelType UnwrapOrCancel() {
        return UnwrapOrCancelType{};
    }

    struct UnwrapOrDefaultType {
    };

    export constexpr UnwrapOrDefaultType UnwrapOrDefault() {
        return UnwrapOrDefaultType{};
    }

    export template<typename T>
    struct UnwrapOrType {
        T provider;
    };

    export template<typename T>
    constexpr UnwrapOrType<T> UnwrapOr(T provider) {
        return UnwrapOrType<T>{std::move(provider)};
    }

    struct WithTimeOutType {
        std::chrono::milliseconds Duration;
    };

    export constexpr WithTimeOutType WithTimeOut(std::chrono::milliseconds duration) {
        return WithTimeOutType{duration};
    }

    export template<typename E, typename Fn>
    struct CatchType {
        Fn catchFunction;
    };

    export template<typename E, typename Fn>
    constexpr auto Catch(Fn catchFunction) {
        return CatchType<E, Fn>{std::move(catchFunction)};
    }

    export template<std::invocable<> Fn>
    struct CatchAnyType {
        Fn catchFunction;
    };

    export template<std::invocable<> Fn>
    constexpr auto Catch(Fn catchFunction) {
        return CatchAnyType<Fn>{std::move(catchFunction)};
    }

    export struct UnwrapOrThrowType {
    };

    export constexpr UnwrapOrThrowType UnwrapOrThrow() {
        return UnwrapOrThrowType{};
    }
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::CancellableType cancellableType) {
    return awaitable.Move().Cancellable(cancellableType.IsCancellable);
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::UnwrapType) {
    return awaitable.Move().Unwrap();
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::UnwrapOrCancelType) {
    return awaitable.Move().UnwrapOrCancel();
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::UnwrapOrDefaultType) {
    return awaitable.Move().UnwrapOrDefault();
}

export template<typename Ret, typename T>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::UnwrapOrType<T> unwrapOrType) {
    return awaitable.Move().UnwrapOr(std::move(unwrapOrType.provider));
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::WithTimeOutType withTimeOutType) {
    return awaitable.Move().WithTimeOut(withTimeOutType.Duration);
}

export template<typename Ret, typename E, typename Fn>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::CatchType<E, Fn> catchType) {
    return awaitable.Move().template Catch<E>(std::move(catchType.catchFunction));
}

export template<typename Ret, std::invocable<> Fn>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::CatchAnyType<Fn> catchAnyType) {
    return awaitable.Move().template Catch<Fn>(std::move(catchAnyType.catchFunction));
}

export template<typename Ret>
auto operator>>(EasyCoro::Awaitable<Ret> awaitable, EasyCoro::UnwrapOrThrowType) {
    return awaitable.Move().UnwrapOrThrow();
}
