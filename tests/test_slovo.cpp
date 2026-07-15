// Test suite for slovo. No network access: the Client tests run against a
// scripted in-memory Transport.
#include <slovo/slovo.hpp>

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)

#define CHECK_THROWS(kind_, expr)                                            \
    do {                                                                     \
        ++g_checks;                                                          \
        bool thrown = false;                                                 \
        try {                                                                \
            (void)(expr);                                                    \
        } catch (const slovo::Error& e) {                                    \
            thrown = (e.kind == slovo::Error::Kind::kind_);                  \
        } catch (...) {}                                                     \
        if (!thrown) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: expected Error::Kind::%s from %s\n",    \
                        __FILE__, __LINE__, #kind_, #expr);                  \
        }                                                                    \
    } while (0)

using slovo::Json;

// ---------------------------------------------------------------------------

static void test_json_basics() {
    Json j = Json::parse(R"({"a": 1, "b": [true, null, "x"], "c": {"d": 2.5}})");
    CHECK(j.is_object());
    CHECK_EQ(j["a"].integer(), 1);
    CHECK(j["b"].is_array());
    CHECK_EQ(j["b"].size(), 3u);
    CHECK_EQ(j["b"][0].boolean(), true);
    CHECK(j["b"][1].is_null());
    CHECK_EQ(j["b"][2].str(), "x");
    CHECK_EQ(j["c"]["d"].number(), 2.5);

    // safe chaining through missing keys
    CHECK(j["nope"]["deeper"][7].is_null());
    CHECK_EQ(j["nope"].str(), "");

    // building
    Json b;
    b["model"] = "test";
    b["n"] = 42;
    b["t"] = 0.5;
    b["flag"] = true;
    b["list"].push_back(1);
    b["list"].push_back("two");
    CHECK_EQ(b.dump(), R"({"model":"test","n":42,"t":0.5,"flag":true,"list":[1,"two"]})");
}

static void test_json_roundtrip() {
    const std::string doc =
        R"({"s":"line\nbreak \"q\" \\ back","i":-9007199254740993,"f":3.141592653589793,)"
        R"("e":1e-7,"deep":[[[{"k":[]}]]],"empty":{},"u":"Жи: 😀"})";
    Json j = Json::parse(doc);
    Json j2 = Json::parse(j.dump());
    CHECK(j == j2);
    CHECK_EQ(j["s"].str(), "line\nbreak \"q\" \\ back");
    CHECK_EQ(j["i"].integer(), -9007199254740993LL);
    CHECK_EQ(j["f"].number(), 3.141592653589793);
    CHECK_EQ(j["e"].number(), 1e-7);
    // "Жи: 😀" in UTF-8
    CHECK_EQ(j["u"].str(), "\xD0\x96\xD0\xB8: \xF0\x9F\x98\x80");

    Json pretty = Json::parse(j.dump(2));
    CHECK(pretty == j);
}

static void test_json_errors() {
    CHECK_THROWS(Parse, Json::parse(""));
    CHECK_THROWS(Parse, Json::parse("{"));
    CHECK_THROWS(Parse, Json::parse("[1,]"));
    CHECK_THROWS(Parse, Json::parse("{\"a\" 1}"));
    CHECK_THROWS(Parse, Json::parse("\"unterminated"));
    CHECK_THROWS(Parse, Json::parse("nul"));
    CHECK_THROWS(Parse, Json::parse("1 2"));
    CHECK_THROWS(Parse, Json::parse("\"bad \\u12\""));
    CHECK_THROWS(Parse, Json::parse("\"lone \\uD800 surrogate\""));
    std::string deep(1000, '[');
    CHECK_THROWS(Parse, Json::parse(deep));
}

static void test_json_order_preserved() {
    Json j = Json::parse(R"({"z":1,"a":2,"m":3})");
    const auto& entries = j.entries();
    CHECK_EQ(entries.size(), 3u);
    CHECK_EQ(entries[0].first, "z");
    CHECK_EQ(entries[1].first, "a");
    CHECK_EQ(entries[2].first, "m");
}

