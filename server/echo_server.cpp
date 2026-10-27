// WebSocket echo server implemented with libwebsockets.

#include <libwebsockets.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace {

constexpr std::size_t kMaxMessageSize = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kReceiveBufferSize = 64 * 1024;
constexpr std::size_t kWriteChunkSize = 16 * 1024;
constexpr std::size_t kLogChunkBytes = 48;

struct Session {
    unsigned char *payload;
    std::size_t capacity;
    std::size_t length;
    std::size_t tx_offset;
    int binary;
    int receiving;
    int tx_pending;
    int tx_flush_pending;
    int close_code;
    char close_reason[125];
};

struct Options {
    std::string host = "0.0.0.0";
    int port = 10000;
};

struct ServerState {
    lws_context *context = nullptr;
};

ServerState g_server;
volatile std::sig_atomic_t g_stop = 0;

void free_payload(Session *session) {
    std::free(session->payload);
    session->payload = nullptr;
    session->capacity = 0;
    session->length = 0;
    session->tx_offset = 0;
}

bool reserve_payload(Session *session, std::size_t additional) {
    if (additional > kMaxMessageSize - session->length) {
        return false;
    }

    const std::size_t required = session->length + additional;
    if (required <= session->capacity &&
        (required != 0 || session->payload != nullptr)) {
        return true;
    }

    std::size_t capacity = session->capacity == 0 ? kReceiveBufferSize
                                                   : session->capacity;
    while (capacity < required) {
        const std::size_t next = capacity > kMaxMessageSize / 2
                                     ? kMaxMessageSize
                                     : capacity * 2;
        if (next == capacity) {
            break;
        }
        capacity = next;
    }
    if (capacity < required || capacity > kMaxMessageSize ||
        capacity > std::numeric_limits<std::size_t>::max() - LWS_PRE) {
        return false;
    }

    auto *buffer = static_cast<unsigned char *>(
        std::realloc(session->payload, LWS_PRE + capacity));
    if (buffer == nullptr) {
        return false;
    }

    session->payload = buffer;
    session->capacity = capacity;
    return true;
}

std::string header_value(lws *wsi, lws_token_indexes token) {
    char value[1024] = {};
    if (lws_hdr_total_length(wsi, token) <= 0 ||
        lws_hdr_copy(wsi, value, sizeof(value), token) <= 0) {
        return {};
    }
    return value;
}

std::string uri_arguments(lws *wsi) {
    std::string value;
    char fragment[1024] = {};
    for (int index = 0;; ++index) {
        const int length = lws_hdr_copy_fragment(
            wsi, fragment, sizeof(fragment), WSI_TOKEN_HTTP_URI_ARGS, index);
        if (length <= 0) {
            break;
        }
        if (!value.empty()) {
            value += '&';
        }
        value.append(fragment, static_cast<std::size_t>(length));
    }
    return value;
}

void log_binary_payload(const Session *session) {
    char line[64 + kLogChunkBytes * 3] = {};
    std::size_t offset = 0;
    bool first = true;

    if (session->length == 0) {
        lwsl_user("received message: <empty binary payload>\n");
        return;
    }

    while (offset < session->length) {
        const std::size_t count =
            std::min(kLogChunkBytes, session->length - offset);
        std::size_t pos = 0;
        const char *prefix = first ? "received message: "
                                   : "received message (continued): ";
        const std::size_t prefix_len = std::strlen(prefix);
        std::memcpy(line, prefix, prefix_len);
        pos += prefix_len;

        for (std::size_t n = 0; n < count; ++n) {
            if (n != 0) {
                line[pos++] = ' ';
            }
            std::snprintf(line + pos, sizeof(line) - pos, "%02x",
                          static_cast<unsigned int>(
                              session->payload[LWS_PRE + offset + n]));
            pos += 2;
        }
        line[pos] = '\0';
        lwsl_user("%s\n", line);
        offset += count;
        first = false;
    }
}

void log_text_payload(const Session *session) {
    if (session->length == 0) {
        lwsl_user("received message: ''\n");
        return;
    }

    // Keep the same escaped, quoted style as Python's repr for common text.
    char line[3 + kLogChunkBytes * 4] = {};
    const char *prefix = "received message: ";
    const char *continued_prefix = "received message (continued): ";
    std::size_t pos = std::strlen(prefix);
    std::memcpy(line, prefix, pos);
    line[pos++] = '\'';
    for (std::size_t n = 0; n < session->length; ++n) {
        const unsigned char ch = session->payload[LWS_PRE + n];
        const bool printable = ch >= 0x20 && ch != 0x7f && ch != '\\' &&
                               ch != '\'';
        char escaped[5] = {};
        std::size_t escaped_len = 0;
        if (printable) {
            escaped[0] = static_cast<char>(ch);
            escaped_len = 1;
        } else {
            const char *escape = nullptr;
            switch (ch) {
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            case '\\': escape = "\\\\"; break;
            case '\'': escape = "\\'"; break;
            default: break;
            }
            if (escape != nullptr) {
                escaped[0] = escape[0];
                escaped[1] = escape[1];
                escaped_len = 2;
            } else {
                std::snprintf(escaped, sizeof(escaped), "\\x%02x",
                              static_cast<unsigned int>(ch));
                escaped_len = 4;
            }
        }

        if (pos + escaped_len + 2 >= sizeof(line)) {
            line[pos++] = '\'';
            line[pos] = '\0';
            lwsl_user("%s\n", line);
            pos = std::strlen(continued_prefix);
            std::memcpy(line, continued_prefix, pos);
            line[pos++] = '\'';
        }
        std::memcpy(line + pos, escaped, escaped_len);
        pos += escaped_len;
    }
    line[pos++] = '\'';
    line[pos] = '\0';
    lwsl_user("%s\n", line);
}

