#pragma once
#include "common.h"
#include "server.h"

namespace rpc {

namespace detail {
template<typename socket_t> struct declare_ssl_context {};

template<>
struct declare_ssl_context<types::ssl_socket_t> {
    declare_ssl_context(asio::ssl::context &ctx) : ssl_context(std::move(ctx)) {}
    asio::ssl::context ssl_context;
};
};

/**
 * @brief An abstraction over a basic acceptor, provides utilities for SSL and
 * non-SSL connections, peer verification and thread management.
 * @tparam socket_t type of socket to use (either types::socket_t or
 * types::ssl_socket_t)
 * @tparam EntrypointService_ A class derived from rpc_server, which will be
 * used to handle incoming connections.
 * @note The service provider itself does not keep track of the
 * EntrypointService_ objects it creates, they are considered self-managed, or
 * rather, owned by the remote client.
 */
template <typename socket_t, template <typename socket_t_> typename EntrypointService_>
requires((std::is_same_v<socket_t, rpc::types::socket_t> || std::is_same_v<socket_t, rpc::types::ssl_socket_t>) &&
          std::is_base_of_v<rpc::server<socket_t>, EntrypointService_<socket_t>>)
struct service_provider : detail::declare_ssl_context<socket_t> {
    using EntrypointService = EntrypointService_<socket_t>;
    static constexpr bool is_ssl = std::is_same_v<socket_t, rpc::types::ssl_socket_t>;

    /**
     * @brief Non-SSL constructor
     * @param ep The endpoint to bind to
     * @param thread_count std::thread::hardware_concurrency() by default
     */
    explicit service_provider(const asio::ip::tcp::endpoint &ep, int thread_count = -1) requires(!is_ssl)
            : acceptor(ctx, ep), thread_count(thread_count) {}

    /**
     * @brief SSL constructor
     * @param ep The endpoint to bind to
     * @param ssl_ctx SSL context
     * @param thread_count std::thread::hardware_concurrency() by default
     */
    service_provider(const asio::ip::tcp::endpoint &ep, asio::ssl::context &ssl_ctx,
                     int thread_count = -1)requires(is_ssl): acceptor(ctx, ep),
                                                             detail::declare_ssl_context<socket_t>(ssl_ctx),
                                                             thread_count(thread_count) {}

    ~service_provider() {
        ctx.stop();
    }

    asio::io_context &get_context() { return ctx; }
    asio::ssl::context &get_ssl_context() requires(is_ssl) { return this->ssl_context; }

    void start() {
        if (thread_count == -1) {
            thread_count = std::thread::hardware_concurrency();
        }
        RPC_MSG(RPC_INFO, "Starting server on port %u with %u thread(s)",
                acceptor.local_endpoint().port(), thread_count);

        accept();

        io_threads.reserve(thread_count);
        for (int i = 0; i < thread_count; i++) {
            io_threads.template emplace_back([this] { ctx.run(); });
        }
    }

    void set_service_created_callback(const std::function<void(const std::shared_ptr<EntrypointService> &)> &callback) {
        service_created_callback = callback;
    }

private:
    void accept() {
        acceptor.async_accept([this](asio::error_code ec, asio::ip::tcp::socket peer) {
            ASIO_ERROR_GUARD(ec);

            // We can enqueue another async_accept, before processing the new connection.
            accept();

            std::shared_ptr<EntrypointService> svc;
            if constexpr (!is_ssl) {
                svc = EntrypointService::template create<EntrypointService>(std::move(peer));
            } else {
                // SSL handshake
                rpc::types::ssl_socket_t sock(std::move(peer), this->ssl_context);
                sock.handshake(asio::ssl::stream_base::handshake_type::server, ec);
                ASIO_ERROR_GUARD(ec);

                svc = EntrypointService::template create<EntrypointService>(std::move(sock));
            }
            if (service_created_callback) {
                service_created_callback(svc);
            }
        });
    }

    int thread_count;
    asio::io_context ctx;
    std::vector<std::jthread> io_threads;

    asio::ip::tcp::acceptor acceptor;

    std::function<void(const std::shared_ptr<EntrypointService> &)> service_created_callback;
};

}; // namespace rpc