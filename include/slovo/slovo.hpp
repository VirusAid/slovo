// ============================================================================
//  slovo — a tiny, zero-dependency, single-header C++17 client for LLM APIs
//
//  "Slovo" (слово) is the Russian word for "word".
//
//  Works with every OpenAI-compatible chat endpoint: OpenAI, OpenRouter,
//  Groq, DeepSeek, Mistral, Together, Ollama, llama.cpp server, LM Studio...
//
//  Features:
//    * single header, no external dependencies
//    * built-in JSON parser/serializer
//    * built-in HTTP transports:
//        - WinHTTP on Windows (http + https)
//        - plain sockets everywhere (http, great for local models)
//        - libcurl if compiled with SLOVO_USE_CURL (http + https)
//    * blocking and streaming (SSE) chat completions
//    * tool calling with an automatic tool-execution loop
//    * embeddings
//    * retries with exponential backoff
//
//  Quick start:
//
//      #include <slovo/slovo.hpp>
//
//      auto llm = slovo::Client::openai(std::getenv("OPENAI_API_KEY"));
//      auto reply = llm.chat("Why is the sky blue?");
//      std::cout << reply.content << "\n";
//
//  https://github.com/VirusAid/slovo — MIT License
// ============================================================================
#ifndef SLOVO_HPP_INCLUDED
#define SLOVO_HPP_INCLUDED

#define SLOVO_VERSION_MAJOR 0
#define SLOVO_VERSION_MINOR 1
#define SLOVO_VERSION_PATCH 0
#define SLOVO_VERSION "0.1.0"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifndef SLOVO_NO_WINHTTP
#    include <winhttp.h>
#    if defined(_MSC_VER)
#      pragma comment(lib, "winhttp.lib")
#    endif
#  endif
#  if defined(_MSC_VER)
#    pragma comment(lib, "ws2_32.lib")
#  endif
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

#if defined(SLOVO_USE_CURL)
#  include <curl/curl.h>
#endif

namespace slovo {

// ============================================================================
//  Error
// ============================================================================

class Error : public std::runtime_error {
public:
    enum class Kind {
        Network,  // DNS, connect, timeout, TLS, broken pipe...
        Http,     // non-2xx response from the server
        Parse,    // malformed JSON / unexpected response shape
        Usage     // the library was used incorrectly
    };

    Error(Kind kind, const std::string& message, int status = 0, std::string body = "")
        : std::runtime_error(message), kind(kind), status(status), body(std::move(body)) {}

