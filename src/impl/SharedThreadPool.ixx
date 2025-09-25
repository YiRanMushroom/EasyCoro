export module EasyCoro.ThreadPool;

import std;

namespace EasyCoro {
    export class IThreadPool {
    public:
        virtual ~IThreadPool() = default;

    protected:
        virtual void EnqueueFunc(std::function<void()> &&task) = 0;

    public:
        template<typename... Args> requires std::invocable<Args...>
        void EnqueueDetached(Args &&... args) {
            std::function<void()> task = [tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                std::apply([]<typename... Tps>(const auto &first, Tps &&... rest) {
                    first(std::forward<Tps>(rest)...);
                }, std::move(tup));
            };
            EnqueueFunc(std::move(task));
        }

        template<typename... Args> requires std::invocable<Args...>
        auto Enqueue(Args &&... args) {
            using ReturnType = std::invoke_result_t<Args...>;
            auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                [tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                    return std::apply([]<typename... Tps>(auto &&first, Tps &&... rest) {
                        return first(std::forward<Tps>(rest)...);
                    }, std::move(tup));
                });

            std::future<ReturnType> res = task->get_future();
            EnqueueFunc([task]() { (*task)(); });
            return res;
        }
    };

    export class SharedThreadPool : public IThreadPool {
        friend class std::shared_ptr<SharedThreadPool>;

    public:
        template<typename OnStartUp, typename OnShutDown>
        static std::shared_ptr<SharedThreadPool> Create(size_t size = std::jthread::hardware_concurrency() * 2,
                                                        OnStartUp &&onStartUp = {},
                                                        OnShutDown &&onShutDown = {}) {
            // ReSharper disable once CppSmartPointerVsMakeFunction
            auto pool = std::shared_ptr<SharedThreadPool>(new SharedThreadPool(size));
            pool->SetSelf(pool);
            pool->Start(std::forward<OnStartUp>(onStartUp), std::forward<OnShutDown>(onShutDown));
            return pool;
        }

    protected:
        SharedThreadPool(size_t size = std::jthread::hardware_concurrency() * 2) {
            m_WorkerThreads.reserve(size);
        }

    public:
        void SetSelf(const std::shared_ptr<SharedThreadPool> &self) {
            m_Self = self;
        }

        template<typename OnStartUp, typename OnShutDown>
        void Start(OnStartUp &&onStartUp = {},
                   OnShutDown &&onShutDown = {}) {
            for (size_t i = 0; i < m_WorkerThreads.capacity(); ++i) {
                m_WorkerThreads.emplace_back([self = m_Self.lock(), onStartUp, onShutDown] {
                    if constexpr (std::is_invocable_v<OnStartUp>) {
                        onStartUp();
                    }
                    while (!self->m_ShouldStop) {
                        std::function<void()> task;

                        // in a scope
                        {
                            std::unique_lock lock(self->m_Mutex);
                            self->m_Condition.wait_for(lock, std::chrono::milliseconds(10), [&self] {
                                return self->m_ShouldStop || !self->m_Tasks.empty();
                            });

                            if (self->m_ShouldStop && self->m_Tasks.empty()) {
                                return;
                            }

                            if (self->m_Tasks.empty()) {
                                continue;
                            }

                            task = std::move(self->m_Tasks.front());
                            self->m_Tasks.pop();
                        }

                        try {
                            task();
                        } catch (std::exception &e) {
                            std::cerr << "Exception in thread pool task: " << e.what() << std::endl;
                        } catch (...) {
                            std::cerr << "Unknown exception in thread pool task" << std::endl;
                        }
                    }
                    if constexpr (std::is_invocable_v<OnShutDown>) {
                        onShutDown();
                    }
                });
            }
        }

        void EnqueueFunc(std::function<void()> &&task) override {
            // forward
            {
                std::lock_guard lock(m_Mutex);
                m_Tasks.emplace(std::move(task));
            }
            m_Condition.notify_one();
        }

        ~SharedThreadPool() {
            if (m_ShouldStop) {
                m_WorkerThreads.clear();
                return;
            }

            m_ShouldStop = true;
            m_Condition.notify_all();
            for (auto &thread: m_WorkerThreads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }

        void DetachAll() {
            for (auto &thread: m_WorkerThreads) {
                if (thread.joinable()) {
                    thread.detach();
                }
            }
            m_WorkerThreads.clear();
        }

        void WaitAllTaskToFinish() {
            while (true) {
                {
                    std::lock_guard lock(m_Mutex);
                    if (m_Tasks.empty()) {
                        break;
                    }
                }
                std::this_thread::yield();
            }
        }

    private:
        std::vector<std::jthread> m_WorkerThreads{};
        std::condition_variable m_Condition{};
        std::mutex m_Mutex{};
        std::queue<std::function<void()>> m_Tasks{};
        std::atomic_bool m_ShouldStop{false};
        std::weak_ptr<SharedThreadPool> m_Self;
    };
}
