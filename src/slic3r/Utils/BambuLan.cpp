///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuLan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "Http.hpp"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace Slic3r {
namespace {

constexpr int MQTT_PORT = 8883;
constexpr int FTPS_PORT = 990;
constexpr const char *MQTT_USER = "bblp";

std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string safe_upload_name(const std::string &stem)
{
    std::string name;
    for (char c : stem) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            name.push_back(c);
    }
    if (name.empty())
        name = "superslicer-print";
    if (name.size() > 40)
        name.resize(40);
    return name + ".gcode.3mf";
}

std::string normalize_bambu_host(std::string host)
{
    const auto scheme = host.find("://");
    if (scheme != std::string::npos)
        host.erase(0, scheme + 3);
    while (!host.empty() && (host.back() == '/' || std::isspace(static_cast<unsigned char>(host.back()))))
        host.pop_back();
    return host;
}

#if !defined(_WIN32)

struct TlsSocket
{
    int fd = -1;
    SSL *ssl = nullptr;

    ~TlsSocket()
    {
        if (ssl) {
            // Don't wait for the printer's close_notify. A blocking shutdown
            // sits until the socket timeout and stalls the next connection.
            SSL_set_quiet_shutdown(ssl, 1);
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (fd >= 0)
            ::close(fd);
    }

    TlsSocket() = default;
    TlsSocket(const TlsSocket &) = delete;
    TlsSocket &operator=(const TlsSocket &) = delete;
};

bool set_timeouts(int fd, int seconds)
{
    timeval tv{};
    tv.tv_sec = seconds;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0
        && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

// Developer Mode printers present a self-signed X.509 v1 certificate.
// Ordinary CA checks reject that certificate, so the LAN connection
// accepts it. The access code is still required.
int tcp_connect(const std::string &host, int port, std::string &error)
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        error = "Could not resolve " + host;
        return -1;
    }
    int fd = -1;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        set_timeouts(fd, 20);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        error = "Could not connect to " + host + ":" + port_str;
    return fd;
}

bool tls_on_socket(TlsSocket &out, SSL_CTX *ctx, SSL_SESSION *reuse, int fd, std::string &error)
{
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        ::close(fd);
        error = "TLS setup failed";
        return false;
    }
    SSL_set_fd(ssl, fd);
    if (reuse && SSL_set_session(ssl, reuse) != 1) {
        error = "Could not reuse the FTP TLS session";
        SSL_free(ssl);
        ::close(fd);
        return false;
    }
    if (SSL_connect(ssl) != 1) {
        const unsigned long err = ERR_get_error();
        error = std::string("TLS handshake failed") + (err ? std::string(": ") + ERR_error_string(err, nullptr) : "");
        SSL_free(ssl);
        ::close(fd);
        return false;
    }
    if (reuse && !SSL_session_reused(ssl)) {
        error = "The printer did not resume the FTP TLS session";
        SSL_free(ssl);
        ::close(fd);
        return false;
    }
    out.fd = fd;
    out.ssl = ssl;
    return true;
}

bool tls_connect(TlsSocket &out, SSL_CTX *ctx, SSL_SESSION *reuse, const std::string &host, int port, std::string &error)
{
    const int fd = tcp_connect(host, port, error);
    if (fd < 0)
        return false;
    return tls_on_socket(out, ctx, reuse, fd, error);
}