    Kind kind;
    int status;        // HTTP status code (Kind::Http), otherwise 0
    std::string body;  // raw response body, when available
};

// ============================================================================
//  Json — a small, self-contained JSON value
//
//  Object keys keep their insertion order. Integers up to int64 round-trip
//  exactly; other numbers are stored as double.
// ============================================================================

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using array_t  = std::vector<Json>;
    using object_t = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), int_(v), is_int_(true) {}
    Json(long long v) : type_(Type::Number), int_(v), is_int_(true) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json array()  { Json j; j.type_ = Type::Array;  return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null()   const { return type_ == Type::Null; }
    bool is_bool()   const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array()  const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool boolean(bool fallback = false) const { return is_bool() ? bool_ : fallback; }

    double number(double fallback = 0.0) const {
        if (!is_number()) return fallback;
        return is_int_ ? static_cast<double>(int_) : num_;
    }

    long long integer(long long fallback = 0) const {
        if (!is_number()) return fallback;
        return is_int_ ? int_ : static_cast<long long>(num_);
    }

    const std::string& str() const {
        static const std::string empty;
        return is_string() ? str_ : empty;
    }

    std::size_t size() const {
        if (is_array())  return arr_.size();
        if (is_object()) return obj_.size();
        return 0;
    }

    // --- object access -------------------------------------------------

    bool contains(const std::string& key) const {
        if (!is_object()) return false;
        for (const auto& kv : obj_)
            if (kv.first == key) return true;
        return false;
    }

    // Missing keys and type mismatches yield a shared immutable null value,
    // so lookups can be chained safely: j["choices"][0]["message"]["content"]
    const Json& operator[](const std::string& key) const {
        if (is_object())
            for (const auto& kv : obj_)
                if (kv.first == key) return kv.second;
        return null_ref();
    }

    Json& operator[](const std::string& key) {
        if (is_null()) type_ = Type::Object;
        if (!is_object()) throw Error(Error::Kind::Usage, "Json::operator[]: not an object");
        for (auto& kv : obj_)
            if (kv.first == key) return kv.second;
        obj_.emplace_back(key, Json());
        return obj_.back().second;
    }

    // --- array access ---------------------------------------------------

    const Json& operator[](std::size_t index) const {
        if (is_array() && index < arr_.size()) return arr_[index];
        return null_ref();
    }
    const Json& operator[](int index) const {
        if (index < 0) return null_ref();
        return (*this)[static_cast<std::size_t>(index)];
    }

    // Non-const indexing follows nlohmann-style semantics: null values turn
    // into arrays and the array grows to fit the index.
    Json& operator[](std::size_t index) {
        if (is_null()) type_ = Type::Array;
        if (!is_array()) throw Error(Error::Kind::Usage, "Json::operator[]: not an array");
        if (index >= arr_.size()) arr_.resize(index + 1);
        return arr_[index];
    }
    Json& operator[](int index) {
        if (index < 0) throw Error(Error::Kind::Usage, "Json::operator[]: negative index");
        return (*this)[static_cast<std::size_t>(index)];
    }

    Json& push_back(Json value) {
        if (is_null()) type_ = Type::Array;
        if (!is_array()) throw Error(Error::Kind::Usage, "Json::push_back: not an array");
        arr_.push_back(std::move(value));
        return arr_.back();
    }

    const array_t&  items()   const { static const array_t  empty; return is_array()  ? arr_ : empty; }
    const object_t& entries() const { static const object_t empty; return is_object() ? obj_ : empty; }

    // --- serialization ----------------------------------------------------

    std::string dump(int indent = -1) const {
        std::string out;
        write(out, indent, 0);
        return out;
    }

    // Throws Error(Kind::Parse) on malformed input.
    static Json parse(const std::string& text) {
        Parser p{text.data(), text.data() + text.size(), text.data(), 0};
        Json v = p.parse_value();
        p.skip_ws();
        if (p.cur != p.end) p.fail("trailing characters after JSON value");
        return v;
    }

    bool operator==(const Json& other) const {
        if (type_ != other.type_) return false;
        switch (type_) {
            case Type::Null:   return true;
            case Type::Bool:   return bool_ == other.bool_;
            case Type::Number: return number() == other.number();
            case Type::String: return str_ == other.str_;
            case Type::Array:  return arr_ == other.arr_;
            case Type::Object: return obj_ == other.obj_;
        }
        return false;
    }
    bool operator!=(const Json& other) const { return !(*this == other); }

private:
    static const Json& null_ref() {
        static const Json null_value;
        return null_value;
    }

    static void write_escaped(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    static void write_number(std::string& out, double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        // %.17g guarantees round-trip but is often longer than needed;
        // prefer the shortest representation that still round-trips.
        for (int prec = 1; prec < 17; ++prec) {
            char shorter[32];
            std::snprintf(shorter, sizeof shorter, "%.*g", prec, v);
            if (std::strtod(shorter, nullptr) == v) {
                std::memcpy(buf, shorter, sizeof shorter);
                break;
            }
        }
        out += buf;
    }

    void write(std::string& out, int indent, int depth) const {
        auto newline = [&](int d) {
            if (indent >= 0) {
                out += '\n';
                out.append(static_cast<std::size_t>(indent) * d, ' ');
            }
        };
        switch (type_) {
            case Type::Null:   out += "null"; break;
            case Type::Bool:   out += bool_ ? "true" : "false"; break;
            case Type::String: write_escaped(out, str_); break;
            case Type::Number:
                if (is_int_) out += std::to_string(int_);
                else write_number(out, num_);
                break;
            case Type::Array: {
                if (arr_.empty()) { out += "[]"; break; }
                out += '[';
                for (std::size_t i = 0; i < arr_.size(); ++i) {
                    if (i) out += ',';
                    newline(depth + 1);
                    arr_[i].write(out, indent, depth + 1);
                }
                newline(depth);
                out += ']';
                break;
            }
            case Type::Object: {
                if (obj_.empty()) { out += "{}"; break; }
                out += '{';
                for (std::size_t i = 0; i < obj_.size(); ++i) {
                    if (i) out += ',';
                    newline(depth + 1);
                    write_escaped(out, obj_[i].first);
                    out += ':';
                    if (indent >= 0) out += ' ';
                    obj_[i].second.write(out, indent, depth + 1);
                }
                newline(depth);
                out += '}';
                break;
            }
        }
    }

    struct Parser {
        const char* cur;
        const char* end;
        const char* begin;
        int depth;

        static constexpr int max_depth = 256;

        [[noreturn]] void fail(const std::string& why) {
            throw Error(Error::Kind::Parse,
                        "JSON parse error at offset " + std::to_string(cur - begin) + ": " + why);
        }

        void skip_ws() {
            while (cur != end && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r'))
                ++cur;
        }

        char peek() {
            if (cur == end) fail("unexpected end of input");
            return *cur;
        }

        void expect(char c) {
            if (cur == end || *cur != c) fail(std::string("expected '") + c + "'");
            ++cur;
        }

        bool consume_literal(const char* lit) {
            std::size_t n = std::strlen(lit);
            if (static_cast<std::size_t>(end - cur) < n || std::memcmp(cur, lit, n) != 0)
                return false;
            cur += n;
            return true;
        }

        Json parse_value() {
            skip_ws();
            switch (peek()) {
                case 'n': if (consume_literal("null"))  return Json();     fail("invalid literal");
                case 't': if (consume_literal("true"))  return Json(true);  fail("invalid literal");
                case 'f': if (consume_literal("false")) return Json(false); fail("invalid literal");
                case '"': return parse_string();
                case '[': return parse_array();
                case '{': return parse_object();
                default:  return parse_number();
            }
        }

        Json parse_array() {
            if (++depth > max_depth) fail("nesting too deep");
            expect('[');
            Json j = Json::array();
            skip_ws();
            if (peek() == ']') { ++cur; --depth; return j; }
            for (;;) {
                j.push_back(parse_value());
                skip_ws();
                char c = peek();
                if (c == ',') { ++cur; continue; }
                if (c == ']') { ++cur; break; }
                fail("expected ',' or ']' in array");
            }
            --depth;
            return j;
        }

        Json parse_object() {
            if (++depth > max_depth) fail("nesting too deep");
            expect('{');
            Json j = Json::object();
            skip_ws();
            if (peek() == '}') { ++cur; --depth; return j; }
            for (;;) {
                skip_ws();
                if (peek() != '"') fail("expected object key");
                std::string key = parse_string().str_;
                skip_ws();
                expect(':');
                j.obj_.emplace_back(std::move(key), parse_value());
                skip_ws();
                char c = peek();
                if (c == ',') { ++cur; continue; }
                if (c == '}') { ++cur; break; }
                fail("expected ',' or '}' in object");
            }
            --depth;
            return j;
        }

        Json parse_number() {
            const char* start = cur;
            if (cur != end && *cur == '-') ++cur;
            bool is_int = true;
            while (cur != end) {
                char c = *cur;
                if (c >= '0' && c <= '9') { ++cur; continue; }
                if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                    if (c == '.' || c == 'e' || c == 'E') is_int = false;
                    ++cur;
                    continue;
                }
                break;
            }
            if (cur == start || (cur == start + 1 && *start == '-'))
                fail("invalid number");
            std::string tok(start, cur);
            if (is_int) {
                errno = 0;
                char* endp = nullptr;
                long long v = std::strtoll(tok.c_str(), &endp, 10);
                if (errno == 0 && endp == tok.c_str() + tok.size())
                    return Json(v);
            }
            errno = 0;
            char* endp = nullptr;
            double d = std::strtod(tok.c_str(), &endp);
            if (endp != tok.c_str() + tok.size()) fail("invalid number");
            return Json(d);
        }

        unsigned parse_hex4() {
            if (end - cur < 4) fail("truncated \\u escape");
            unsigned v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = *cur++;
                v <<= 4;
                if (c >= '0' && c <= '9') v += static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') v += static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v += static_cast<unsigned>(c - 'A' + 10);
                else fail("invalid \\u escape");
            }
            return v;
        }

        static void append_utf8(std::string& s, unsigned cp) {
            if (cp < 0x80) {
                s += static_cast<char>(cp);
            } else if (cp < 0x800) {
                s += static_cast<char>(0xC0 | (cp >> 6));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                s += static_cast<char>(0xE0 | (cp >> 12));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                s += static_cast<char>(0xF0 | (cp >> 18));
                s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        Json parse_string() {
            expect('"');
            std::string s;
            for (;;) {
                if (cur == end) fail("unterminated string");
                unsigned char c = static_cast<unsigned char>(*cur++);
                if (c == '"') break;
                if (c == '\\') {
                    if (cur == end) fail("unterminated escape");
                    char e = *cur++;
                    switch (e) {
                        case '"':  s += '"';  break;
                        case '\\': s += '\\'; break;
                        case '/':  s += '/';  break;
                        case 'b':  s += '\b'; break;
                        case 'f':  s += '\f'; break;
                        case 'n':  s += '\n'; break;
                        case 'r':  s += '\r'; break;
                        case 't':  s += '\t'; break;
                        case 'u': {
                            unsigned cp = parse_hex4();
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (end - cur >= 2 && cur[0] == '\\' && cur[1] == 'u') {
                                    cur += 2;
                                    unsigned lo = parse_hex4();
                                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    else
                                        fail("invalid low surrogate");
                                } else {
                                    fail("unpaired high surrogate");
                                }
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                fail("unpaired low surrogate");
                            }
                            append_utf8(s, cp);
                            break;
                        }
                        default: fail("invalid escape");
                    }
                } else if (c < 0x20) {
                    fail("unescaped control character in string");
                } else {
                    s += static_cast<char>(c);
                }
            }
            return Json(std::move(s));
        }
    };

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    long long int_ = 0;
    bool is_int_ = false;
    std::string str_;
    array_t arr_;
    object_t obj_;
};

// ============================================================================
//  HTTP layer
// ============================================================================

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    long timeout_ms = 120000;
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

// Streaming consumer: return false to cancel the transfer.
using ChunkSink = std::function<bool(const char* data, std::size_t size)>;

// Transports deliver 2xx bodies to `sink` when one is provided; non-2xx
// bodies are always accumulated into HttpResponse::body so the caller can
// build an error message.
class Transport {
public:
    virtual ~Transport() = default;
    virtual HttpResponse send(const HttpRequest& request, const ChunkSink& sink) = 0;
};

namespace detail {

struct Url {
    bool https = false;
    std::string host;
    int port = 0;
    std::string target;  // path + query, always starts with '/'
};

inline Url parse_url(const std::string& url) {
    Url u;
    std::string rest;
    if (url.rfind("https://", 0) == 0) { u.https = true;  rest = url.substr(8); }
    else if (url.rfind("http://", 0) == 0) { u.https = false; rest = url.substr(7); }
    else throw Error(Error::Kind::Usage, "unsupported URL scheme in: " + url);

    std::size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.target = (slash == std::string::npos) ? "/" : rest.substr(slash);

    if (!hostport.empty() && hostport[0] == '[') {  // [::1]:8080
        std::size_t close = hostport.find(']');
        if (close == std::string::npos) throw Error(Error::Kind::Usage, "bad IPv6 host in: " + url);
        u.host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == ':')
            u.port = std::atoi(hostport.c_str() + close + 2);
    } else {
        std::size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            u.host = hostport.substr(0, colon);
            u.port = std::atoi(hostport.c_str() + colon + 1);
        } else {
            u.host = hostport;
        }
    }
    if (u.port == 0) u.port = u.https ? 443 : 80;
    if (u.host.empty()) throw Error(Error::Kind::Usage, "missing host in: " + url);
    return u;
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace detail

// ----------------------------------------------------------------------------
//  SocketTransport — plain HTTP/1.1 over TCP. No TLS: intended for local
//  model servers (Ollama, llama.cpp, LM Studio) and tests. Supports
//  Content-Length, chunked transfer encoding, and read-to-close bodies.
// ----------------------------------------------------------------------------

class SocketTransport : public Transport {
public:
    HttpResponse send(const HttpRequest& request, const ChunkSink& sink) override {
        detail::Url url = detail::parse_url(request.url);
        if (url.https)
            throw Error(Error::Kind::Usage,
                        "SocketTransport is http-only; for https use WinHTTP (Windows default) "
                        "or build with SLOVO_USE_CURL and link libcurl");

        net_init();
        socket_t fd = connect_to(url.host, url.port, request.timeout_ms);
        struct Closer {
            socket_t fd;
            ~Closer() { close_socket(fd); }
        } closer{fd};

        send_all(fd, build_request_text(request, url));

        Receiver rx(fd, sink);
        rx.run();
        return std::move(rx.response);
    }

private:
#if defined(_WIN32)
    using socket_t = SOCKET;
    static constexpr socket_t invalid_socket = INVALID_SOCKET;
    static void close_socket(socket_t fd) { ::closesocket(fd); }
    static void net_init() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
        });
    }
    static void set_timeouts(socket_t fd, long ms) {
        DWORD t = static_cast<DWORD>(ms);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    }
