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

EasyCoro::SimpleAwaitable<void> ReturnVoid() {
    ReportDestructor destructor{};
    co_await Sleep(1);
    co_return;
}

int main() {
    EasyCoro::ExecutionContext context(32);

    std::string example1 = "Example string 1";
    std::string example2 = "Example string 2";
    std::string example3 = "Example string 3";

    try {
        context.BlockOn(EasyCoro::AnyOf(
            Sleep(3),
            []() -> EasyCoro::SimpleAwaitable<size_t> {
                std::cout << "Starting lambda coroutine..." << std::endl;
                co_return 123;
            }().Then([](size_t value) -> EasyCoro::SimpleAwaitable<std::string> {
                std::cout << "Next lambda coroutine with value: " << value << std::endl;
                co_return "Value is " + std::to_string(value);
            }).Then(
                [example1, example2, example3](std::string str) -> EasyCoro::SimpleAwaitable<void> {
                    std::cout << "Final lambda coroutine with string: " << str << std::endl;
                    co_await Sleep(2);
                    std::cout << "Using captured strings: " << example1 << ", " << example2 << ", " << example3 << std::endl;
                    co_return;
                }
            ).Then([]() -> EasyCoro::SimpleAwaitable<size_t> {
                co_return co_await Sleep(5);
            })
        ));
    } catch (const std::exception &ex) {
        std::cerr << "Caught exception: " << ex.what() << std::endl;
    }

    std::cout << "Main function completed." << std::endl;
}
