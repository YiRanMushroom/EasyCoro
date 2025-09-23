import EasyCoro.Awaitable;

import std;

EasyCoro::SimpleAwaitable<double> NestedCoroutine(EasyCoro::SimpleAwaitable<int> inner) {
    std::cout << "Starting nested coroutine..." << std::endl;
    int value = co_await inner;
    std::cout << "Inner coroutine completed with value: " << value << std::endl;
    co_return value * 2.5;
}

// EasyCoro::SimpleAwaitable<int> ExampleCoroutine() {
//     std::cout << "Hello from coroutine!" << std::endl;
//     co_return 42;
// }

int main() {
    EasyCoro::MultiThreadedExecutionContext context(32);
    try {
        std::cout << context.BlockOn(NestedCoroutine(
            [](int value) -> EasyCoro::SimpleAwaitable<int> {
                std::cout << "Inner coroutine running with value: " << value << std::endl;
                co_return 100;
            }(20)
        ));
    } catch (const std::exception &ex) {
        std::cerr << "Caught exception: " << ex.what() << std::endl;
    }
}