#else
    using socket_t = int;
    static constexpr socket_t invalid_socket = -1;
    static void close_socket(socket_t fd) { ::close(fd); }
    static void net_init() {}
    static void set_timeouts(socket_t fd, long ms) {
        timeval t;
        t.tv_sec = ms / 1000;
        t.tv_usec = (ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
    }
#endif

    static socket_t connect_to(const std::string& host, int port, long timeout_ms) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results) != 0)
            throw Error(Error::Kind::Network, "failed to resolve host: " + host);

        socket_t fd = invalid_socket;
        for (addrinfo* ai = results; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd == invalid_socket) continue;
            set_timeouts(fd, timeout_ms);
            if (::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
            close_socket(fd);
            fd = invalid_socket;
        }
        freeaddrinfo(results);
        if (fd == invalid_socket)
            throw Error(Error::Kind::Network,
                        "failed to connect to " + host + ":" + std::to_string(port));
        return fd;
    }

    static void send_all(socket_t fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            auto n = ::send(fd, data.data() + sent,
#if defined(_WIN32)
                            static_cast<int>(data.size() - sent),
#else
                            data.size() - sent,
#endif
                            0);
            if (n <= 0) throw Error(Error::Kind::Network, "failed to send request");
            sent += static_cast<std::size_t>(n);
        }
    }

    static std::string build_request_text(const HttpRequest& request, const detail::Url& url) {
        std::string text = request.method + " " + url.target + " HTTP/1.1\r\n";
        text += "Host: " + url.host;
        if (url.port != 80) text += ":" + std::to_string(url.port);
        text += "\r\n";
        for (const auto& h : request.headers)
            text += h.name + ": " + h.value + "\r\n";
        text += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
        text += "Accept-Encoding: identity\r\n";
        text += "Connection: close\r\n\r\n";
        text += request.body;
        return text;
    }

    // Incremental HTTP/1.1 response reader: parses the status line and
    // headers, then de-chunks or length-delimits the body, feeding it to
    // the sink as it arrives.
    struct Receiver {
        Receiver(socket_t fd, const ChunkSink& sink) : fd(fd), sink(sink) {}

        socket_t fd;
        const ChunkSink& sink;
        HttpResponse response;

        std::string header_buf;
        bool headers_done = false;
        bool chunked = false;
        long long content_length = -1;   // -1: read until close
        long long body_received = 0;
        bool cancelled = false;
        bool done = false;

        enum class ChunkState { Size, Data, DataCr, DataLf, Trailer } chunk_state = ChunkState::Size;
        std::string chunk_size_line;
        long long chunk_remaining = 0;

        void run() {
            char buf[16384];
            for (;;) {
                auto n = ::recv(fd, buf, sizeof buf, 0);
                if (n < 0) throw Error(Error::Kind::Network, "read failed or timed out");
                if (n == 0) break;
                consume(buf, static_cast<std::size_t>(n));
                if (done || cancelled) return;
            }
            if (!headers_done)
                throw Error(Error::Kind::Network, "connection closed before response headers");
            if (chunked && !done)
                throw Error(Error::Kind::Network, "connection closed mid-chunk");
            if (content_length >= 0 && body_received < content_length)
                throw Error(Error::Kind::Network, "connection closed before full body");
        }

        void consume(const char* data, std::size_t size) {
            if (!headers_done) {
                header_buf.append(data, size);
                std::size_t pos = header_buf.find("\r\n\r\n");
                if (pos == std::string::npos) {
                    if (header_buf.size() > 1024 * 1024)
                        throw Error(Error::Kind::Network, "response headers too large");
                    return;
                }
                parse_headers(header_buf.substr(0, pos));
                headers_done = true;
                std::string leftover = header_buf.substr(pos + 4);
                header_buf.clear();
                if (!leftover.empty()) consume_body(leftover.data(), leftover.size());
                if (!chunked && content_length == 0) done = true;
                return;
            }
            consume_body(data, size);
        }

        void parse_headers(const std::string& text) {
            std::size_t line_end = text.find("\r\n");
            std::string status_line = text.substr(0, line_end);
            // "HTTP/1.1 200 OK"
            std::size_t sp = status_line.find(' ');
            if (sp == std::string::npos)
                throw Error(Error::Kind::Network, "malformed status line: " + status_line);
            response.status = std::atoi(status_line.c_str() + sp + 1);

            std::size_t pos = (line_end == std::string::npos) ? text.size() : line_end + 2;
            while (pos < text.size()) {
                std::size_t eol = text.find("\r\n", pos);
                std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos
                                                                             : eol - pos);
                pos = (eol == std::string::npos) ? text.size() : eol + 2;
                std::size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = detail::lower(line.substr(0, colon));
                std::size_t vstart = line.find_first_not_of(" \t", colon + 1);
                std::string value = (vstart == std::string::npos) ? "" : line.substr(vstart);
                if (name == "transfer-encoding" &&
                    detail::lower(value).find("chunked") != std::string::npos)
                    chunked = true;
                else if (name == "content-length")
                    content_length = std::atoll(value.c_str());
            }
        }

        void deliver(const char* data, std::size_t size) {
            if (size == 0) return;
            body_received += static_cast<long long>(size);
            if (sink && response.status / 100 == 2) {
                if (!sink(data, size)) cancelled = true;
            } else {
                response.body.append(data, size);
            }
        }

        void consume_body(const char* data, std::size_t size) {
            if (!chunked) {
                if (content_length >= 0) {
                    long long want = content_length - body_received;
                    std::size_t take = static_cast<std::size_t>(
                        std::min<long long>(want, static_cast<long long>(size)));
                    deliver(data, take);
                    if (body_received >= content_length) done = true;
                } else {
                    deliver(data, size);
                }
                return;
            }
            // De-chunk incrementally.
            std::size_t i = 0;
            while (i < size && !done && !cancelled) {
                switch (chunk_state) {
                    case ChunkState::Size: {
                        char c = data[i++];
                        if (c == '\n') {
                            chunk_remaining = std::strtoll(chunk_size_line.c_str(), nullptr, 16);
                            chunk_size_line.clear();
                            if (chunk_remaining == 0) {
                                chunk_state = ChunkState::Trailer;
                                done = true;  // trailers are ignored
                            } else {
                                chunk_state = ChunkState::Data;
                            }
                        } else if (c != '\r') {
                            chunk_size_line += c;
                            if (chunk_size_line.size() > 32)
                                throw Error(Error::Kind::Network, "malformed chunk size");
                        }
                        break;
                    }
                    case ChunkState::Data: {
                        std::size_t take = static_cast<std::size_t>(std::min<long long>(
                            chunk_remaining, static_cast<long long>(size - i)));
                        deliver(data + i, take);
                        i += take;
                        chunk_remaining -= static_cast<long long>(take);
                        if (chunk_remaining == 0) chunk_state = ChunkState::DataCr;
                        break;
                    }
                    case ChunkState::DataCr:
                        if (data[i] == '\r') ++i;
                        chunk_state = ChunkState::DataLf;
                        break;
                    case ChunkState::DataLf:
                        if (data[i] == '\n') ++i;
                        chunk_state = ChunkState::Size;
                        break;
                    case ChunkState::Trailer:
                        i = size;
                        break;
                }
            }
        }
    };
};

