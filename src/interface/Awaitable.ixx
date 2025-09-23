export module EasyCoro.Awaitable;

import std;
import EasyCoro.ThreadPool;
import <cassert>;
import <stddef.h>;

namespace EasyCoro {
    export class AwaitableBase {
    public:
        virtual ~AwaitableBase() = default;

        virtual void Cancel() = 0;

        [[nodiscard]] virtual std::coroutine_handle<> GetHandle() const = 0;

        [[nodiscard]] virtual bool IsCancelled() const = 0;
    };

    export class ExecutionContext {
    public:
        virtual ~ExecutionContext() = default;

        virtual void Schedule(std::coroutine_handle<> awaitable) = 0;
    };

    export thread_local ExecutionContext *CurrentExecutionContext = nullptr;

    export ExecutionContext &GetCurrentExecutionContext() {
        if (!CurrentExecutionContext) {
            throw std::runtime_error("No current execution context set");
        }

        return *CurrentExecutionContext;
    }

    export template<typename Ret>
    class AwaitableBaseRet : public AwaitableBase {
    public:
        ~AwaitableBaseRet() override = default;

        [[nodiscard]] virtual Ret GetResult() = 0;
    };

    export class MultiThreadedExecutionContext : public ExecutionContext {
    public:
        MultiThreadedExecutionContext(size_t threadCount = std::jthread::hardware_concurrency() * 2)
            : m_ThreadPool(threadCount, [this] {
                CurrentExecutionContext = this;
            }, [] {
                CurrentExecutionContext = nullptr;
            }) {}

    public:
        void Schedule(std::coroutine_handle<> handle) override {
            m_ThreadPool.Enqueue([handle] {
                handle.resume();
            });
        }

        void WaitAllTaskToFinish() {
            m_ThreadPool.WaitAllTaskToFinish();
        }

        auto BlockOn(auto &&awaitable) {
            auto handle = awaitable.GetHandle();
            if (!handle.done()) {
                Schedule(handle);
            }
            while (!handle.done()) {
                WaitAllTaskToFinish();
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            return awaitable.GetResult();
        }

    private:
        ThreadPool m_ThreadPool{};
    };

    export template<typename Ret>
    class SimpleAwaitable;



    export template<>
    class SimpleAwaitable<void> : public AwaitableBaseRet<void> {
    public:
        struct PromiseType {
            std::atomic_bool IsCancelled = false;
            std::function<std::optional<std::coroutine_handle<>>()> ParentHandleCallback = [] {
                return std::nullopt;
            };

            std::mutex ChildrenMutex{};
            std::function<void()> CancelChildren = [] {};

            std::variant<std::monostate, std::exception_ptr> Result{std::monostate{}};

            void Cancel() {
                IsCancelled = true;
                std::lock_guard lock(ChildrenMutex);
                CancelChildren();
            }

            auto get_return_object() {
                return SimpleAwaitable{std::coroutine_handle<PromiseType>::from_promise(*this)};
            }

            constexpr static std::suspend_always initial_suspend() { return {}; }
            constexpr static std::suspend_always final_suspend() noexcept { return {}; }

            void return_void() {
                Result = std::monostate{};
                if (auto parent = ParentHandleCallback()) {
                    GetCurrentExecutionContext().Schedule(*parent);
                }
            }

            void unhandled_exception() { Result = std::current_exception(); }
        };

        using promise_type = PromiseType;

        std::coroutine_handle<PromiseType> m_MyHandle;

    public:
        SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandle(
            handle) {}

        SimpleAwaitable(const SimpleAwaitable &) = delete;

        SimpleAwaitable(SimpleAwaitable &&other) noexcept : m_MyHandle(other.m_MyHandle) {
            other.m_MyHandle = nullptr;
        }

        SimpleAwaitable &operator=(const SimpleAwaitable &) = delete;

        SimpleAwaitable &operator=(SimpleAwaitable &&other) noexcept {
            if (this != &other) {
                if (m_MyHandle) {
                    m_MyHandle.destroy();
                }
                m_MyHandle = other.m_MyHandle;
                other.m_MyHandle = nullptr;
            }
            return *this;
        }

    protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return m_MyHandle;
        }

    public:
        ~SimpleAwaitable() override {
            if (m_MyHandle) {
                m_MyHandle.destroy();
            }
        }

