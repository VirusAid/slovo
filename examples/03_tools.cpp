// Tool calling: the model calls C++ functions, slovo runs the loop.
#include <slovo/slovo.hpp>
#include <cstdlib>
#include <ctime>
#include <iostream>

int main() {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) {
        std::cerr << "set OPENAI_API_KEY first\n";
        return 1;
    }

    auto llm = slovo::Client::openai(key, "gpt-4o-mini");

    slovo::ToolBox toolbox;
    toolbox.add(
        "get_time", "Returns the current local time as a string.",
        slovo::Json::parse(R"({"type":"object","properties":{}})"),
        [](const slovo::Json&) {
            std::time_t now = std::time(nullptr);
            char buf[64];
            std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            return std::string(buf);
        });
    toolbox.add(
        "get_weather", "Returns the weather for a city.",
        slovo::Json::parse(R"({
            "type": "object",
            "properties": {"city": {"type": "string"}},
            "required": ["city"]
        })"),
        [](const slovo::Json& args) {
            return "Sunny, +21C in " + args["city"].str();  // your real API here
        });

    try {
        std::vector<slovo::Message> messages{
            slovo::Message::user("What time is it, and what's the weather in Oslo?"),
        };
        auto reply = llm.chat_tools(messages, toolbox);
        std::cout << reply.content << "\n";
    } catch (const slovo::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