// ----------------------------------------------------------------------------
//  WinHttpTransport — Windows default, handles both http and https.
// ----------------------------------------------------------------------------

#if defined(_WIN32) && !defined(SLOVO_NO_WINHTTP)

#define SLOVO_WIDEN2(x) L##x
#define SLOVO_WIDEN(x) SLOVO_WIDEN2(x)
#define SLOVO_WVERSION SLOVO_WIDEN(SLOVO_VERSION)

#if defined(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_CONFIG)
#  define SLOVO_WINHTTP_PROXY WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_CONFIG
#else
#  define SLOVO_WINHTTP_PROXY WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
#endif

class WinHttpTransport : public Transport {
public:
    HttpResponse send(const HttpRequest& request, const ChunkSink& sink) override {
        detail::Url url = detail::parse_url(request.url);

        Handle session(WinHttpOpen(L"slovo/" SLOVO_WVERSION, SLOVO_WINHTTP_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session)
            throw net_error("WinHttpOpen");

        int t = static_cast<int>(request.timeout_ms);
        WinHttpSetTimeouts(session.h, t, t, t, t);

        Handle connection(WinHttpConnect(session.h, widen(url.host).c_str(),
                                         static_cast<INTERNET_PORT>(url.port), 0));
        if (!connection)
            throw net_error("WinHttpConnect");

        Handle req(WinHttpOpenRequest(connection.h, widen(request.method).c_str(),
                                      widen(url.target).c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      url.https ? WINHTTP_FLAG_SECURE : 0));
        if (!req)
            throw net_error("WinHttpOpenRequest");

        std::wstring headers;
        for (const auto& h : request.headers)
            headers += widen(h.name) + L": " + widen(h.value) + L"\r\n";
        if (!headers.empty() &&
            !WinHttpAddRequestHeaders(req.h, headers.c_str(),
                                      static_cast<DWORD>(headers.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD))
            throw net_error("WinHttpAddRequestHeaders");

        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                request.body.empty()
                                    ? WINHTTP_NO_REQUEST_DATA
                                    : const_cast<char*>(request.body.data()),
                                static_cast<DWORD>(request.body.size()),
                                static_cast<DWORD>(request.body.size()), 0))
            throw net_error("WinHttpSendRequest");

        if (!WinHttpReceiveResponse(req.h, nullptr))
            throw net_error("WinHttpReceiveResponse");

        HttpResponse response;
        DWORD status = 0, status_size = sizeof status;
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX))
            throw net_error("WinHttpQueryHeaders");
        response.status = static_cast<int>(status);

        std::vector<char> buffer;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req.h, &available))
                throw net_error("WinHttpQueryDataAvailable");
            if (available == 0) break;
            buffer.resize(available);
            DWORD read = 0;
            if (!WinHttpReadData(req.h, buffer.data(), available, &read))
                throw net_error("WinHttpReadData");
            if (read == 0) break;
            if (sink && response.status / 100 == 2) {
                if (!sink(buffer.data(), read)) break;
            } else {
                response.body.append(buffer.data(), read);
            }
        }
        return response;
    }