// ---------------------------------------------------------------------------

static void test_url_parsing() {
    auto u = slovo::detail::parse_url("https://api.openai.com/v1/chat/completions");
    CHECK_EQ(u.host, "api.openai.com");
    CHECK_EQ(u.port, 443);
    CHECK(u.https);
    CHECK_EQ(u.target, "/v1/chat/completions");

    u = slovo::detail::parse_url("http://127.0.0.1:11434/v1/chat?x=1");
    CHECK_EQ(u.host, "127.0.0.1");
    CHECK_EQ(u.port, 11434);
    CHECK(!u.https);
    CHECK_EQ(u.target, "/v1/chat?x=1");

    u = slovo::detail::parse_url("http://[::1]:8080/v1");
    CHECK_EQ(u.host, "::1");
    CHECK_EQ(u.port, 8080);

    u = slovo::detail::parse_url("http://localhost");
    CHECK_EQ(u.port, 80);
    CHECK_EQ(u.target, "/");

    CHECK_THROWS(Usage, slovo::detail::parse_url("ftp://x/"));
    CHECK_THROWS(Usage, slovo::detail::parse_url("https:///path"));
}

// ---------------------------------------------------------------------------

static void test_sse_parser() {
    // Feed byte-by-byte to exercise buffering.
    const std::string stream =
        ": comment\r\n"
        "data: {\"a\":1}\r\n"
        "\r\n"
        "data: part1\n"
        "data: part2\n"
        "\n"
        "data: [DONE]\n\n";
    std::vector<std::string> events;
    slovo::SseParser sse;
    for (char c : stream)
        sse.feed(&c, 1, [&](const std::string& d) { events.push_back(d); });
    sse.finish([&](const std::string& d) { events.push_back(d); });

    CHECK_EQ(events.size(), 3u);
    CHECK_EQ(events[0], "{\"a\":1}");
    CHECK_EQ(events[1], "part1\npart2");  // multi-line data joins with \n
    CHECK_EQ(events[2], "[DONE]");

    // Trailing event without a final blank line is flushed by finish().
    slovo::SseParser sse2;
    std::vector<std::string> events2;
    std::string tail = "data: last";
    sse2.feed(tail.data(), tail.size(), [&](const std::string& d) { events2.push_back(d); });
    CHECK_EQ(events2.size(), 0u);
    sse2.finish([&](const std::string& d) { events2.push_back(d); });
    CHECK_EQ(events2.size(), 1u);
    CHECK_EQ(events2[0], "last");
}

// ---------------------------------------------------------------------------

static void test_build_chat_body() {
    std::vector<slovo::Message> messages{
        slovo::Message::system("be brief"),
        slovo::Message::user("hi"),
    };
    slovo::Options options;
    options.temperature = 0.0;
    options.max_tokens = 100;
    options.stop = {"END"};
    options.json_mode = true;
    options.extra["seed"] = 7;

    Json body = slovo::detail::build_chat_body(messages, options, "test-model", false);
    CHECK_EQ(body["model"].str(), "test-model");
    CHECK_EQ(body["messages"].size(), 2u);
    CHECK_EQ(body["messages"][0]["role"].str(), "system");
    CHECK_EQ(body["messages"][1]["content"].str(), "hi");
    CHECK_EQ(body["temperature"].number(), 0.0);
    CHECK(body["temperature"].is_number());  // temperature=0 must not be dropped
    CHECK_EQ(body["max_tokens"].integer(), 100);
    CHECK_EQ(body["stop"][0].str(), "END");
    CHECK_EQ(body["response_format"]["type"].str(), "json_object");
    CHECK_EQ(body["seed"].integer(), 7);
    CHECK(body["stream"].is_null());

    // tool definitions and assistant tool_calls round-trip
    slovo::Options with_tools;
    with_tools.tools.push_back({"get_time", "current time",
                                Json::parse(R"({"type":"object","properties":{}})")});
    with_tools.tool_choice = "auto";

    slovo::Message assistant;
    assistant.role = slovo::Role::assistant;
    assistant.tool_calls.push_back({"call_1", "get_time", "{}"});
    std::vector<slovo::Message> messages2{
        slovo::Message::user("time?"),
        assistant,
        slovo::Message::tool_result("call_1", "12:00", "get_time"),
    };
    Json body2 = slovo::detail::build_chat_body(messages2, with_tools, "m", true);
    CHECK_EQ(body2["tools"][0]["function"]["name"].str(), "get_time");
    CHECK_EQ(body2["tool_choice"].str(), "auto");
    CHECK_EQ(body2["stream"].boolean(), true);
    CHECK_EQ(body2["messages"][1]["tool_calls"][0]["id"].str(), "call_1");
    CHECK_EQ(body2["messages"][2]["role"].str(), "tool");
    CHECK_EQ(body2["messages"][2]["tool_call_id"].str(), "call_1");
}