bool ssl_write_all(SSL *ssl, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int n = SSL_write(ssl, data + off, static_cast<int>(len - off));
        if (n <= 0)
            return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ssl_read_some(SSL *ssl, std::string &buf)
{
    char tmp[2048];
    const int n = SSL_read(ssl, tmp, sizeof(tmp));
    if (n <= 0)
        return false;
    buf.append(tmp, tmp + n);
    return true;
}

enum class SslIo { Data, Timeout, Closed };

// A short receive timeout is a pause, not a dead printer. The long-lived
// controls session uses it so a jog can be written between reports.
SslIo ssl_read_status(SSL *ssl, std::string &buf, std::string &error)
{
    char tmp[4096];
    ERR_clear_error();
    const int n = SSL_read(ssl, tmp, sizeof(tmp));
    if (n > 0) {
        buf.append(tmp, tmp + n);
        return SslIo::Data;
    }
    const int code = SSL_get_error(ssl, n);
    if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE
        || (code == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT || errno == EINTR))) {
        ERR_clear_error();
        return SslIo::Timeout;
    }
    if (code == SSL_ERROR_ZERO_RETURN)
        error = "The printer closed the MQTT connection";
    else {
        const unsigned long err = ERR_get_error();
        error = "The printer closed the MQTT connection";
        if (err)
            error += std::string(": ") + ERR_error_string(err, nullptr);
    }
    return SslIo::Closed;
}

std::string mqtt_publish_payload(unsigned char type, const std::string &body)
{
    if (body.size() < 2)
        return {};
    const size_t topic_len = (static_cast<unsigned char>(body[0]) << 8) | static_cast<unsigned char>(body[1]);
    size_t offset = 2 + topic_len;
    const int qos = (type >> 1) & 0x03;
    if (qos > 0)
        offset += 2;
    if (body.size() < offset)
        return {};
    return body.substr(offset);
}

void mqtt_remaining_length(std::vector<unsigned char> &out, size_t len)
{
    do {
        unsigned char byte = static_cast<unsigned char>(len % 128);
        len /= 128;
        if (len)
            byte |= 0x80;
        out.push_back(byte);
    } while (len);
}

void mqtt_string(std::vector<unsigned char> &out, const std::string &s)
{
    out.push_back(static_cast<unsigned char>((s.size() >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>(s.size() & 0xff));
    out.insert(out.end(), s.begin(), s.end());
}

bool mqtt_write_packet(SSL *ssl, unsigned char type, const std::vector<unsigned char> &body)
{
    std::vector<unsigned char> packet;
    packet.push_back(type);
    mqtt_remaining_length(packet, body.size());
    packet.insert(packet.end(), body.begin(), body.end());
    return ssl_write_all(ssl, reinterpret_cast<const char *>(packet.data()), packet.size());
}

bool mqtt_read_packet(SSL *ssl, std::string &pending, unsigned char &type, std::string &body, std::string &error)
{
    std::string raw = std::move(pending);
    pending.clear();
    while (raw.size() < 2) {
        if (!ssl_read_some(ssl, raw)) {
            error = "The printer closed the MQTT connection";
            return false;
        }
    }
    type = static_cast<unsigned char>(raw[0]);
    size_t len = 0;
    size_t mult = 1;
    size_t i = 1;
    for (;;) {
        if (i >= raw.size()) {
            if (!ssl_read_some(ssl, raw)) {
                error = "Short MQTT packet";
                return false;
            }
        }
        const unsigned char byte = static_cast<unsigned char>(raw[i++]);
        len += static_cast<size_t>(byte & 0x7f) * mult;
        if ((byte & 0x80) == 0)
            break;
        mult *= 128;
        if (mult > 128 * 128 * 128) {
            error = "MQTT packet is too long";
            return false;
        }
    }
    while (raw.size() < i + len) {
        if (!ssl_read_some(ssl, raw)) {
            error = "Short MQTT packet";
            return false;
        }
    }
    body.assign(raw, i, len);
    if (raw.size() > i + len)
        pending.assign(raw, i + len, std::string::npos);
    return true;
}

bool ftp_write_cmd(SSL *ssl, const std::string &cmd)
{
    const std::string line = cmd + "\r\n";
    return ssl_write_all(ssl, line.data(), line.size());
}

// Read one complete FTP reply, including a multi-line 123- ... 123 reply.
bool ftp_read_reply(SSL *ssl, std::string &buf, int &code, std::string &text, std::string &error)
{
    code = 0;
    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl == std::string::npos) {
            if (!ssl_read_some(ssl, buf)) {
                error = "The printer closed the FTP connection";
                return false;
            }
            continue;
        }
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        text += line;
        text.push_back('\n');
        if (line.size() >= 4 && std::isdigit(static_cast<unsigned char>(line[0]))) {
            const int line_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
            if (code == 0)
                code = line_code;
            if (line[3] == ' ')
                return true;
        }
    }
}

