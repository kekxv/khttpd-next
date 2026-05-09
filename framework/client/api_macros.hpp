#ifndef KHTTPD_FRAMEWORK_CLIENT_API_MACROS_HPP
#define KHTTPD_FRAMEWORK_CLIENT_API_MACROS_HPP

// Compiler warning suppression (must come before any includes that might trigger warnings)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#pragma clang diagnostic ignored "-Wvariadic-macro-arguments-omitted"
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

// API Client definition macro (with single host)
#define KHTTPD_API_CLIENT(Name, Host) \
    class Name : public khttpd::framework::client::HttpClient { \
    public: \
        Name() { set_base_url(Host); } \
        explicit Name(boost::asio::io_context& ioc) : HttpClient(ioc) { set_base_url(Host); }

// API Client definition macro (with multi-host pool)
#define KHTTPD_API_CLIENT_POOL(Name, ...) \
    class Name : public khttpd::framework::client::HttpClient { \
    public: \
        Name() { \
            static const std::vector<khttpd::framework::client::HostEntry> pool_hosts = { __VA_ARGS__ }; \
            set_base_url_pool(pool_hosts); \
        } \
        explicit Name(boost::asio::io_context& ioc) : HttpClient(ioc) { \
            static const std::vector<khttpd::framework::client::HostEntry> pool_hosts = { __VA_ARGS__ }; \
            set_base_url_pool(pool_hosts); \
        }

#define KHTTPD_API_CLIENT_END() };

// Host entry shorthand — includes trailing comma for use in initializer lists
#define KHTTPD_HOST(Url, W) {Url, W},

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif // KHTTPD_FRAMEWORK_CLIENT_API_MACROS_HPP