void log_received_payload(const Session *session) {
    if (session->binary) {
        log_binary_payload(session);
    } else {
        log_text_payload(session);
    }
}

void log_connection_open(lws *wsi) {
    char peer[128] = {};
    lws_get_peer_simple(wsi, peer, sizeof(peer));

    auto *vhost = lws_get_vhost(wsi);
    const char *vhost_name = lws_get_vhost_name(vhost);
    const int vhost_port = lws_get_vhost_port(vhost);
    std::string path = header_value(wsi, WSI_TOKEN_GET_URI);
    const std::string uri_args = uri_arguments(wsi);
    if (!uri_args.empty()) {
        path += "?" + uri_args;
    }
    const std::string host = header_value(wsi, WSI_TOKEN_HOST);
    const std::string origin = header_value(wsi, WSI_TOKEN_ORIGIN);
    const std::string requested_protocol =
        header_value(wsi, WSI_TOKEN_PROTOCOL);
    const std::string extensions = header_value(wsi, WSI_TOKEN_EXTENSIONS);
#if defined(LWS_WITH_HTTP_UNCOMMON_HEADERS) || defined(LWS_HTTP_HEADERS_ALL)
    const std::string user_agent =
        header_value(wsi, WSI_TOKEN_HTTP_USER_AGENT);
#else
    const std::string user_agent;
#endif
    const auto *protocol = lws_get_protocol(wsi);

    lwsl_user(
        "client connected: remote=%s local=%s:%d path=%s host=%s "
        "origin=%s user_agent=%s requested_subprotocol=%s "
        "handler_protocol=%s requested_extensions=%s\n",
        peer[0] != '\0' ? peer : "<unknown>",
        vhost_name != nullptr ? vhost_name : "<unknown>", vhost_port,
        path.empty() ? "<unknown>" : path.c_str(),
        host.empty() ? "<none>" : host.c_str(),
        origin.empty() ? "<none>" : origin.c_str(),
        user_agent.empty() ? "<none>" : user_agent.c_str(),
        requested_protocol.empty() ? "<none>" : requested_protocol.c_str(),
        protocol != nullptr && protocol->name != nullptr ? protocol->name
                                                         : "<unknown>",
        extensions.empty() ? "<none>" : extensions.c_str());
}

void log_connection_closed(lws *wsi, const Session *session) {
    char peer[128] = {};
    lws_get_peer_simple(wsi, peer, sizeof(peer));
    lwsl_user("client disconnected: remote=%s code=%d reason=%s\n",
              peer[0] != '\0' ? peer : "<unknown>", session->close_code,
              session->close_reason[0] != '\0' ? session->close_reason : "");
}

bool parse_integer(const char *value, int *result) {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0 ||
        parsed > 65535) {
        return false;
    }
    *result = static_cast<int>(parsed);
    return true;
}

bool parse_options(int argc, char **argv, Options *options) {
    for (int n = 1; n < argc; ++n) {
        const std::string argument = argv[n];
        if (argument == "--help" || argument == "-h") {
            std::printf("Usage: %s [--host ADDRESS] [--port PORT]\n", argv[0]);
            return false;
        }

        const auto parse_value = [&](const char *name, std::string *value) {
            if (argument == name && n + 1 < argc) {
                *value = argv[++n];
                return true;
            }
            return false;
        };
        if (parse_value("--host", &options->host)) {
            continue;
        }
        if (argument == "--port" && n + 1 < argc) {
            if (!parse_integer(argv[++n], &options->port)) {
                std::fprintf(stderr, "invalid port\n");
                return false;
            }
            continue;
        }

        std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
        return false;
    }
    return true;
}

void signal_handler(int) {
    g_stop = 1;
    if (g_server.context != nullptr) {
        lws_cancel_service(g_server.context);
    }
}

