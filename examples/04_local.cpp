// Local models: Ollama / llama.cpp server / LM Studio, no API key, no TLS —
// works on every platform with zero dependencies.
#include <slovo/slovo.hpp>
#include <iostream>

int main() {
    // ollama pull llama3.2 && ollama serve
    auto llm = slovo::Client::ollama("llama3.2");

    try {
        auto reply = llm.stream("Explain RAII in two sentences.",
                                [](const std::string& text) {
                                    std::cout << text << std::flush;
                                });
        std::cout << "\n";
    } catch (const slovo::Error& e) {
        std::cerr << "error: " << e.what()
                  << "\n(is Ollama running on 127.0.0.1:11434?)\n";
        return 1;
    }
    return 0;
}
