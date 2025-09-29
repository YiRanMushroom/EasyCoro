import EasyCoro.Awaitable;

import std;
import <cassert>;
import <cstdlib>;

EasyCoro::Awaitable<double> NestedCoroutine(EasyCoro::Awaitable<int> inner,
                                            EasyCoro::Awaitable<int> inner2) {
    std::cout << "Starting nested coroutine..." << '\n';
    int value = co_await inner;
    int value2 = co_await inner2;
    std::cout << "Inner coroutine completed with value: " << value << " and " << value2 << '\n';
    co_return value * 2.5 * value2;
}

EasyCoro::Awaitable<int> ExampleCoroutine() {
    std::cout << "Hello from coroutine!" << '\n';
    co_return 42;
}

struct ReportDestructor {
    // ~ReportDestructor() {
    //     std::cout << "ReportDestructor destroyed!" << '\n';
    // }
};


EasyCoro::Awaitable<size_t> Sleep(int time) {
    // std::cout << std::format("Sleeping for {} ms...\n", time);
    std::this_thread::sleep_for(std::chrono::milliseconds(time));
    co_return time;
}

EasyCoro::Awaitable<size_t> Sleep(int time, auto &&... rest) {
    size_t result = 0;
    result += co_await Sleep(std::forward<decltype(rest)>(rest)...);
    co_return result;
}

EasyCoro::Awaitable<void> ReturnVoid() {
    co_await Sleep(0);
    co_return;
}

struct {
    std::mutex Mutex;
    std::optional<std::string> Value;
} g_ConsoleBuffer;

void StartConsoleListener() {
    std::thread([] {
        while (true) {
            std::string input;
            std::getline(std::cin, input);
            while (true) {
                std::unique_lock lock(g_ConsoleBuffer.Mutex);
                if (!g_ConsoleBuffer.Value.has_value()) {
                    g_ConsoleBuffer.Value = input;
                    break;
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }).detach();
}

std::optional<std::string> GetConsoleInput() {
    std::unique_lock lock(g_ConsoleBuffer.Mutex);
    if (g_ConsoleBuffer.Value.has_value()) {
        auto value = g_ConsoleBuffer.Value;
        g_ConsoleBuffer.Value = std::nullopt;
        return value;
    }
    return std::nullopt;
}

std::atomic_size_t g_AllocCount = 0;

EasyCoro::Awaitable<size_t> (*SleepPtr)(int) = Sleep;

// specialize formatter<std::option<T>>
template<typename T>
struct std::formatter<std::optional<T>> : std::formatter<T> {
    constexpr auto parse(auto &ctx) const { return ctx.begin(); }

    auto format(const std::optional<T> &opt, auto &ctx) const {
        if (opt.has_value()) {
            return std::formatter<T>::format(opt.value(), ctx);
        } else {
            return std::format_to(ctx.out(), "nullopt");
        }
    }
};

template<>
struct std::formatter<EasyCoro::Unit> {
    constexpr auto parse(auto &ctx) const { return ctx.begin(); }

    auto format(const EasyCoro::Unit &, auto &ctx) const {
        return std::format_to(ctx.out(), "Unit");
    }
};

int main() {
    std::atomic_size_t counter = 0;
    StartConsoleListener();
    // Inner
    {
        EasyCoro::ExecutionContext context(64);

        EasyCoro::Finally f1([&] {
            context.Join();
        });

        std::string example1 = "Example string 1";
        std::string example2 = "Example string 2";
        std::string example3 = "Example string 3";


        try {
            auto now = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < 1000; ++i) {
                context.BlockOn(Sleep(0) >> SleepPtr >> SleepPtr
                                && Sleep(0)
                                || EasyCoro::Pull([] -> EasyCoro::Awaitable<std::optional<size_t>> {
                                    if (rand() % 2 == 0) {
                                        co_return std::nullopt;
                                    }
                                    co_return std::nullopt;
                                })
                                >> EasyCoro::UnwrapOr([] { return rand(); })
                                >> [&](size_t value) -> EasyCoro::Awaitable<std::string> {
                                    static std::atomic_size_t localCounter = 0;
                                    ++localCounter;
                                    static EasyCoro::Finally report([&] {
                                        std::cout << std::format("Local counter: {}\n", localCounter.load());
                                    });

                                    std::cout << std::format("Value from random coroutine: {}\n", value);

                                    co_return co_await EasyCoro::Pull(EasyCoro::TryUntilHasValue(
                                                GetConsoleInput, std::chrono::milliseconds(10))).WithTimeOut(
                                                std::chrono::milliseconds(0))
                                            .UnwrapOr("Default Value");
                                }
                                >> EasyCoro::AsynchronousOf([](std::string str) {
                                    std::cout << std::format("Processing string asynchronously: {}\n", str);
                                    return str.size();
                                })
                                >> [](auto thing) -> EasyCoro::Awaitable<void> {
                                    std::cout << std::format("Completed AnyOf {}\n", thing);
                                    co_return;
                                }
                                >> EasyCoro::Cancellable(false));
            }
            auto later = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(later - now).count();
            std::cout << "Total duration for 10000 iterations: " << duration << " ms" << std::endl;
        } catch (const std::exception &ex) {
            std::cerr << "Caught exception: " << ex.what() << std::endl;
        }
    }

    // report memory
    std::cout << std::endl;

    // std::this_thread::sleep_for(std::chrono::seconds(5));

    // std::cout << "Counter: " << counter.load() << std::endl;

    std::mutex mtx;

    mtx.lock();

    std::cout << "Alloc count: " << EasyCoro::g_AllocCount.load() << std::endl;
    std::cout << "Dealloc count: " << EasyCoro::g_DeallocCount.load() << std::endl;

    mtx.unlock();
    std::cout << "Main function completed." << std::endl;
}