private:
    struct Handle {
        HINTERNET h;
        explicit Handle(HINTERNET h) : h(h) {}
        ~Handle() { if (h) WinHttpCloseHandle(h); }
        explicit operator bool() const { return h != nullptr; }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    static Error net_error(const char* where) {
        return Error(Error::Kind::Network,
                     std::string(where) + " failed (error " + std::to_string(GetLastError()) + ")");
    }

    static std::wstring widen(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
        return w;
    }
};

#endif  // _WIN32 && !SLOVO_NO_WINHTTP

// ----------------------------------------------------------------------------
//  CurlTransport — optional, enabled with SLOVO_USE_CURL (link libcurl).
// ----------------------------------------------------------------------------

#if defined(SLOVO_USE_CURL)

class CurlTransport : public Transport {
public:
    CurlTransport() {
        static std::once_flag flag;
        std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    HttpResponse send(const HttpRequest& request, const ChunkSink& sink) override {
        CURL* handle = curl_easy_init();
        if (!handle) throw Error(Error::Kind::Network, "curl_easy_init failed");

        struct Ctx {
            CURL* handle;
            const ChunkSink* sink;
            std::string body;
            bool cancelled = false;
        } ctx{handle, sink ? &sink : nullptr, {}, false};

        curl_slist* headers = nullptr;
        for (const auto& h : request.headers)
            headers = curl_slist_append(headers, (h.name + ": " + h.value).c_str());

        struct Cleanup {
            CURL* handle;
            curl_slist* headers;
            ~Cleanup() {
                if (headers) curl_slist_free_all(headers);
                curl_easy_cleanup(handle);
            }
        } cleanup{handle, headers};

        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, request.timeout_ms);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION,
                         +[](char* data, std::size_t size, std::size_t nmemb, void* userp) -> std::size_t {
                             auto* c = static_cast<Ctx*>(userp);
                             std::size_t total = size * nmemb;
                             long code = 0;
                             curl_easy_getinfo(c->handle, CURLINFO_RESPONSE_CODE, &code);
                             if (c->sink && code / 100 == 2) {
                                 if (!(*c->sink)(data, total)) {
                                     c->cancelled = true;
                                     return 0;  // aborts the transfer
                                 }
                             } else {
                                 c->body.append(data, total);
                             }
                             return total;
                         });

        CURLcode rc = curl_easy_perform(handle);
        if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && ctx.cancelled))
            throw Error(Error::Kind::Network,
                        std::string("curl: ") + curl_easy_strerror(rc));

        long code = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
        HttpResponse response;
        response.status = static_cast<int>(code);
        response.body = std::move(ctx.body);
        return response;
    }
};

#endif  // SLOVO_USE_CURL

inline std::shared_ptr<Transport> default_transport() {
#if defined(SLOVO_USE_CURL)
    return std::make_shared<CurlTransport>();
#elif defined(_WIN32) && !defined(SLOVO_NO_WINHTTP)
    return std::make_shared<WinHttpTransport>();
#else
    return std::make_shared<SocketTransport>();
#endif
}

// ============================================================================
//  SSE (Server-Sent Events) parser
// ============================================================================

class SseParser {
public:
    // Feed raw bytes; `on_event` receives each event's decoded data payload.
    void feed(const char* data, std::size_t size,
              const std::function<void(const std::string&)>& on_event) {
        buffer_.append(data, size);
        std::size_t pos = 0;
        for (;;) {
            std::size_t nl = buffer_.find('\n', pos);
            if (nl == std::string::npos) break;
            std::size_t len = nl - pos;
            if (len > 0 && buffer_[pos + len - 1] == '\r') --len;
            handle_line(buffer_.data() + pos, len, on_event);
            pos = nl + 1;
        }
        buffer_.erase(0, pos);
    }

