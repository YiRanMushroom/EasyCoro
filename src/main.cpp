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
    std::this_thread::sleep_for(std::chrono::seconds(time));
    std::cout << "Slept for " << time << " seconds." << std::endl;
    co_return time;
}

int main() {
    EasyCoro::MultiThreadedExecutionContext context(32);
    try {
        context.BlockOn(EasyCoro::AllOf(
            NestedCoroutine(ExampleCoroutine(), ExampleCoroutine()),
            Sleep(2),
            Sleep(3),
            Sleep(1)
        ));
    } catch (const std::exception &ex) {
        std::cerr << "Caught exception: " << ex.what() << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Main function completed." << std::endl;
}