bool ftp_expect(SSL *ssl, std::string &buf, int expected, std::string &error)
{
    int code = 0;
    std::string text;
    if (!ftp_read_reply(ssl, buf, code, text, error))
        return false;
    if (code != expected) {
        error = text;
        return false;
    }
    return true;
}

bool ftp_pasv(SSL *ssl, std::string &buf, std::string &host_out, int &port_out, std::string &error)
{
    if (!ftp_write_cmd(ssl, "PASV"))
        return false;
    int code = 0;
    std::string text;
    if (!ftp_read_reply(ssl, buf, code, text, error))
        return false;
    if (code != 227) {
        error = text;
        return false;
    }
    const size_t open = text.find('(');
    const size_t close = text.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) {
        error = "PASV reply was not understood";
        return false;
    }
    int h1, h2, h3, h4, p1, p2;
    if (std::sscanf(text.c_str() + open + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        error = "PASV reply was not understood";
        return false;
    }
    port_out = p1 * 256 + p2;
    if (h1 == 0 && h2 == 0 && h3 == 0 && h4 == 0)
        host_out.clear();
    else
        host_out = std::to_string(h1) + "." + std::to_string(h2) + "." + std::to_string(h3) + "." + std::to_string(h4);
    return true;
}

class BambuSession
{
public:
    explicit BambuSession(std::string host, std::string access_code, std::string serial)
        : m_host(std::move(host)), m_access_code(std::move(access_code)), m_serial(std::move(serial))
    {
        OPENSSL_init_ssl(0, nullptr);
        m_ctx = SSL_CTX_new(TLS_client_method());
        if (m_ctx) {
            SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);
            // TLS 1.2 session ids are what the printer's FTPS server resumes.
            // TLS 1.3 tickets are not available in time for the data connection.
            SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_session_cache_mode(m_ctx, SSL_SESS_CACHE_CLIENT);
        }
    }

    ~BambuSession()
    {
        if (m_ctx)
            SSL_CTX_free(m_ctx);
    }

    bool ok() const { return m_ctx != nullptr; }

    bool mqtt_connect(std::string &error)
    {
        if (!tls_connect(m_mqtt, m_ctx, nullptr, m_host, MQTT_PORT, error))
            return false;
        std::vector<unsigned char> body;
        mqtt_string(body, "MQTT");
        body.push_back(4); // MQTT 3.1.1
        body.push_back(0xC2); // clean session, username, password
        body.push_back(0);
        body.push_back(30); // keepalive
        mqtt_string(body, "superslicer");
        mqtt_string(body, MQTT_USER);
        mqtt_string(body, m_access_code);
        if (!mqtt_write_packet(m_mqtt.ssl, 0x10, body)) {
            error = "MQTT CONNECT was not sent";
            return false;
        }
        unsigned char type = 0;
        std::string reply;
        if (!mqtt_read_packet(m_mqtt.ssl, m_mqtt_pending, type, reply, error))
            return false;
        if (type != 0x20 || reply.size() < 2 || reply[1] != 0) {
            error = "The printer refused the MQTT login. Check the access code, and that Developer Mode is on.";
            return false;
        }
        std::vector<unsigned char> sub;
        sub.push_back(0);
        sub.push_back(1);
        mqtt_string(sub, "device/" + m_serial + "/report");
        sub.push_back(0);
        if (!mqtt_write_packet(m_mqtt.ssl, 0x82, sub)) {
            error = "MQTT SUBSCRIBE was not sent";
            return false;
        }
        if (!mqtt_read_packet(m_mqtt.ssl, m_mqtt_pending, type, reply, error))
            return false;
        m_connected = true;
        return true;
    }

    bool mqtt_publish(const std::string &payload, std::string &error)
    {
        std::vector<unsigned char> body;
        mqtt_string(body, "device/" + m_serial + "/request");
        body.insert(body.end(), payload.begin(), payload.end());
        if (!mqtt_write_packet(m_mqtt.ssl, 0x30, body)) {
            error = "MQTT publish failed";
            return false;
        }
        return true;
    }

    // Wait until a report mentions the printer state. pushall is a read and
    // works once the socket is up; a missing report means the serial is wrong
    // or Developer Mode is off.
    bool mqtt_wait_report(std::string &error)
    {
        const std::string pushall = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
        if (!mqtt_publish(pushall, error))
            return false;
        for (int i = 0; i < 8; ++i) {
            unsigned char type = 0;
            std::string body;
            if (!mqtt_read_packet(m_mqtt.ssl, m_mqtt_pending, type, body, error))
                return false;
            if ((type & 0xF0) == 0x30 && body.find("print") != std::string::npos)
                return true;
        }
        error = "The printer accepted the login but sent no status. Check the serial number.";
        return false;
    }

    bool ftps_upload(const std::string &local_path, const std::string &remote_name, std::string &error, const std::function<bool(size_t, size_t)> &on_bytes)
    {
        TlsSocket ctrl;
        std::string ftp_buf;
        if (!tls_connect(ctrl, m_ctx, nullptr, m_host, FTPS_PORT, error))
            return false;
        if (!ftp_expect(ctrl.ssl, ftp_buf, 220, error))
            return false;
        if (!ftp_write_cmd(ctrl.ssl, std::string("USER ") + MQTT_USER) || !ftp_expect(ctrl.ssl, ftp_buf, 331, error))
            return false;
        if (!ftp_write_cmd(ctrl.ssl, std::string("PASS ") + m_access_code) || !ftp_expect(ctrl.ssl, ftp_buf, 230, error))
            return false;
        if (!ftp_write_cmd(ctrl.ssl, "PBSZ 0") || !ftp_expect(ctrl.ssl, ftp_buf, 200, error))
            return false;
        if (!ftp_write_cmd(ctrl.ssl, "PROT P") || !ftp_expect(ctrl.ssl, ftp_buf, 200, error))
            return false;
        if (!ftp_write_cmd(ctrl.ssl, "TYPE I") || !ftp_expect(ctrl.ssl, ftp_buf, 200, error))
            return false;

        std::string data_host;
        int data_port = 0;
        if (!ftp_pasv(ctrl.ssl, ftp_buf, data_host, data_port, error))
            return false;
        if (data_host.empty())
            data_host = m_host;

        // The printer accepts the data socket only after STOR, and it starts
        // the data TLS handshake inside that accept. Handshake first and the
        // two sides wait on each other.
        const int data_fd = tcp_connect(data_host, data_port, error);
        if (data_fd < 0)
            return false;
        if (!ftp_write_cmd(ctrl.ssl, "STOR " + remote_name)) {
            ::close(data_fd);
            return false;
        }
        SSL_SESSION *sess = SSL_get1_session(ctrl.ssl);
        TlsSocket data;
        const bool data_ok = tls_on_socket(data, m_ctx, sess, data_fd, error);
        if (sess)
            SSL_SESSION_free(sess);
        if (!data_ok)
            return false;
        int code = 0;
        std::string text;
        if (!ftp_read_reply(ctrl.ssl, ftp_buf, code, text, error) || (code != 150 && code != 125)) {
            if (error.empty())
                error = text;
            return false;
        }

        std::ifstream in(local_path, std::ios::binary);
        if (!in) {
            error = "Could not read the packed print file";
            return false;
        }
        in.seekg(0, std::ios::end);
        const size_t total = static_cast<size_t>(in.tellg());
        in.seekg(0);
        char chunk[16 * 1024];
        size_t sent = 0;
        while (in) {
            in.read(chunk, sizeof(chunk));
            const std::streamsize got = in.gcount();
            if (got > 0 && !ssl_write_all(data.ssl, chunk, static_cast<size_t>(got))) {
                error = "Upload to the printer failed";
                return false;
            }
            sent += static_cast<size_t>(std::max<std::streamsize>(got, 0));
            if (on_bytes && !on_bytes(sent, total)) {
                error = "Upload cancelled";
                return false;
            }
        }
        SSL_shutdown(data.ssl);
        SSL_free(data.ssl);
        data.ssl = nullptr;
        ::close(data.fd);
        data.fd = -1;
        return ftp_expect(ctrl.ssl, ftp_buf, 226, error);
    }

    bool connected() const { return m_connected && m_mqtt.ssl != nullptr; }

    void set_mqtt_recv_timeout_ms(int ms)
    {
        if (m_mqtt.fd < 0)
            return;
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
        setsockopt(m_mqtt.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    bool mqtt_ping(std::string &error)
    {
        if (!mqtt_write_packet(m_mqtt.ssl, 0xC0, {})) {
            error = "MQTT keepalive failed";
            m_connected = false;
            return false;
        }
        return true;
    }

    // Read one MQTT packet. Timeout leaves the connection open.
    SslIo mqtt_read_timeout(unsigned char &type, std::string &body, std::string &error)
    {
        std::string raw = std::move(m_mqtt_pending);
        m_mqtt_pending.clear();
        auto pull = [&](size_t need) -> SslIo {
            while (raw.size() < need) {
                const SslIo st = ssl_read_status(m_mqtt.ssl, raw, error);
                if (st != SslIo::Data) {
                    m_mqtt_pending = std::move(raw);
                    if (st == SslIo::Closed)
                        m_connected = false;
                    return st;
                }
            }
            return SslIo::Data;
        };
        if (pull(2) != SslIo::Data)
            return m_connected ? SslIo::Timeout : SslIo::Closed;
        type = static_cast<unsigned char>(raw[0]);
        size_t len = 0;
        size_t mult = 1;
        size_t i = 1;
        for (;;) {
            if (pull(i + 1) != SslIo::Data)
                return m_connected ? SslIo::Timeout : SslIo::Closed;
            const unsigned char byte = static_cast<unsigned char>(raw[i++]);
            len += static_cast<size_t>(byte & 0x7f) * mult;
            if ((byte & 0x80) == 0)
                break;
            mult *= 128;
            if (mult > 128 * 128 * 128) {
                error = "MQTT packet is too long";
                m_connected = false;
                return SslIo::Closed;
            }
        }
        if (pull(i + len) != SslIo::Data)
            return m_connected ? SslIo::Timeout : SslIo::Closed;
        body.assign(raw, i, len);
        if (raw.size() > i + len)
            m_mqtt_pending.assign(raw, i + len, std::string::npos);
        if ((type & 0xF0) == 0x30)
            m_last_report = mqtt_publish_payload(type, body);
        else if ((type & 0xF0) == 0xC0) {
            // The printer's ping has to be answered or it drops the session.
            std::vector<unsigned char> empty;
            if (!mqtt_write_packet(m_mqtt.ssl, 0xD0, empty)) {
                error = "MQTT keepalive failed";
                m_connected = false;
                return SslIo::Closed;
            }
        } else if ((type & 0xF0) == 0xE0) {
            error = "The printer closed the MQTT connection";
            m_connected = false;
            return SslIo::Closed;
        }
        return SslIo::Data;
    }

    // Block until a report contains needle, or the deadline passes.
    // On timeout, reply is the last report seen (possibly empty).
    bool wait_containing(const std::string &needle, int timeout_ms, std::string &reply, std::string &error)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            unsigned char type = 0;
            std::string body;
            const SslIo st = mqtt_read_timeout(type, body, error);
            if (st == SslIo::Timeout)
                continue;
            if (st == SslIo::Closed)
                return false;
            if ((type & 0xF0) != 0x30)
                continue;
            if (!needle.empty() && m_last_report.find(needle) != std::string::npos) {
                reply = m_last_report;
                return true;
            }
        }
        reply = m_last_report;
        error = "The printer did not answer that command.";
        return false;
    }

    // Read whatever is waiting, and answer a server ping. Timeout is success.
    bool poll_once(std::string &error)
    {
        unsigned char type = 0;
        std::string body;
        const SslIo st = mqtt_read_timeout(type, body, error);
        (void) type;
        (void) body;
        if (st == SslIo::Closed)
            return false;
        return true;
    }

