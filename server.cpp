#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

constexpr const char* kScreenshotDir = "/home/collin/hackmatrix/screenshots";

std::string guess_mime_type(const fs::path& path) {
    auto ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".bmp") return "image/bmp";
    return "application/octet-stream";
}

std::optional<fs::path> latest_screenshot(const fs::path& dir) {
    std::optional<fs::path> latest_path;
    fs::file_time_type latest_time = fs::file_time_type::min();

    try {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return std::nullopt;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const auto current_time = entry.last_write_time();
            if (!latest_path || current_time > latest_time) {
                latest_time = current_time;
                latest_path = entry.path();
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error scanning screenshots: " << ex.what() << std::endl;
        return std::nullopt;
    }

    return latest_path;
}

bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool send_response(int fd, const std::string& status, const std::string& content_type, const std::string& body, bool head_only = false) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << "\r\n";
    header << "Content-Type: " << content_type << "\r\n";
    header << "Content-Length: " << body.size() << "\r\n";
    header << "Connection: close\r\n\r\n";

    const auto header_str = header.str();
    if (!send_all(fd, header_str.data(), header_str.size())) return false;
    if (head_only) return true;
    return send_all(fd, body.data(), body.size());
}

bool send_binary_response(int fd, const std::string& status, const std::string& content_type, const std::vector<char>& body, bool head_only = false) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << "\r\n";
    header << "Content-Type: " << content_type << "\r\n";
    header << "Content-Length: " << body.size() << "\r\n";
    header << "Connection: close\r\n\r\n";

    const auto header_str = header.str();
    if (!send_all(fd, header_str.data(), header_str.size())) return false;
    if (head_only) return true;
    return send_all(fd, body.data(), body.size());
}

void handle_client(int client_fd, std::string screenshot_dir) {
    std::vector<char> buffer(4096);
    const ssize_t n = ::recv(client_fd, buffer.data(), buffer.size() - 1, 0);
    if (n <= 0) {
        ::close(client_fd);
        return;
    }
    buffer[static_cast<size_t>(n)] = '\0';

    std::istringstream request(std::string(buffer.data(), static_cast<size_t>(n)));
    std::string method, path;
    request >> method >> path;

    const bool head_only = (method == "HEAD");

    if (!head_only && method != "GET") {
        send_response(client_fd, "405 Method Not Allowed", "text/plain; charset=UTF-8", "Method Not Allowed\n");
        ::close(client_fd);
        return;
    }

    if (path == "/" || path == "/index.html") {
        auto latest = latest_screenshot(screenshot_dir);
        std::ostringstream body;
        body << "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>Agartha Online</title></head><body>";
        body << "<h1>Agartha Online</h1>";
        body << "<p>Checkout HackMatrix multi-player desktop engine on "
             << "<a href=\"https://github.com/collinalexbell/hackmatrix\">GitHub</a> and "
             << "<a href=\"https://www.youtube.com/watch?v=P2c7QRIz8IA\">YouTube</a>.</p>";
        body << "<div style=\"margin:16px 0;max-width:960px;\">"
             << "<iframe width=\"100%\" height=\"540\" "
             << "src=\"https://www.youtube.com/embed/P2c7QRIz8IA\" "
             << "title=\"HackMatrix\" frameborder=\"0\" "
             << "allow=\"accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share\" "
             << "allowfullscreen></iframe>"
             << "</div>";
        if (latest) {
            body << "<p>Showing latest screenshot from " << screenshot_dir << ":</p>";
            body << "<img src=\"/latest-image\" alt=\"Latest screenshot\" style=\"max-width:100%;height:auto;\">";
        } else {
            body << "<p>No screenshots found in " << screenshot_dir << ".</p>";
        }
        body << "</body></html>";
        send_response(client_fd, "200 OK", "text/html; charset=UTF-8", body.str(), head_only);
        ::close(client_fd);
        return;
    }

    if (path == "/latest-image") {
        auto latest = latest_screenshot(screenshot_dir);
        if (!latest) {
            send_response(client_fd, "404 Not Found", "text/plain; charset=UTF-8", "No screenshots available\n", head_only);
            ::close(client_fd);
            return;
        }

        std::ifstream input(*latest, std::ios::binary);
        if (!input) {
            send_response(client_fd, "500 Internal Server Error", "text/plain; charset=UTF-8", "Failed to open screenshot\n", head_only);
            ::close(client_fd);
            return;
        }

        std::vector<char> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto mime = guess_mime_type(*latest);
        send_binary_response(client_fd, "200 OK", mime, data, head_only);
        ::close(client_fd);
        return;
    }

    send_response(client_fd, "404 Not Found", "text/plain; charset=UTF-8", "Not Found\n", head_only);
    ::close(client_fd);
}

uint16_t parse_port(int argc, char* argv[]) {
    const char* env_port = std::getenv("PORT");
    std::string port_str = "80";
    if (argc > 1) {
        port_str = argv[1];
    } else if (env_port) {
        port_str = env_port;
    }
    try {
        int port_val = std::stoi(port_str);
        if (port_val < 1 || port_val > 65535) {
            throw std::out_of_range("port out of range");
        }
        return static_cast<uint16_t>(port_val);
    } catch (const std::exception& ex) {
        std::cerr << "Invalid port value '" << port_str << "': " << ex.what() << ", falling back to 80\n";
        return 80;
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    const uint16_t port = parse_port(argc, argv);

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        ::close(server_fd);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        ::close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        ::close(server_fd);
        return 1;
    }

    std::cout << "Agartha Online HTTP server listening on port " << port << std::endl;
    std::cout << "Serving latest screenshot from " << kScreenshotDir << std::endl;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        std::thread(handle_client, client_fd, std::string(kScreenshotDir)).detach();
    }
}
