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

    export class ThreadPool : public IThreadPool {
    public:
        ThreadPool(size_t size = std::jthread::hardware_concurrency() * 2) {
            m_WorkerThreads.reserve(size);
            for (size_t i = 0; i < size; ++i) {
                m_WorkerThreads.emplace_back([this] {
                    while (!m_ShouldStop) {
                        std::function<void()> task;

                        // in a scope
                        {
                            std::unique_lock lock(m_Mutex);
                            m_Condition.wait(lock, [this] { return m_ShouldStop || !m_Tasks.empty(); });

                            if (m_ShouldStop && m_Tasks.empty()) {
                                return;
                            }

                            task = std::move(m_Tasks.front());
                            m_Tasks.pop();
                        }

                        task();
                    }
                });
            }
        }

        template<typename OnStartUp, typename OnShutDown>
        ThreadPool(size_t size = std::jthread::hardware_concurrency() * 2, OnStartUp &&onStartUp = {},
                   OnShutDown &&onShutDown = {}) {
            m_WorkerThreads.reserve(size);
            for (size_t i = 0; i < size; ++i) {
                m_WorkerThreads.emplace_back([this, onStartUp, onShutDown] {
                    if constexpr (std::is_invocable_v<OnStartUp>) {
                        onStartUp();
                    }
                    while (!m_ShouldStop) {
                        std::function<void()> task;

                        // in a scope
                        {
                            std::unique_lock lock(m_Mutex);
                            m_Condition.wait(lock, [this] { return m_ShouldStop || !m_Tasks.empty(); });

                            if (m_ShouldStop && m_Tasks.empty()) {
                                return;
                            }

                            task = std::move(m_Tasks.front());
                            m_Tasks.pop();
                        }

                        task();
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

        ~ThreadPool() {
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
            m_ShouldStop = true;
            m_Condition.notify_all();
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
    };

    export IThreadPool *GlobalThreadPool() {
        static ThreadPool pool{};
        return &pool;
    }
}