private:
    std::string m_host;
    std::string m_access_code;
    std::string m_serial;
    SSL_CTX *m_ctx = nullptr;
    TlsSocket m_mqtt;
    std::string m_mqtt_pending;
    std::string m_last_report;
    bool m_connected = false;
};

#endif // !_WIN32

bool pack_gcode_3mf(const std::string &gcode_path, const std::string &zip_path, std::string &error)
{
    std::ifstream in(gcode_path, std::ios::binary);
    if (!in) {
        error = "Could not read the G-code file";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string gcode = ss.str();

    mz_zip_archive zip{};
    mz_zip_zero_struct(&zip);
    if (!open_zip_writer(&zip, zip_path)) {
        error = "Could not pack the print file";
        return false;
    }
    const mz_bool added = mz_zip_writer_add_mem(&zip, "Metadata/plate_1.gcode", gcode.data(), gcode.size(), MZ_BEST_SPEED);
    const mz_bool finalized = added && mz_zip_writer_finalize_archive(&zip);
    if (!finalized || !close_zip_writer(&zip)) {
        error = "Could not pack the print file";
        return false;
    }
    return true;
}

} // namespace

BambuLanCredentials bambu_lan_credentials(const DynamicPrintConfig *config)
{
    BambuLanCredentials creds;
    if (!config)
        return creds;
    if (const auto *host = config->option<ConfigOptionString>("print_host"))
        creds.host = normalize_bambu_host(host->value);
    if (const auto *code = config->option<ConfigOptionString>("printhost_apikey"))
        creds.access_code = code->value;
    if (const auto *serial = config->option<ConfigOptionString>("bambu_device_serial"))
        creds.serial = serial->value;
    return creds;
}

