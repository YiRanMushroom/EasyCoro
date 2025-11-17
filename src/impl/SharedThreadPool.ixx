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
            EnqueueFunc([task] { (*task)(); });
            return res;
        }

    public:
        virtual void Join() = 0;
    };

    export class SharedThreadPool : public IThreadPool {
        friend class std::shared_ptr<SharedThreadPool>;

    public:
        template<typename OnStartUp, typename OnShutDown>
        static std::shared_ptr<IThreadPool> Create(size_t size = std::jthread::hardware_concurrency() * 2,
                                                   OnStartUp &&onStartUp = {},
                                                   OnShutDown &&onShutDown = {}) {
            // ReSharper disable once CppSmartPointerVsMakeFunction
            auto pool = std::shared_ptr<SharedThreadPool>(new SharedThreadPool(size));
            pool->SetSelf(pool);
            Start(pool, std::forward<OnStartUp>(onStartUp), std::forward<OnShutDown>(onShutDown));
            return pool;
        }

    protected:
        SharedThreadPool(size_t size = std::jthread::hardware_concurrency() * 2) {
            m_WorkerThreads.reserve(size);
        }

    public:
        void Join() override {
            WaitAllTaskToFinish();
            m_ShouldStop = true;
            m_Condition.notify_all();
            for (auto &thread: m_WorkerThreads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            m_WorkerThreads.clear();
        }

        void SetSelf(const std::shared_ptr<SharedThreadPool> &self) {
            m_Self = self;
        }

        template<typename OnStartUp, typename OnShutDown>
        static std::shared_ptr<IThreadPool> Start(std::shared_ptr<SharedThreadPool> self, OnStartUp &&onStartUp = {},
                                                  OnShutDown &&onShutDown = {}) {
            for (size_t i = 0; i < self->m_WorkerThreads.capacity(); ++i) {
                self->m_WorkerThreads.emplace_back([self = self->m_Self.lock(), onStartUp, onShutDown] {
                    if constexpr (std::is_invocable_v<OnStartUp>) {
                        onStartUp();
                    }
                    while (!(self->m_ShouldStop && self->m_Tasks.empty())) {
                        std::function<void()> task;

                        // in a scope
                        {
                            std::unique_lock lock(self->m_Mutex);
                            self->m_Condition.wait_for(lock, std::chrono::milliseconds(100), [&self] {
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
            return self;
        }

        void EnqueueFunc(std::function<void()> &&task) override {
            std::lock_guard lock(m_Mutex);
            m_Tasks.emplace(std::move(task));
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

    export class StandardThreadPool : public IThreadPool {
    public:
        StandardThreadPool() = default;

        void EnqueueFunc(std::function<void()> &&task) override = 0;

        /*{
            std::binary_semaphore sem1(0), sem2(0);
            std::atomic<std::shared_ptr<std::future<void>>> shared_ptr = std::make_shared<std::future<void>>(std::async(
                std::launch::async, [&, task = std::move(task)] {
                    sem1.acquire();
                    auto copy = shared_ptr.load();
                    sem2.release();
                    task();
                }));
            sem1.release();
            sem2.acquire();
            shared_ptr.store(nullptr);
        }*/


        static std::shared_ptr<IThreadPool> Create() {
            // return std::shared_ptr<IThreadPool>(std::make_shared<StandardThreadPool>());
            throw std::logic_error("StandardThreadPool is not implemented");
        }
    };
}
