import EasyCoro.Awaitable;

import std;
import <cassert>;
import <cstdlib>;

EasyCoro::SimpleAwaitable<double> NestedCoroutine(EasyCoro::SimpleAwaitable<int> inner,
                                                  EasyCoro::SimpleAwaitable<int> inner2) {
    std::cout << "Starting nested coroutine..." << '\n';
    int value = co_await inner;
    int value2 = co_await inner2;
    std::cout << "Inner coroutine completed with value: " << value << " and " << value2 << '\n';
    co_return value * 2.5 * value2;
}

EasyCoro::SimpleAwaitable<int> ExampleCoroutine() {
    std::cout << "Hello from coroutine!" << '\n';
    co_return 42;
}

struct ReportDestructor {
    // ~ReportDestructor() {
    //     std::cout << "ReportDestructor destroyed!" << '\n';
    // }
};


EasyCoro::SimpleAwaitable<size_t> Sleep(int time) {
    std::this_thread::sleep_for(std::chrono::milliseconds(time));
    co_return time;
}

EasyCoro::SimpleAwaitable<size_t> Sleep(int time, auto &&... rest) {
    size_t result = co_await Sleep(time);
    result += co_await Sleep(std::forward<decltype(rest)>(rest)...);
    co_return result;
}

EasyCoro::SimpleAwaitable<void> ReturnVoid() {
    // ReportDestructor destructor{};
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

EasyCoro::SimpleAwaitable<size_t> (*SleepPtr)(int) = Sleep;

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

// specialize formatter<Unit>
template<>
struct std::formatter<EasyCoro::Unit> {
    constexpr auto parse(auto &ctx) const { return ctx.begin(); }

    auto format(const EasyCoro::Unit &, auto &ctx) const {
        return std::format_to(ctx.out(), "Unit");
    }
};

int main() {
    std::atomic_size_t counter = 0;
    // Inner
    {
        EasyCoro::ExecutionContext context(32);

        std::string example1 = "Example string 1";
        std::string example2 = "Example string 2";
        std::string example3 = "Example string 3";

        try {
            auto now = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < 10000; ++i) {
                context.BlockOn(
                    // EasyCoro::AllOf(
                        Sleep(0)/*, Sleep(0), Sleep(0), Sleep(0), Sleep(0),
                        Sleep(0), Sleep(0), Sleep(0), Sleep(0, 0, 0, 0, 0, 0),
                        Sleep(0), Sleep(0), Sleep(0), Sleep(0), Sleep(0),
                        EasyCoro::Pull([] -> EasyCoro::SimpleAwaitable<std::optional<size_t>> {
                            if (rand() % 2 == 0) {
                                co_return std::nullopt;
                            }
                            co_return std::nullopt;
                        }).UnWrapOr([] { return rand(); })
                        .Then([&](size_t value) -> EasyCoro::SimpleAwaitable<size_t> {
                            std::cout << std::format("Value from random coroutine: {}\n", value);

                            co_await Sleep(0);
                            co_await Sleep(0);

                            // ++counter;
                            co_return value;
                        }).Cancellable(true)
                    ).Then([](auto thing) -> EasyCoro::SimpleAwaitable<void> {
                        std::cout << std::format("Completed AnyOf {}\n", thing);
                        co_return;
                    }*///)
                );
            }
            auto later = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(later - now).count();
            std::cout << "Total duration for 10000 iterations: " << duration << " ms" << std::endl;
        } catch (const std::exception &ex) {
            std::cerr << "Caught exception: " << ex.what() << std::endl;
        }

        // context.Detach();
    }

    // report memory
    std::cout << std::endl;

    // std::this_thread::sleep_for(std::chrono::seconds(5));

    // std::cout << "Counter: " << counter.load() << std::endl;

    std::cout << "Alloc count: " << EasyCoro::g_AllocCount.load() << std::endl;
    std::cout << "Dealloc count: " << EasyCoro::g_DeallocCount.load() << std::endl;

    std::cout << "Main function completed." << std::endl;
}