BambuLan::BambuLan(DynamicPrintConfig *config)
{
    if (!config)
        return;
    const BambuLanCredentials creds = bambu_lan_credentials(config);
    m_host = creds.host;
    m_access_code = creds.access_code;
    m_serial = creds.serial;
}

const char *BambuLan::get_name() const
{
    return "Bambu Lab";
}

wxString BambuLan::get_test_ok_msg() const
{
    return _L("Connected to the Bambu Lab printer in Developer LAN mode.");
}

bool BambuLan::test(wxString &msg) const
{
#if defined(_WIN32)
    msg = _L("Bambu Developer LAN mode is not built for Windows yet.");
    return false;
#else
    if (m_host.empty() || m_access_code.empty() || m_serial.empty()) {
        msg = _L("Enter the printer IP, the LAN access code, and the serial number.");
        return false;
    }
    BambuSession session(m_host, m_access_code, m_serial);
    if (!session.ok()) {
        msg = _L("TLS setup failed.");
        return false;
    }
    std::string error;
    if (!session.mqtt_connect(error) || !session.mqtt_wait_report(error)) {
        msg = wxString::FromUTF8(error);
        return false;
    }
    return true;
#endif
}

bool BambuLan::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn) const
{
#if defined(_WIN32)
    error_fn(_L("Bambu Developer LAN mode is not built for Windows yet."));
    return false;
#else
    if (m_host.empty() || m_access_code.empty() || m_serial.empty()) {
        error_fn(_L("Enter the printer IP, the LAN access code, and the serial number."));
        return false;
    }
    const std::string remote = safe_upload_name(upload_data.upload_path.stem().string());
    const boost::filesystem::path packed = boost::filesystem::temp_directory_path() / remote;
    std::string error;
    const std::string progress_buffer;
    bool cancel = false;
    auto report = [&](size_t sent, size_t total) {
        if (!progress_fn)
            return true;
        progress_fn(Http::Progress(0, 0, total, sent, progress_buffer), cancel);
        return !cancel;
    };
    if (!pack_gcode_3mf(upload_data.source_path.string(), packed.string(), error)) {
        error_fn(wxString::FromUTF8(error));
        return false;
    }
    BambuSession session(m_host, m_access_code, m_serial);
    if (!session.ok() || !session.ftps_upload(packed.string(), remote, error, report)) {
        boost::filesystem::remove(packed);
        error_fn(wxString::FromUTF8(error.empty() ? "Upload failed" : error));
        return false;
    }
    boost::filesystem::remove(packed);
    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return true;
    if (!session.mqtt_connect(error)) {
        error_fn(wxString::FromUTF8(error));
        return false;
    }
    const std::string payload =
        std::string("{\"print\":{\"sequence_id\":\"1\",\"command\":\"project_file\",")
        + "\"param\":\"Metadata/plate_1.gcode\","
        + "\"subtask_name\":\"" + json_escape(remote) + "\","
        + "\"file\":\"" + json_escape(remote) + "\","
        + "\"url\":\"ftp:///" + json_escape(remote) + "\","
        + "\"md5\":\"\",\"project_id\":\"0\",\"profile_id\":\"0\",\"task_id\":\"0\",\"subtask_id\":\"0\","
        + "\"timelapse\":false,\"bed_type\":\"auto\",\"bed_leveling\":true,\"bed_levelling\":true,"
        + "\"flow_cali\":false,\"vibration_cali\":false,\"layer_inspect\":false,\"use_ams\":false,\"ams_mapping\":[0]}}";
    if (!session.mqtt_publish(payload, error)) {
        error_fn(wxString::FromUTF8(error));
        return false;
    }
    return true;
#endif
}