    // Flush a trailing event that was not followed by a blank line.
    void finish(const std::function<void(const std::string&)>& on_event) {
        if (!buffer_.empty()) {
            std::size_t len = buffer_.size();
            if (buffer_[len - 1] == '\r') --len;
            handle_line(buffer_.data(), len, on_event);
            buffer_.clear();
        }
        dispatch(on_event);
    }

private:
    void handle_line(const char* line, std::size_t len,
                     const std::function<void(const std::string&)>& on_event) {
        if (len == 0) {  // blank line terminates the event
            dispatch(on_event);
            return;
        }
        if (line[0] == ':') return;  // comment
        static constexpr char prefix[] = "data:";
        if (len >= 5 && std::memcmp(line, prefix, 5) == 0) {
            std::size_t start = 5;
            if (start < len && line[start] == ' ') ++start;
            if (has_data_) data_ += '\n';
            data_.append(line + start, len - start);
            has_data_ = true;
        }
        // other fields (event:, id:, retry:) are irrelevant for LLM streams
    }

    void dispatch(const std::function<void(const std::string&)>& on_event) {
        if (!has_data_) return;
        std::string payload = std::move(data_);
        data_.clear();
        has_data_ = false;
        on_event(payload);
    }

    std::string buffer_;
    std::string data_;
    bool has_data_ = false;
};

// ============================================================================
//  Chat API types
// ============================================================================

enum class Role { system, user, assistant, tool };

inline const char* to_string(Role role) {
    switch (role) {
        case Role::system:    return "system";
        case Role::user:      return "user";
        case Role::assistant: return "assistant";
        case Role::tool:      return "tool";
    }
    return "user";
}

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // raw JSON string, as sent by the model
};

struct Message {
    Role role = Role::user;
    std::string content;
    std::vector<ToolCall> tool_calls;  // assistant messages only
    std::string tool_call_id;          // tool messages only
    std::string name;                  // optional participant / tool name

    static Message system(std::string text) {
        Message m; m.role = Role::system; m.content = std::move(text); return m;
    }
    static Message user(std::string text) {
        Message m; m.role = Role::user; m.content = std::move(text); return m;
    }
    static Message assistant(std::string text) {
        Message m; m.role = Role::assistant; m.content = std::move(text); return m;
    }
    static Message tool_result(std::string call_id, std::string content, std::string name = "") {
        Message m;
        m.role = Role::tool;
        m.tool_call_id = std::move(call_id);
        m.content = std::move(content);
        m.name = std::move(name);
        return m;
    }
};

struct Tool {
    std::string name;
    std::string description;
    Json parameters;  // JSON Schema of the arguments
};

struct Options {
    std::string model;                    // overrides Config::model
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> max_tokens;
    std::vector<std::string> stop;
    bool json_mode = false;               // response_format: json_object
    bool stream_usage = false;            // stream_options: include_usage
    std::vector<Tool> tools;
    std::string tool_choice;              // "auto" | "none" | "required" | ""
    Json extra;                           // extra fields merged into the request body
};

struct Usage {
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long total_tokens = 0;
};

struct Reply {
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
    std::string model;
    Usage usage;
    Json raw;  // full response (last chunk's metadata when streaming)

    bool wants_tools() const { return !tool_calls.empty(); }
};

namespace detail {

inline Json message_to_json(const Message& m) {
    Json j = Json::object();
    j["role"] = to_string(m.role);
    if (m.role == Role::tool) {
        j["tool_call_id"] = m.tool_call_id;
        j["content"] = m.content;
    } else {
        if (!m.content.empty() || m.tool_calls.empty())
            j["content"] = m.content;
        if (!m.tool_calls.empty()) {
            Json calls = Json::array();
            for (const auto& call : m.tool_calls) {
                Json c = Json::object();
                c["id"] = call.id;
                c["type"] = "function";
                c["function"]["name"] = call.name;
                c["function"]["arguments"] = call.arguments;
                calls.push_back(std::move(c));
            }
            j["tool_calls"] = std::move(calls);
        }
    }
    if (!m.name.empty()) j["name"] = m.name;
    return j;
}

inline Json build_chat_body(const std::vector<Message>& messages, const Options& options,
                            const std::string& model, bool stream) {
    Json body = Json::object();
    body["model"] = model;
    Json msgs = Json::array();
    for (const auto& m : messages) msgs.push_back(message_to_json(m));
    body["messages"] = std::move(msgs);

    if (options.temperature) body["temperature"] = *options.temperature;
    if (options.top_p)       body["top_p"] = *options.top_p;
    if (options.max_tokens)  body["max_tokens"] = *options.max_tokens;
    if (!options.stop.empty()) {
        Json stop = Json::array();
        for (const auto& s : options.stop) stop.push_back(s);
        body["stop"] = std::move(stop);
    }
    if (options.json_mode)
        body["response_format"]["type"] = "json_object";
    if (!options.tools.empty()) {
        Json tools = Json::array();
        for (const auto& t : options.tools) {
            Json tool = Json::object();
            tool["type"] = "function";
            tool["function"]["name"] = t.name;
            tool["function"]["description"] = t.description;
            tool["function"]["parameters"] = t.parameters;
            tools.push_back(std::move(tool));
        }
        body["tools"] = std::move(tools);
        if (!options.tool_choice.empty()) body["tool_choice"] = options.tool_choice;
    }
    if (stream) {
        body["stream"] = true;
        if (options.stream_usage)
            body["stream_options"]["include_usage"] = true;
    }
    for (const auto& kv : options.extra.entries())
        body[kv.first] = kv.second;
    return body;
}

inline std::vector<ToolCall> parse_tool_calls(const Json& calls) {
    std::vector<ToolCall> out;
    for (const auto& c : calls.items()) {
        ToolCall call;
        call.id = c["id"].str();
        call.name = c["function"]["name"].str();
        call.arguments = c["function"]["arguments"].str();
        out.push_back(std::move(call));
    }
    return out;
}

inline Usage parse_usage(const Json& u) {
    Usage usage;
    usage.prompt_tokens = u["prompt_tokens"].integer();
    usage.completion_tokens = u["completion_tokens"].integer();
    usage.total_tokens = u["total_tokens"].integer();
    return usage;
}

inline Reply parse_reply(const Json& j) {
    if (!j["error"].is_null()) {
        std::string message = j["error"]["message"].str();
        if (message.empty()) message = j["error"].dump();
        throw Error(Error::Kind::Http, "API error: " + message, 0, j.dump());
    }
    const Json& choice = j["choices"][0];
    if (choice.is_null())
        throw Error(Error::Kind::Parse, "response has no choices: " + j.dump());

    Reply reply;
    reply.raw = j;
    reply.model = j["model"].str();
    const Json& message = choice["message"];
    reply.content = message["content"].str();
    reply.tool_calls = parse_tool_calls(message["tool_calls"]);
    reply.finish_reason = choice["finish_reason"].str();
    reply.usage = parse_usage(j["usage"]);
    return reply;
}

// Accumulates streamed chunks into a complete Reply, merging incremental
// tool-call fragments by their index.
struct StreamAccumulator {
    Reply reply;
    std::string last_text;  // text delta of the most recent chunk

