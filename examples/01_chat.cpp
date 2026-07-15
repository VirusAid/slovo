// Basic blocking chat. Set OPENAI_API_KEY (or adapt to your provider).
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

    slovo::Options options;
    options.temperature = 0.7;
    options.max_tokens = 200;

    try {
        auto reply = llm.chat({
            slovo::Message::system("You answer in one short paragraph."),
            slovo::Message::user("Why is the sky blue?"),
        }, options);

        std::cout << reply.content << "\n\n"
                  << "model: " << reply.model
                  << ", tokens: " << reply.usage.total_tokens << "\n";
    } catch (const slovo::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