static void test_parse_reply() {
    Json j = Json::parse(R"({
        "id": "chatcmpl-1", "model": "test-model",
        "choices": [{"index": 0, "finish_reason": "stop",
                     "message": {"role": "assistant", "content": "hello"}}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 2, "total_tokens": 12}
    })");
    slovo::Reply reply = slovo::detail::parse_reply(j);
    CHECK_EQ(reply.content, "hello");
    CHECK_EQ(reply.finish_reason, "stop");
    CHECK_EQ(reply.model, "test-model");
    CHECK_EQ(reply.usage.total_tokens, 12);
    CHECK(!reply.wants_tools());

    Json with_tools = Json::parse(R"({
        "choices": [{"finish_reason": "tool_calls",
                     "message": {"role": "assistant", "content": null,
                                 "tool_calls": [{"id": "c1", "type": "function",
                                                 "function": {"name": "f", "arguments": "{\"x\":1}"}}]}}]
    })");
    slovo::Reply reply2 = slovo::detail::parse_reply(with_tools);
    CHECK(reply2.wants_tools());
    CHECK_EQ(reply2.tool_calls[0].name, "f");
    CHECK_EQ(reply2.tool_calls[0].arguments, "{\"x\":1}");

    CHECK_THROWS(Http, slovo::detail::parse_reply(
        Json::parse(R"({"error":{"message":"bad key"}})")));
    CHECK_THROWS(Parse, slovo::detail::parse_reply(Json::parse(R"({"foo":1})")));
}

static void test_stream_accumulator() {
    slovo::detail::StreamAccumulator acc;
    acc.feed(Json::parse(R"({"model":"m","choices":[{"delta":{"role":"assistant","content":"Hel"}}]})"));
    CHECK_EQ(acc.last_text, "Hel");
    acc.feed(Json::parse(R"({"choices":[{"delta":{"content":"lo"}}]})"));
    acc.feed(Json::parse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"));
    acc.feed(Json::parse(R"({"choices":[],"usage":{"prompt_tokens":1,"completion_tokens":2,"total_tokens":3}})"));
    CHECK_EQ(acc.reply.content, "Hello");
    CHECK_EQ(acc.reply.finish_reason, "stop");
    CHECK_EQ(acc.reply.model, "m");
    CHECK_EQ(acc.reply.usage.total_tokens, 3);

    // incremental tool-call fragments merged by index
    slovo::detail::StreamAccumulator acc2;
    acc2.feed(Json::parse(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"id":"c1","function":{"name":"weather","arguments":""}}]}}]})"));
    acc2.feed(Json::parse(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"function":{"arguments":"{\"city\":"}}]}}]})"));
    acc2.feed(Json::parse(R"({"choices":[{"delta":{"tool_calls":[
        {"index":0,"function":{"arguments":"\"Oslo\"}"}}]}}]})"));
    CHECK_EQ(acc2.reply.tool_calls.size(), 1u);
    CHECK_EQ(acc2.reply.tool_calls[0].id, "c1");
    CHECK_EQ(acc2.reply.tool_calls[0].name, "weather");
    CHECK_EQ(acc2.reply.tool_calls[0].arguments, "{\"city\":\"Oslo\"}");
}

