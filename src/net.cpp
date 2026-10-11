#include "net.hpp"
#include "shutdown.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>

#ifdef HAVE_BOOST_BEAST
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

static void load_windows_root_certs(boost::asio::ssl::context& ctx) {
    HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
    if (!hStore) return;

    X509_STORE* store = SSL_CTX_get_cert_store(ctx.native_handle());
    PCCERT_CONTEXT pContext = nullptr;
    while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != nullptr) {
        X509* x509 = d2i_X509(nullptr, const_cast<const unsigned char**>(&pContext->pbCertEncoded), pContext->cbCertEncoded);
        if (x509) {
            X509_STORE_add_cert(store, x509);
            X509_free(x509);
        }
    }
    CertCloseStore(hStore, 0);
}
#endif // _WIN32

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

#endif // HAVE_BOOST_BEAST

namespace lob {

constexpr size_t MAX_MESSAGE_BYTES = 65536;

std::string NetClient::build_combined_stream_path(
    binance::Venue venue, const std::vector<std::string>& symbols) {
    std::string path = (venue == binance::Venue::Usdm) ? "/public/stream?streams=" : "/stream?streams=";
    for (size_t i = 0; i < symbols.size(); ++i) {
        std::string s_lower = symbols[i];
        for (char& c : s_lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (i > 0) path.push_back('/');
        path.append(s_lower);
        path.append("@depth@100ms/");
        path.append(s_lower);
        path.append("@depth5@100ms/");
        path.append(s_lower);
        path.append("@trade");
    }
    return path;
}

NetClient::NetClient(NetClientConfig config,
                     SpscRingBuffer<RingSlot, 2048>& ring,
                     Processor& processor)
    : config_(std::move(config))
    , ring_(ring)
    , processor_(processor) {
}

NetClient::~NetClient() = default;

void NetClient::run() {
#ifdef HAVE_BOOST_BEAST
    std::cout << "[NET] Initializing Boost.Beast TLS WebSocket client...\n";
    std::string target_path = build_combined_stream_path(config_.venue, config_.symbols);
    std::cout << "[NET] Host: " << config_.host << ":" << config_.port << "\n"
              << "[NET] Target: " << target_path << "\n";

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> jitter_dist(0, 300);
    int backoff_attempt = 0;

    while (!is_stop_requested()) {
        try {
            net::io_context ioc;
            ssl::context ssl_ctx{ssl::context::tlsv12_client};

            // TLS Verification: Windows Root CA Store or Linux default verify paths
#ifdef _WIN32
            load_windows_root_certs(ssl_ctx);
#else
            ssl_ctx.set_default_verify_paths();
#endif
            ssl_ctx.set_verify_mode(ssl::verify_peer);

            tcp::resolver resolver{ioc};
            websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ssl_ctx};

            // 1. Resolve host
            auto const results = resolver.resolve(config_.host, config_.port);

            // 2. TCP connect & TCP_NODELAY
            net::connect(beast::get_lowest_layer(ws), results);
            beast::get_lowest_layer(ws).set_option(tcp::no_delay(true));

            // 3. Set SNI (Server Name Indication)
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), config_.host.c_str())) {
                throw beast::system_error(
                    beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()),
                    "Failed to set SNI HostName");
            }

            // 4. SSL Handshake
            ws.next_layer().handshake(ssl::stream_base::client);

            // 5. Configure WebSocket options & automatic ping/pong handling
            ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
            ws.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(http::field::user_agent, "binance-lob-capture/1.0");
                }));

            // 6. WebSocket Handshake
            std::string host_header = config_.host + ":" + config_.port;
            ws.handshake(host_header, target_path);

            std::cout << "[NET] Connected successfully to Binance WebSocket stream!\n";
            backoff_attempt = 0; // Reset backoff on successful connect

            // Reused flat buffer for WebSocket frame reads
            beast::flat_buffer read_buf;
            read_buf.max_size(MAX_MESSAGE_BYTES + 1024);

            while (!is_stop_requested()) {
                read_buf.consume(read_buf.size());
                beast::error_code ec;
                ws.read(read_buf, ec);

                // Wall-clock nanoseconds captured immediately after read completes
                const auto read_wall_time = std::chrono::system_clock::now();
                int64_t recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    read_wall_time.time_since_epoch()).count();

                if (ec) {
                    if (ec == websocket::error::closed || is_stop_requested()) {
                        break;
                    }
                    std::cerr << "[NET] Read error: " << ec.message() << "\n";
                    break;
                }

                size_t len = read_buf.size();
                if (len > MAX_MESSAGE_BYTES) {
                    metrics_.oversized_drops.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                RingSlot* slot = ring_.prepare_write();
                if (!slot) {
                    metrics_.ring_overflow_drops.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                slot->recv_ns = recv_ns;
                slot->shard = config_.shard_id;
                slot->epoch = conn_epoch_;
                slot->seq = ++conn_seq_;
                slot->len = static_cast<uint32_t>(len);
                net::buffer_copy(net::buffer(slot->data, len), read_buf.data());
                ring_.commit_write();

                metrics_.messages_received.fetch_add(1, std::memory_order_relaxed);
                metrics_.bytes_received.fetch_add(len, std::memory_order_relaxed);
            }

            // Graceful WebSocket close if shutdown requested
            if (ws.is_open()) {
                beast::error_code ec_close;
                ws.close(websocket::close_code::normal, ec_close);
            }

        } catch (const std::exception& e) {
            if (!is_stop_requested()) {
                std::cerr << "[NET] Connection exception: " << e.what() << "\n";
            }
        }

        if (is_stop_requested()) {
            break;
        }

        // Reconnect with exponential backoff + jitter
        conn_epoch_++;
        conn_seq_ = 0;
        metrics_.reconnects.fetch_add(1, std::memory_order_relaxed);
        processor_.on_reconnect(conn_epoch_);

        int base_ms = 500;
        int exp_ms = base_ms * (1 << std::min(backoff_attempt++, 5));
        int capped_ms = std::min(exp_ms, 10000);
        int sleep_ms = capped_ms + jitter_dist(rng);

        std::cout << "[NET] Reconnecting in " << sleep_ms << " ms (epoch " << conn_epoch_ << ")...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }

    std::cout << "[NET] Network thread stopped cleanly.\n";

#else
    std::cout << "\n[NET] Note: Boost.Beast and OpenSSL were not enabled during build.\n"
              << "[NET] To enable live capture, install dependencies and rebuild:\n"
              << "      vcpkg install boost-beast boost-asio boost-system openssl\n"
              << "      cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=\"<vcpkg-root>/scripts/buildsystems/vcpkg.cmake\"\n"
              << "[NET] Running in offline replay mode. Use: binance_capture --replay <market_data.csv>\n\n";

    while (!is_stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

} // namespace lob