        [[nodiscard]] std::coroutine_handle<> GetHandle() const override {
            return GetMyHandle();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            GetMyHandle().promise().ParentHandleCallback = [parentHandle
                    ]() mutable -> std::optional<std::coroutine_handle<>> {
                        if (auto copied = parentHandle) {
                            parentHandle = nullptr;
                            return copied;
                        }
                        return std::nullopt;
                    };

            // Unsafe, probably no better way, rely on undefined behavior
            {
                auto castedParent = std::coroutine_handle<PromiseType>::from_address(
                    parentHandle.address());
                auto &parentPromise = castedParent.promise();
                std::lock_guard lock(parentPromise.ChildrenMutex);
                parentPromise.CancelChildren = [this]() {
                    this->Cancel();
                };
            }

            GetCurrentExecutionContext().Schedule(GetMyHandle());
        }

        void Cancel() override {
            GetMyHandle().promise().Cancel();
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

        bool IsCancelled() const override {
            return GetMyHandle().promise().IsCancelled;
        }

        void GetResult() override {
            auto handle = GetMyHandle();
            assert(handle.done());
            await_resume();
        }
    };

    template<typename Ret>
    class SimpleAwaitable : public AwaitableBaseRet<Ret> {
    public:
        struct PromiseType {
            std::atomic_bool IsCancelled = false;
            std::function<std::optional<std::coroutine_handle<>>()> ParentHandleCallback = [] {
                return std::nullopt;
            };

            std::mutex ChildrenMutex{};
            std::function<void()> CancelChildren = [] {};

            std::variant<std::monostate, Ret, std::exception_ptr> Result{std::monostate{}};

            void Cancel() {
                IsCancelled = true;
                std::lock_guard lock(ChildrenMutex);
                CancelChildren();
            }

            auto get_return_object() {
                return SimpleAwaitable{std::coroutine_handle<PromiseType>::from_promise(*this)};
            }

            constexpr static std::suspend_always initial_suspend() { return {}; }
            constexpr static std::suspend_always final_suspend() noexcept { return {}; }

            void return_value(Ret value) {
                Result = std::move(value);
                if (auto parent = ParentHandleCallback()) {
                    GetCurrentExecutionContext().Schedule(*parent);
                }
            }

            void unhandled_exception() { Result = std::current_exception(); }
        };

        using promise_type = PromiseType;

        static_assert(offsetof(PromiseType, ChildrenMutex) == offsetof(SimpleAwaitable<void>::PromiseType, ChildrenMutex),
              "ChildrenMutex must be at the same offset in both PromiseType specializations");

        static_assert(offsetof(PromiseType, CancelChildren) == offsetof(SimpleAwaitable<void>::PromiseType, CancelChildren),
                      "CancelChildren must be at the same offset in both PromiseType specializations");

        std::coroutine_handle<PromiseType> m_MyHandle;

    public:
        SimpleAwaitable(std::coroutine_handle<PromiseType> handle) : m_MyHandle(
            handle) {}

        SimpleAwaitable(const SimpleAwaitable &) = delete;

        SimpleAwaitable(SimpleAwaitable &&other) noexcept : m_MyHandle(other.m_MyHandle) {
            other.m_MyHandle = nullptr;
        }

        SimpleAwaitable &operator=(const SimpleAwaitable &) = delete;

        SimpleAwaitable &operator=(SimpleAwaitable &&other) noexcept {
            if (this != &other) {
                if (m_MyHandle) {
                    m_MyHandle.destroy();
                }
                m_MyHandle = other.m_MyHandle;
                other.m_MyHandle = nullptr;
            }
            return *this;
        }

    protected:
        std::coroutine_handle<PromiseType> GetMyHandle() const {
            return m_MyHandle;
        }

    public:
        ~SimpleAwaitable() override {
            if (m_MyHandle) {
                m_MyHandle.destroy();
            }
        }

        [[nodiscard]] std::coroutine_handle<> GetHandle() const override {
            return GetMyHandle();
        }

        // Awaitable interface
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> parentHandle) {
            GetMyHandle().promise().ParentHandleCallback = [parentHandle
                    ]() mutable -> std::optional<std::coroutine_handle<>> {
                        if (auto copied = parentHandle) {
                            parentHandle = nullptr;
                            return copied;
                        }
                        return std::nullopt;
                    };

            // Unsafe, temporary change int
            {
                auto castedParent = std::coroutine_handle<SimpleAwaitable<void>::PromiseType>::from_address(
                    parentHandle.address());
                auto &parentPromise = castedParent.promise();
                std::lock_guard lock(parentPromise.ChildrenMutex);
                parentPromise.CancelChildren = [this]() {
                    this->Cancel();
                };
            }

            GetCurrentExecutionContext().Schedule(GetMyHandle());
        }

        void Cancel() override {
            GetMyHandle().promise().Cancel();
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

        bool IsCancelled() const override {
            return GetMyHandle().promise().IsCancelled;
        }

        Ret GetResult() override {
            auto handle = GetMyHandle();
            assert(handle.done());
            return await_resume();
        }
    };
}