int callback_echo(lws *wsi, lws_callback_reasons reason, void *user,
                 void *in, std::size_t len) {
    auto *session = static_cast<Session *>(user);

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        std::memset(session, 0, sizeof(*session));
        session->close_code = LWS_CLOSE_STATUS_ABNORMAL_CLOSE;
        log_connection_open(wsi);
        break;

    case LWS_CALLBACK_RECEIVE: {
        if (lws_is_first_fragment(wsi)) {
            if (session->receiving || session->tx_pending ||
                session->tx_flush_pending) {
                lwsl_err("new message arrived while the previous echo is pending\n");
                return -1;
            }
            session->length = 0;
            session->tx_offset = 0;
            session->binary = lws_frame_is_binary(wsi);
            session->receiving = 1;
        }

        if (!session->receiving) {
            lwsl_err("invalid WebSocket continuation frame\n");
            session->close_code = LWS_CLOSE_STATUS_PROTOCOL_ERR;
            lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR, nullptr, 0);
            return -1;
        }
        if (len > kMaxMessageSize - session->length) {
            lwsl_err("incoming WebSocket message exceeds the 1 GiB limit\n");
            session->close_code = LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE;
            lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, nullptr,
                             0);
            return -1;
        }
        if (!reserve_payload(session, len)) {
            lwsl_err("memory exhausted while receiving WebSocket message\n");
            session->close_code = LWS_CLOSE_STATUS_UNEXPECTED_CONDITION;
            lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION,
                             nullptr, 0);
            return -1;
        }

        if (len != 0) {
            std::memcpy(session->payload + LWS_PRE + session->length, in, len);
            session->length += len;
        }

        if (lws_is_final_fragment(wsi)) {
            session->receiving = 0;
            session->tx_pending = 1;
            session->tx_offset = 0;
            log_received_payload(session);
            lws_rx_flow_control(wsi, 0);
            lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (session->tx_flush_pending) {
            session->tx_flush_pending = 0;
            session->tx_pending = 0;
            session->length = 0;
            session->tx_offset = 0;
            lws_rx_flow_control(wsi, 1);
            break;
        }
        if (!session->tx_pending) {
            break;
        }

        const std::size_t remaining = session->length - session->tx_offset;
        const std::size_t chunk = std::min(kWriteChunkSize, remaining);
        const bool first = session->tx_offset == 0;
        const bool final = chunk == remaining;
        const auto flags = static_cast<lws_write_protocol>(lws_write_ws_flags(
            session->binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT, first, final));
        const int written = lws_write(
            wsi, session->payload + LWS_PRE + session->tx_offset, chunk, flags);
        if (written < 0 || static_cast<std::size_t>(written) < chunk) {
            lwsl_err("failed to echo WebSocket message: wrote %d of %zu bytes\n",
                     written, chunk);
            return -1;
        }

        session->tx_offset += chunk;
        if (final) {
            // Keep the buffer until the next writable callback.  This is
            // required when permessage-deflate still has deferred output.
            session->tx_flush_pending = 1;
            lws_callback_on_writable(wsi);
        } else {
            lws_callback_on_writable(wsi);
        }
        break;
    }

    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        if (len >= 2 && in != nullptr) {
            const auto *close = static_cast<const unsigned char *>(in);
            session->close_code = (static_cast<int>(close[0]) << 8) | close[1];
            const std::size_t reason_len =
                std::min(len - 2, sizeof(session->close_reason) - 1);
            if (reason_len != 0) {
                std::memcpy(session->close_reason, close + 2, reason_len);
                session->close_reason[reason_len] = '\0';
            }
        }
        break;

    case LWS_CALLBACK_CLOSED:
        log_connection_closed(wsi, session);
        free_payload(session);
        break;

    default:
        break;
    }

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return argc > 1 && (std::string(argv[1]) == "--help" ||
                            std::string(argv[1]) == "-h")
                   ? 0
                   : 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_USER, nullptr);

    lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    info.port = options.port;
    info.iface = options.host.c_str();
    info.fd_limit_per_thread = 0;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

    static lws_protocols protocols[] = {
        {"echo", callback_echo, sizeof(Session), kReceiveBufferSize, 0,
         nullptr, kWriteChunkSize},
        LWS_PROTOCOL_LIST_TERM};
    static const lws_extension extensions[] = {
        {"permessage-deflate", lws_extension_callback_pm_deflate,
         "permessage-deflate; client_no_context_takeover"
         "; client_max_window_bits"},
        {nullptr, nullptr, nullptr}};
    info.protocols = protocols;
    info.extensions = extensions;

    g_server.context = lws_create_context(&info);
    if (g_server.context == nullptr) {
        lwsl_err("failed to create libwebsockets context\n");
        return 1;
    }

    lwsl_user("WebSocket echo server listening on ws://%s:%d (max inbound message: 1 GiB)\n",
              options.host.c_str(), options.port);
    int result = 0;
    while (!g_stop && result >= 0) {
        result = lws_service(g_server.context, 0);
    }

    lws_context_destroy(g_server.context);
    g_server.context = nullptr;
    return 0;
}