struct BambuLanSession::Impl
{
    std::string host;
    std::string access_code;
    std::string serial;
    std::mutex mu;
    std::condition_variable cv;
    struct Job {
        std::string payload;
        std::string needle;
        int timeout_ms = 0;
        ResultFn done;
    };
    std::deque<Job> jobs;
    bool stop = false;
    bool started = false;
    std::thread worker;
};

namespace {

#if !defined(_WIN32)

constexpr int kMqttPollMs = 200;
constexpr int kMqttKeepaliveS = 15;

bool open_control_session(const std::string &host, const std::string &access_code, const std::string &serial,
                          std::unique_ptr<BambuSession> &session, std::string &error)
{
    session = std::make_unique<BambuSession>(host, access_code, serial);
    if (!session->ok() || !session->mqtt_connect(error) || !session->mqtt_wait_report(error)) {
        session.reset();
        if (error.empty())
            error = "Could not connect to the printer.";
        return false;
    }
    session->set_mqtt_recv_timeout_ms(kMqttPollMs);
    return true;
}

#endif

} // namespace

BambuLanSession::BambuLanSession(std::string host, std::string access_code, std::string serial)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->host = std::move(host);
    m_impl->access_code = std::move(access_code);
    m_impl->serial = std::move(serial);
}

BambuLanSession::~BambuLanSession()
{
    stop();
}