// ---------------------------------------------------------------------------
//  Client tests against a scripted transport
// ---------------------------------------------------------------------------

struct FakeTransport : slovo::Transport {
    struct Scripted {
        int status;
        std::string body;
        bool stream_it = false;  // deliver body through the sink in small pieces
    };
    std::deque<Scripted> script;
    std::vector<slovo::HttpRequest> seen;

    slovo::HttpResponse send(const slovo::HttpRequest& request,
                             const slovo::ChunkSink& sink) override {
        seen.push_back(request);
        if (script.empty())
            throw slovo::Error(slovo::Error::Kind::Usage, "FakeTransport: script exhausted");
        Scripted s = script.front();
        script.pop_front();

        slovo::HttpResponse response;
        response.status = s.status;
        if (sink && s.status / 100 == 2 && s.stream_it) {
            for (std::size_t i = 0; i < s.body.size(); i += 7)
                if (!sink(s.body.data() + i, std::min<std::size_t>(7, s.body.size() - i)))
                    break;
        } else {
            response.body = s.body;
        }
        return response;
    }
};

static slovo::Client make_client(std::shared_ptr<FakeTransport> transport) {
    slovo::Config config;
    config.base_url = "http://fake.local/v1";
    config.api_key = "sk-test";
    config.model = "fake-model";
    config.max_retries = 2;
    config.transport = transport;
    return slovo::Client(std::move(config));
}

static void test_client_chat() {
    auto transport = std::make_shared<FakeTransport>();
    transport->script.push_back({200, R"({"model":"fake-model",
        "choices":[{"message":{"role":"assistant","content":"pong"},"finish_reason":"stop"}],
        "usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}})"});
    auto client = make_client(transport);

    slovo::Reply reply = client.chat("ping");
    CHECK_EQ(reply.content, "pong");
    CHECK_EQ(transport->seen.size(), 1u);
    CHECK_EQ(transport->seen[0].url, "http://fake.local/v1/chat/completions");

    // auth + content-type headers present
    bool has_auth = false, has_ct = false;
    for (const auto& h : transport->seen[0].headers) {
        if (h.name == "Authorization" && h.value == "Bearer sk-test") has_auth = true;
        if (h.name == "Content-Type" && h.value == "application/json") has_ct = true;
    }
    CHECK(has_auth);
    CHECK(has_ct);

    Json sent = Json::parse(transport->seen[0].body);
    CHECK_EQ(sent["model"].str(), "fake-model");
    CHECK_EQ(sent["messages"][0]["content"].str(), "ping");
}

static void test_client_retry_and_errors() {
    auto transport = std::make_shared<FakeTransport>();
    transport->script.push_back({429, R"({"error":{"message":"slow down"}})"});
    transport->script.push_back({500, "oops"});
    transport->script.push_back({200, R"({"choices":[{"message":{"content":"ok"}}]})"});
    auto client = make_client(transport);
    slovo::Reply reply = client.chat("x");
    CHECK_EQ(reply.content, "ok");
    CHECK_EQ(transport->seen.size(), 3u);

    // non-retryable status fails immediately
    auto transport2 = std::make_shared<FakeTransport>();
    transport2->script.push_back({401, R"({"error":{"message":"bad key"}})"});
    auto client2 = make_client(transport2);
    try {
        client2.chat("x");
        CHECK(false);
    } catch (const slovo::Error& e) {
        CHECK(e.kind == slovo::Error::Kind::Http);
        CHECK_EQ(e.status, 401);
        CHECK(e.body.find("bad key") != std::string::npos);
    }
    CHECK_EQ(transport2->seen.size(), 1u);

    // retries are capped at max_retries
    auto transport3 = std::make_shared<FakeTransport>();
    for (int i = 0; i < 3; ++i) transport3->script.push_back({503, "busy"});
    auto client3 = make_client(transport3);
    CHECK_THROWS(Http, client3.chat("x"));
    CHECK_EQ(transport3->seen.size(), 3u);  // 1 attempt + 2 retries
}

