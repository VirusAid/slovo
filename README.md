# slovo

**A tiny, zero-dependency, single-header C++17 client for LLM APIs.**

[![CI](https://github.com/VirusAid/slovo/actions/workflows/ci.yml/badge.svg)](https://github.com/VirusAid/slovo/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Header only](https://img.shields.io/badge/header--only-yes-green.svg)

*Slovo* (слово) is the Russian word for "word".

```cpp
#include <slovo/slovo.hpp>

int main() {
    auto llm = slovo::Client::openai(std::getenv("OPENAI_API_KEY"));
    std::cout << llm.chat("Why is the sky blue?").content << "\n";
}
```

One header. No libcurl, no nlohmann, no OpenSSL, no build system gymnastics.
Copy `slovo.hpp` into your project and talk to any OpenAI-compatible endpoint:
**OpenAI, OpenRouter, Groq, DeepSeek, Mistral, Together, Ollama, llama.cpp
server, LM Studio, vLLM** — cloud or fully local.

## Why slovo?

Every other C++ LLM wrapper drags in libcurl + a JSON library + CMake glue
before you can send one prompt. slovo ships its own:

- **JSON parser/serializer** — small, strict, UTF-8 correct, insertion-ordered
- **HTTP transports** — WinHTTP on Windows (https included), plain sockets
  everywhere (perfect for local models), optional libcurl backend
- **SSE streaming parser** — token-by-token output with chunked encoding
  handled properly
- **Tool calling loop** — register C++ lambdas as tools, slovo runs the
  request → tool call → result → final answer cycle for you
- **Retries** — exponential backoff with jitter on 429/5xx and network errors

Everything lives in [`include/slovo/slovo.hpp`](include/slovo/slovo.hpp).

## Install

**Option 1 — copy the header.** It's one file. Done.

**Option 2 — CMake FetchContent:**

```cmake
include(FetchContent)
FetchContent_Declare(slovo
    GIT_REPOSITORY https://github.com/VirusAid/slovo.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(slovo)
target_link_libraries(your_app PRIVATE slovo::slovo)
```

**Option 3 — CMake subdirectory / install:** `add_subdirectory(slovo)` or
`cmake --install`, then `find_package(slovo)`.

> **HTTPS note:** on Windows, https works out of the box via WinHTTP. On
> Linux/macOS the zero-dependency build is http-only (fine for Ollama,
> llama.cpp, LM Studio and other local servers); for https to cloud APIs,
> configure with `-DSLOVO_USE_CURL=ON` (or define `SLOVO_USE_CURL` and link
> libcurl).

## Providers

```cpp
auto a = slovo::Client::openai(key);                       // api.openai.com
auto b = slovo::Client::openrouter(key, "openai/gpt-4o");  // 400+ models, one key
auto c = slovo::Client::groq(key);                         // fast Llama
auto d = slovo::Client::deepseek(key);
auto e = slovo::Client::mistral(key);
auto f = slovo::Client::ollama("llama3.2");                // local, no key
auto g = slovo::Client::llamacpp();                        // llama-server
auto h = slovo::Client::lmstudio();

// ...or any other OpenAI-compatible endpoint:
slovo::Config cfg;
cfg.base_url = "https://my-gateway.example.com/v1";
cfg.api_key  = "...";
cfg.model    = "my-model";
slovo::Client custom(cfg);
```

## Streaming

```cpp
llm.stream("Tell me a story.", [](const std::string& token) {
    std::cout << token << std::flush;
});
```

## Conversations & options

```cpp
slovo::Options opt;
opt.temperature = 0.2;
opt.max_tokens  = 500;
opt.json_mode   = true;                 // response_format: json_object
opt.extra["seed"] = 42;                 // any provider-specific field

auto reply = llm.chat({
    slovo::Message::system("You are a terse assistant."),
    slovo::Message::user("List three C++17 features as JSON."),
}, opt);

slovo::Json parsed = slovo::Json::parse(reply.content);
```

## Tool calling

```cpp
slovo::ToolBox tools;
tools.add("get_weather", "Weather for a city",
    slovo::Json::parse(R"({
        "type": "object",
        "properties": {"city": {"type": "string"}},
        "required": ["city"]
    })"),
    [](const slovo::Json& args) {
        return my_weather_api(args["city"].str());
    });

std::vector<slovo::Message> history{slovo::Message::user("Weather in Oslo?")};
auto reply = llm.chat_tools(history, tools);   // runs the full tool loop
std::cout << reply.content << "\n";
```

## Embeddings

```cpp
auto vectors = llm.embed({"first text", "second text"}, "text-embedding-3-small");
```

## Error handling

```cpp
try {
    auto reply = llm.chat("hi");
} catch (const slovo::Error& e) {
    // e.kind: Network | Http | Parse | Usage
    // e.status: HTTP status code, e.body: raw response
    std::cerr << e.what() << "\n";
}
```

Transient failures (429, 5xx, connection errors) are retried automatically
with exponential backoff — tune with `Config::max_retries`.

## Custom transport

Own event loop, proxy, recorded fixtures for tests? Implement one virtual
method:

```cpp
struct MyTransport : slovo::Transport {
    slovo::HttpResponse send(const slovo::HttpRequest& req,
                             const slovo::ChunkSink& sink) override;
};
cfg.transport = std::make_shared<MyTransport>();
```

The test suite uses exactly this to run the whole client offline.

## Building the tests & examples

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Design notes

- C++17, exceptions for errors, no RTTI requirements, no macros in your code.
- `slovo::Json` keeps object key order and round-trips int64 exactly.
- A `Client` is cheap to construct; use one per thread for concurrency.
- The escape hatches `client.get(path)` / `client.post(path, json)` cover
  endpoints the typed API doesn't.

## Roadmap

- [ ] Anthropic Messages API (native schema)
- [ ] Vision / image inputs
- [ ] Async interface
- [ ] Response headers surfaced (Retry-After aware backoff)
- [ ] vcpkg / Conan packaging

Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) © 2026 D3F0LT (VirusAid)