void BambuLanSession::stop()
{
    if (!m_impl)
        return;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->stop = true;
    }
    m_impl->cv.notify_all();
    if (m_impl->worker.joinable())
        m_impl->worker.join();
}

void BambuLanSession::start(ResultFn on_connect)
{
    if (!m_impl)
        return;
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (m_impl->started)
            return;
        m_impl->started = true;
        m_impl->stop = false;
    }

#if defined(_WIN32)
    if (on_connect)
        on_connect(false, {}, "Bambu Developer LAN mode is not built for Windows yet.");
    return;
#else
    m_impl->worker = std::thread([impl = m_impl.get(), on_connect = std::move(on_connect)] {
        std::unique_ptr<BambuSession> session;
        std::string error;
        const bool up = open_control_session(impl->host, impl->access_code, impl->serial, session, error);
        if (on_connect)
            on_connect(up, {}, error);
        if (!up)
            return;

        auto last_ping = std::chrono::steady_clock::now();
        while (true) {
            Impl::Job job;
            bool have_job = false;
            {
                std::unique_lock<std::mutex> lock(impl->mu);
                impl->cv.wait_for(lock, std::chrono::milliseconds(kMqttPollMs), [&] {
                    return impl->stop || !impl->jobs.empty();
                });
                if (impl->stop && impl->jobs.empty())
                    return;
                if (impl->stop && !impl->jobs.empty()) {
                    // Drop queued work once the dialog is closing.
                    impl->jobs.clear();
                    return;
                }
                if (!impl->jobs.empty()) {
                    job = std::move(impl->jobs.front());
                    impl->jobs.pop_front();
                    have_job = true;
                }
            }

            if (!session || !session->connected()) {
                // Reconnect when a command is waiting. An idle dialog does not
                // hammer the printer after the socket drops.
                if (!have_job)
                    continue;
                std::string reconnect_error;
                if (!open_control_session(impl->host, impl->access_code, impl->serial, session, reconnect_error)) {
                    if (job.done)
                        job.done(false, {}, reconnect_error);
                    continue;
                }
                last_ping = std::chrono::steady_clock::now();
            }

            if (have_job) {
                std::string pub_error;
                std::string reply;
                bool ok = session->mqtt_publish(job.payload, pub_error);
                if (ok && !job.needle.empty()) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(job.timeout_ms);
                    ok = false;
                    while (std::chrono::steady_clock::now() < deadline) {
                        {
                            std::lock_guard<std::mutex> lock(impl->mu);
                            if (impl->stop) {
                                pub_error = "The printer session is closed.";
                                break;
                            }
                        }
                        std::string piece;
                        std::string slice_error;
                        if (session->wait_containing(job.needle, kMqttPollMs, piece, slice_error)) {
                            reply = std::move(piece);
                            ok = true;
                            pub_error.clear();
                            break;
                        }
                        if (!piece.empty())
                            reply = std::move(piece);
                        if (!session->connected()) {
                            pub_error = slice_error.empty() ? "The printer closed the MQTT connection" : slice_error;
                            break;
                        }
                    }
                    if (!ok && pub_error.empty())
                        pub_error = "The printer did not answer that command.";
                }
                if (job.done)
                    job.done(ok, reply, pub_error);
                continue;
            }

            std::string poll_error;
            if (!session->poll_once(poll_error))
                session.reset();
            const auto now = std::chrono::steady_clock::now();
            if (session && now - last_ping > std::chrono::seconds(kMqttKeepaliveS)) {
                std::string ping_error;
                if (!session->mqtt_ping(ping_error))
                    session.reset();
                last_ping = now;
            }
        }
    });
#endif
}

void BambuLanSession::publish(std::string json, std::string await_containing, int timeout_ms, ResultFn on_done)
{
    if (!m_impl) {
        if (on_done)
            on_done(false, {}, "The printer session is closed.");
        return;
    }
#if defined(_WIN32)
    if (on_done)
        on_done(false, {}, "Bambu Developer LAN mode is not built for Windows yet.");
    return;
#else
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (m_impl->stop || !m_impl->started) {
            if (on_done)
                on_done(false, {}, m_impl->stop ? "The printer session is closed." : "The printer is not connected.");
            return;
        }
        m_impl->jobs.push_back(Impl::Job{std::move(json), std::move(await_containing), timeout_ms, std::move(on_done)});
    }
    m_impl->cv.notify_all();
#endif
}

} // namespace Slic3r
