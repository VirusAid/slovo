// Streaming: tokens are printed as they arrive.
#include <slovo/slovo.hpp>
#include <cstdlib>
#include <iostream>

int main() {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) {
        std::cerr << "set OPENAI_API_KEY first\n";
        return 1;
    }

    auto llm = slovo::Client::openai(key, "gpt-4o-mini");

    try {
        auto reply = llm.stream("Write a haiku about compilers.",
                                [](const std::string& text) {
                                    std::cout << text << std::flush;
                                });
        std::cout << "\n\nfinish_reason: " << reply.finish_reason << "\n";
    } catch (const slovo::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