static void test_client_stream() {
    std::string sse;
    sse += "data: {\"model\":\"m\",\"choices\":[{\"delta\":{\"content\":\"Wo\"}}]}\n\n";
    sse += "data: {\"choices\":[{\"delta\":{\"content\":\"rd\"}}]}\n\n";
    sse += "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    sse += "data: [DONE]\n\n";

    auto transport = std::make_shared<FakeTransport>();
    transport->script.push_back({200, sse, true});
    auto client = make_client(transport);

    std::string streamed;
    slovo::Reply reply = client.stream("hi", [&](const std::string& t) { streamed += t; });
    CHECK_EQ(streamed, "Word");
    CHECK_EQ(reply.content, "Word");
    CHECK_EQ(reply.finish_reason, "stop");
    Json sent = Json::parse(transport->seen[0].body);
    CHECK_EQ(sent["stream"].boolean(), true);
}

static void test_client_tool_loop() {
    auto transport = std::make_shared<FakeTransport>();
    // Round 1: the model asks for a tool call.
    transport->script.push_back({200, R"({"choices":[{"finish_reason":"tool_calls",
        "message":{"role":"assistant","content":"",
                   "tool_calls":[{"id":"c1","type":"function",
                                  "function":{"name":"add","arguments":"{\"a\":2,\"b\":3}"}}]}}]})"});
    // Round 2: final answer.
    transport->script.push_back({200, R"({"choices":[{"finish_reason":"stop",
        "message":{"role":"assistant","content":"2+3=5"}}]})"});
    auto client = make_client(transport);

    int calls = 0;
    slovo::ToolBox toolbox;
    toolbox.add("add", "adds two numbers",
                Json::parse(R"({"type":"object","properties":{
                    "a":{"type":"number"},"b":{"type":"number"}}})"),
                [&](const Json& args) {
                    ++calls;
                    return std::to_string(args["a"].integer() + args["b"].integer());
                });

    std::vector<slovo::Message> messages{slovo::Message::user("what is 2+3?")};
    slovo::Reply reply = client.chat_tools(messages, toolbox);
    CHECK_EQ(reply.content, "2+3=5");
    CHECK_EQ(calls, 1);
    // conversation grew: user, assistant(tool_calls), tool result
    CHECK_EQ(messages.size(), 3u);
    CHECK(messages[1].role == slovo::Role::assistant);
    CHECK_EQ(messages[2].content, "5");

    // second request must contain the tool result
    Json second = Json::parse(transport->seen[1].body);
    CHECK_EQ(second["messages"][2]["role"].str(), "tool");
    CHECK_EQ(second["messages"][2]["content"].str(), "5");
}

static void test_client_embeddings() {
    auto transport = std::make_shared<FakeTransport>();
    transport->script.push_back({200, R"({"data":[
        {"index":1,"embedding":[0.4,0.5]},
        {"index":0,"embedding":[0.1,0.2,0.3]}]})"});
    auto client = make_client(transport);
    auto vectors = client.embed({"one", "two"}, "embed-model");
    CHECK_EQ(vectors.size(), 2u);
    CHECK_EQ(vectors[0].size(), 3u);       // order restored by index
    CHECK_EQ(vectors[1].size(), 2u);
    CHECK(vectors[0][0] > 0.09f && vectors[0][0] < 0.11f);
    Json sent = Json::parse(transport->seen[0].body);
    CHECK_EQ(sent["model"].str(), "embed-model");
    CHECK_EQ(sent["input"][1].str(), "two");
}

// ---------------------------------------------------------------------------

int main() {
    test_json_basics();
    test_json_roundtrip();
    test_json_errors();
    test_json_order_preserved();
    test_url_parsing();
    test_sse_parser();
    test_build_chat_body();
    test_parse_reply();
    test_stream_accumulator();
    test_client_chat();
    test_client_retry_and_errors();
    test_client_stream();
    test_client_tool_loop();
    test_client_embeddings();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
