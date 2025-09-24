import EasyCoro.Awaitable;

import std;

EasyCoro::SimpleAwaitable<double> NestedCoroutine(EasyCoro::SimpleAwaitable<int> inner,
                                                  EasyCoro::SimpleAwaitable<int> inner2) {
    std::cout << "Starting nested coroutine..." << std::endl;
    int value = co_await inner;
    int value2 = co_await inner2;
    std::cout << "Inner coroutine completed with value: " << value << std::endl;
    co_return value * 2.5 * value2;
}

EasyCoro::SimpleAwaitable<int> ExampleCoroutine() {
    std::cout << "Hello from coroutine!" << std::endl;
    co_return 42;
}

struct ReportDestructor {
    ~ReportDestructor() {
        std::cout << "ReportDestructor destroyed!" << std::endl;
    }
};

EasyCoro::SimpleAwaitable<size_t> Sleep(int time) {
    ReportDestructor destructor{};
    std::this_thread::sleep_for(std::chrono::seconds(time));
    std::cout << "Slept for " << time << " seconds." << std::endl;
    co_return time;
}

EasyCoro::SimpleAwaitable<size_t> Sleep(int time, auto &&... rest) {
    size_t result = co_await Sleep(time);
    result += co_await Sleep(std::forward<decltype(rest)>(rest)...);
    co_return result;
}

int main() {
    EasyCoro::ExecutionContext context(32);
    try {
        context.BlockOn(EasyCoro::AnyOf(
            NestedCoroutine(ExampleCoroutine(), ExampleCoroutine()),
            Sleep(2),
            Sleep(3, 6, 6, 4),
            EasyCoro::AllOf(Sleep(1), Sleep(2), Sleep(3))
        ));
    } catch (const std::exception &ex) {
        std::cerr << "Caught exception: " << ex.what() << std::endl;
    }

    std::cout << "Main function completed." << std::endl;
}