    void feed(const Json& chunk) {
        last_text.clear();
        if (!chunk["error"].is_null()) {
            std::string message = chunk["error"]["message"].str();
            if (message.empty()) message = chunk["error"].dump();
            throw Error(Error::Kind::Http, "API error: " + message, 0, chunk.dump());
        }
        reply.raw = chunk;
        if (!chunk["model"].str().empty()) reply.model = chunk["model"].str();
        if (!chunk["usage"].is_null()) reply.usage = parse_usage(chunk["usage"]);

        const Json& choice = chunk["choices"][0];
        if (choice.is_null()) return;
        if (!choice["finish_reason"].str().empty())
            reply.finish_reason = choice["finish_reason"].str();

        const Json& delta = choice["delta"];
        if (delta["content"].is_string()) {
            last_text = delta["content"].str();
            reply.content += last_text;
        }
        for (const auto& fragment : delta["tool_calls"].items()) {
            std::size_t index = static_cast<std::size_t>(fragment["index"].integer());
            if (reply.tool_calls.size() <= index) reply.tool_calls.resize(index + 1);
            ToolCall& call = reply.tool_calls[index];
            if (!fragment["id"].str().empty()) call.id = fragment["id"].str();
            if (!fragment["function"]["name"].str().empty())
                call.name += fragment["function"]["name"].str();
            call.arguments += fragment["function"]["arguments"].str();
        }
    }
};

}  // namespace detail

// ============================================================================
//  ToolBox — named tools with C++ callbacks, for the automatic tool loop
// ============================================================================

class ToolBox {
public:
    using Handler = std::function<std::string(const Json& arguments)>;

    // `parameters` is the JSON Schema of the arguments, e.g. parsed from a
    // string literal with Json::parse(R"({"type":"object",...})").
    ToolBox& add(std::string name, std::string description, Json parameters, Handler handler) {
        Tool tool;
        tool.name = std::move(name);
        tool.description = std::move(description);
        tool.parameters = std::move(parameters);
        handlers_.push_back(std::move(handler));
        tools_.push_back(std::move(tool));
        return *this;
    }

    const std::vector<Tool>& tools() const { return tools_; }

    bool has(const std::string& name) const {
        for (const auto& t : tools_)
            if (t.name == name) return true;
        return false;
    }

    std::string call(const std::string& name, const Json& arguments) const {
        for (std::size_t i = 0; i < tools_.size(); ++i)
            if (tools_[i].name == name) return handlers_[i](arguments);
        throw Error(Error::Kind::Usage, "unknown tool: " + name);
    }

private:
    std::vector<Tool> tools_;
    std::vector<Handler> handlers_;
};

// ============================================================================
//  Client
// ============================================================================

struct Config {
    std::string base_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string model;
    long timeout_ms = 120000;
    int max_retries = 2;
    std::vector<HttpHeader> extra_headers;
    std::shared_ptr<Transport> transport;  // default_transport() when null
};

class Client {
public:
    explicit Client(Config config) : config_(std::move(config)) {
        if (!config_.transport) config_.transport = default_transport();
        while (!config_.base_url.empty() && config_.base_url.back() == '/')
            config_.base_url.pop_back();
    }

    // --- ready-made providers (all OpenAI-compatible) --------------------

    static Client openai(std::string api_key, std::string model = "gpt-4o-mini") {
        Config c;
        c.api_key = std::move(api_key);
        c.model = std::move(model);
        return Client(std::move(c));
    }
    static Client openrouter(std::string api_key, std::string model) {
        Config c;
        c.base_url = "https://openrouter.ai/api/v1";
        c.api_key = std::move(api_key);
        c.model = std::move(model);
        return Client(std::move(c));
    }
    static Client groq(std::string api_key, std::string model = "llama-3.3-70b-versatile") {
        Config c;
        c.base_url = "https://api.groq.com/openai/v1";
        c.api_key = std::move(api_key);
        c.model = std::move(model);
        return Client(std::move(c));
    }
    static Client deepseek(std::string api_key, std::string model = "deepseek-chat") {
        Config c;
        c.base_url = "https://api.deepseek.com/v1";
        c.api_key = std::move(api_key);
        c.model = std::move(model);
        return Client(std::move(c));
    }
    static Client mistral(std::string api_key, std::string model = "mistral-small-latest") {
        Config c;
        c.base_url = "https://api.mistral.ai/v1";
        c.api_key = std::move(api_key);
        c.model = std::move(model);
        return Client(std::move(c));
    }
    static Client ollama(std::string model,
                         std::string base_url = "http://127.0.0.1:11434/v1") {
        Config c;
        c.base_url = std::move(base_url);
        c.model = std::move(model);
        return Client(std::move(c));
    }
    static Client llamacpp(std::string base_url = "http://127.0.0.1:8080/v1") {
        Config c;
        c.base_url = std::move(base_url);
        c.model = "default";
        return Client(std::move(c));
    }
    static Client lmstudio(std::string model = "",
                           std::string base_url = "http://127.0.0.1:1234/v1") {
        Config c;
        c.base_url = std::move(base_url);
        c.model = std::move(model);
        return Client(std::move(c));
    }

    const Config& config() const { return config_; }

    // --- chat -------------------------------------------------------------

    Reply chat(const std::vector<Message>& messages, const Options& options = {}) {
        Json body = detail::build_chat_body(messages, options, pick_model(options), false);
        HttpResponse response = request("POST", "/chat/completions", body.dump(), nullptr);
        return detail::parse_reply(parse_json(response.body));
    }

    Reply chat(const std::string& user_prompt, const Options& options = {}) {
        return chat(std::vector<Message>{Message::user(user_prompt)}, options);
    }

    // Streams text deltas into `on_text` and returns the accumulated reply.
    Reply stream(const std::vector<Message>& messages,
                 const std::function<void(const std::string&)>& on_text,
                 const Options& options = {}) {
        Json body = detail::build_chat_body(messages, options, pick_model(options), true);
        detail::StreamAccumulator acc;
        SseParser sse;
        auto on_event = [&](const std::string& data) {
            if (data == "[DONE]") return;
            acc.feed(parse_json(data));
            if (on_text && !acc.last_text.empty()) on_text(acc.last_text);
        };
        ChunkSink sink = [&](const char* data, std::size_t size) {
            sse.feed(data, size, on_event);
            return true;
        };
        request("POST", "/chat/completions", body.dump(), sink);
        sse.finish(on_event);
        return std::move(acc.reply);
    }

    Reply stream(const std::string& user_prompt,
                 const std::function<void(const std::string&)>& on_text,
                 const Options& options = {}) {
        return stream(std::vector<Message>{Message::user(user_prompt)}, on_text, options);
    }

    // Runs the model with tools, executing requested tool calls and feeding
    // results back until the model produces a final answer. `messages` is
    // extended in place with the full exchange.
    Reply chat_tools(std::vector<Message>& messages, const ToolBox& toolbox,
                     const Options& options = {}, int max_rounds = 8) {
        Options opts = options;
        opts.tools = toolbox.tools();
        for (int round = 0; round < max_rounds; ++round) {
            Reply reply = chat(messages, opts);
            if (!reply.wants_tools()) return reply;

            Message assistant;
            assistant.role = Role::assistant;
            assistant.content = reply.content;
            assistant.tool_calls = reply.tool_calls;
            messages.push_back(std::move(assistant));

            for (const auto& call : reply.tool_calls) {
                std::string result;
                try {
                    Json arguments = call.arguments.empty() ? Json::object()
                                                            : Json::parse(call.arguments);
                    result = toolbox.call(call.name, arguments);
                } catch (const std::exception& e) {
                    result = std::string("error: ") + e.what();
                }
                messages.push_back(Message::tool_result(call.id, result, call.name));
            }
        }
        throw Error(Error::Kind::Usage,
                    "chat_tools: no final answer after " + std::to_string(max_rounds) + " rounds");
    }

    // --- embeddings ---------------------------------------------------------

    std::vector<std::vector<float>> embed(const std::vector<std::string>& inputs,
                                          const std::string& model = "") {
        Json body = Json::object();
        body["model"] = model.empty() ? pick_model({}) : model;
        Json input = Json::array();
        for (const auto& s : inputs) input.push_back(s);
        body["input"] = std::move(input);

        HttpResponse response = request("POST", "/embeddings", body.dump(), nullptr);
        Json j = parse_json(response.body);
        std::vector<std::vector<float>> out(inputs.size());
        for (const auto& item : j["data"].items()) {
            std::size_t index = static_cast<std::size_t>(item["index"].integer());
            if (index >= out.size()) out.resize(index + 1);
            for (const auto& v : item["embedding"].items())
                out[index].push_back(static_cast<float>(v.number()));
        }
        return out;
    }

    // --- escape hatches -----------------------------------------------------

    Json get(const std::string& path) {
        return parse_json(request("GET", path, "", nullptr).body);
    }
    Json post(const std::string& path, const Json& body) {
        return parse_json(request("POST", path, body.dump(), nullptr).body);
    }
    Json models() { return get("/models"); }

private:
    Config config_;

    std::string pick_model(const Options& options) const {
        if (!options.model.empty()) return options.model;
        if (!config_.model.empty()) return config_.model;
        throw Error(Error::Kind::Usage, "no model set (Config::model or Options::model)");
    }

    static Json parse_json(const std::string& text) {
        try {
            return Json::parse(text);
        } catch (const Error&) {
            std::string excerpt = text.substr(0, 500);
            throw Error(Error::Kind::Parse, "server returned invalid JSON: " + excerpt);
        }
    }

    static bool retryable(int status) {
        return status == 408 || status == 429 ||
               (status >= 500 && status != 501 && status != 505);
    }

    HttpResponse request(const std::string& method, const std::string& path,
                         const std::string& body, const ChunkSink& sink) {
        HttpRequest req;
        req.method = method;
        req.url = config_.base_url + path;
        req.timeout_ms = config_.timeout_ms;
        if (!body.empty())
            req.headers.push_back({"Content-Type", "application/json"});
        if (!config_.api_key.empty())
            req.headers.push_back({"Authorization", "Bearer " + config_.api_key});
        req.headers.push_back({"User-Agent", "slovo/" SLOVO_VERSION});
        for (const auto& h : config_.extra_headers) req.headers.push_back(h);
        req.body = body;

        bool delivered = false;
        ChunkSink guarded_sink;
        if (sink)
            guarded_sink = [&](const char* data, std::size_t size) {
                delivered = true;
                return sink(data, size);
            };

        for (int attempt = 0;; ++attempt) {
            bool last_attempt = attempt >= config_.max_retries;
            try {
                HttpResponse response = config_.transport->send(req, guarded_sink);
                if (response.status / 100 == 2) return response;
                if (!retryable(response.status) || last_attempt)
                    throw Error(Error::Kind::Http,
                                "HTTP " + std::to_string(response.status) + " from " + path +
                                    ": " + response.body.substr(0, 500),
                                response.status, response.body);
            } catch (const Error& e) {
                bool can_retry = e.kind == Error::Kind::Network && !delivered && !last_attempt;
                if (!can_retry) throw;
            }
            backoff(attempt);
        }
    }

    static void backoff(int attempt) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> jitter(0, 250);
        long ms = (400L << std::min(attempt, 6)) + jitter(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

}  // namespace slovo

#endif  // SLOVO_HPP_INCLUDED
