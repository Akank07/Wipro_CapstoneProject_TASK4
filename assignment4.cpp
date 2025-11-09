

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const int DEFAULT_PORT = 12345;
static const int BACKLOG = 10;
static const size_t BUFFER_SIZE = 8192;

// If NO_NETWORK is NOT defined, include socket headers and compile network code.
#ifndef NO_NETWORK

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Helper: send all bytes
ssize_t sendAll(int sock, const char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(sock, buf + total, len - total, 0);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            return -1;
        }
        total += sent;
    }
    return (ssize_t)total;
}

// Helper: recv exact number of bytes
ssize_t recvExact(int sock, char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(sock, buf + total, len - total, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        total += r;
    }
    return (ssize_t)total;
}

// Read a line (ending with '\n'), return without newline. Returns false on EOF/error.
bool readLine(int sock, std::string& outLine) {
    outLine.clear();
    char ch;
    while (true) {
        ssize_t r = recv(sock, &ch, 1, 0);
        if (r == 1) {
            if (ch == '\n') return true;
            if (ch != '\r') outLine.push_back(ch);
        } else if (r == 0) {
            // connection closed
            return false;
        } else {
            if (errno == EINTR) continue;
            return false;
        }
    }
}

// Send a text line ending with '\n'
bool sendLine(int sock, const std::string& line) {
    std::string withnl = line + "\n";
    return sendAll(sock, withnl.data(), withnl.size()) == (ssize_t)withnl.size();
}

// Sanitize filename: disallow path separators and parent traversal
bool isSafeFilename(const std::string& fn) {
    if (fn.empty()) return false;
    if (fn.find('/') != std::string::npos) return false;
    if (fn.find('\\') != std::string::npos) return false;
    if (fn.find("..") != std::string::npos) return false;
    return true;
}

// Server-side handling of a single client
void handle_client(int client_sock, fs::path serve_dir) {
    // Make sure serve_dir exists
    try {
        if (!fs::exists(serve_dir)) fs::create_directories(serve_dir);
    } catch (...) {}

    std::string line;
    while (true) {
        bool ok = readLine(client_sock, line);
        if (!ok) break; // connection closed or error

        if (line.rfind("LIST", 0) == 0) {
            // produce listing
            std::ostringstream oss;
            for (auto& entry : fs::directory_iterator(serve_dir)) {
                std::string name = entry.path().filename().string();
                if (entry.is_regular_file()) {
                    oss << name << "\tfile\n";
                } else if (entry.is_directory()) {
                    oss << name << "\tdir\n";
                } else {
                    oss << name << "\tother\n";
                }
            }
            std::string listing = oss.str();
            // Send OK\n<size>\n<data>
            if (!sendLine(client_sock, "OK")) break;
            if (!sendLine(client_sock, std::to_string(listing.size()))) break;
            if (!listing.empty()) {
                if (sendAll(client_sock, listing.data(), listing.size()) < 0) break;
            }
        } else if (line.rfind("GET ", 0) == 0) {
            std::string filename = line.substr(4);
            if (!isSafeFilename(filename)) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "Invalid filename");
                continue;
            }
            fs::path filep = serve_dir / filename;
            if (!fs::exists(filep) || !fs::is_regular_file(filep)) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "File not found");
                continue;
            }
            std::uintmax_t fsize = fs::file_size(filep);
            std::ifstream ifs(filep, std::ios::binary);
            if (!ifs) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "Failed to open file");
                continue;
            }
            sendLine(client_sock, "OK");
            sendLine(client_sock, std::to_string((unsigned long long)fsize));
            // send file in chunks
            std::vector<char> buf(BUFFER_SIZE);
            while (ifs) {
                ifs.read(buf.data(), buf.size());
                std::streamsize r = ifs.gcount();
                if (r > 0) {
                    if (sendAll(client_sock, buf.data(), (size_t)r) < 0) break;
                }
            }
        } else if (line.rfind("PUT ", 0) == 0) {
            std::string filename = line.substr(4);
            if (!isSafeFilename(filename)) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "Invalid filename");
                continue;
            }
            // read size line
            std::string sizeLine;
            if (!readLine(client_sock, sizeLine)) break;
            unsigned long long size = 0;
            try {
                size = std::stoull(sizeLine);
            } catch (...) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "Invalid size header");
                continue;
            }
            fs::path filep = serve_dir / filename;
            // Write file
            std::ofstream ofs(filep, std::ios::binary);
            if (!ofs) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "Failed to create file");
                // drain incoming data to keep stream consistent
                unsigned long long toDiscard = size;
                std::vector<char> discardBuf(4096);
                while (toDiscard > 0) {
                    size_t chunk = (toDiscard > discardBuf.size()) ? discardBuf.size() : (size_t)toDiscard;
                    ssize_t got = recvExact(client_sock, discardBuf.data(), chunk);
                    if (got <= 0) break;
                    toDiscard -= (unsigned long long)got;
                }
                continue;
            }
            unsigned long long remaining = size;
            std::vector<char> buf(BUFFER_SIZE);
            bool err = false;
            while (remaining > 0) {
                size_t chunk = (remaining > buf.size()) ? buf.size() : (size_t)remaining;
                ssize_t got = recvExact(client_sock, buf.data(), chunk);
                if (got <= 0) {
                    err = true;
                    break;
                }
                ofs.write(buf.data(), got);
                if (!ofs) {
                    err = true;
                    break;
                }
                remaining -= (unsigned long long)got;
            }
            ofs.close();
            if (err) {
                sendLine(client_sock, "ERR");
                sendLine(client_sock, "Transfer error");
            } else {
                sendLine(client_sock, "OK");
            }
        } else if (line.rfind("QUIT", 0) == 0) {
            break;
        } else {
            sendLine(client_sock, "ERR");
            sendLine(client_sock, "Unknown command");
        }
    }
    close(client_sock);
}

// Server accept loop
void run_server(int port, fs::path serve_dir) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << strerror(errno) << "\n";
        close(listen_sock);
        return;
    }

    if (listen(listen_sock, BACKLOG) < 0) {
        std::cerr << "listen() failed: " << strerror(errno) << "\n";
        close(listen_sock);
        return;
    }

    std::cout << "Server listening on port " << port << ", serving directory: " << serve_dir << "\n";

    std::vector<std::thread> threads;
    std::atomic<bool> running(true);

    while (running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            if (errno == EINTR) continue;
            std::cerr << "accept() failed: " << strerror(errno) << "\n";
            break;
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        std::cout << "Accepted connection from " << ipstr << ":" << ntohs(client_addr.sin_port) << "\n";

        // spawn thread to handle client
        threads.emplace_back([client_sock, serve_dir]() {
            handle_client(client_sock, serve_dir);
        });

        // Clean up finished threads occasionally
        if (threads.size() > 50) {
            for (auto it = threads.begin(); it != threads.end();) {
                if (it->joinable()) {
                    it->join();
                    it = threads.erase(it);
                } else ++it;
            }
        }
    }

    // join remaining threads
    for (auto& t : threads) if (t.joinable()) t.join();

    close(listen_sock);
}

// Client helper: receive a response that begins with a line
bool recvResponseOKAndSize(int sock, unsigned long long& sizeOut, std::string& errMsg) {
    std::string status;
    if (!readLine(sock, status)) return false;
    if (status == "OK") {
        std::string sizeLine;
        if (!readLine(sock, sizeLine)) return false;
        try {
            sizeOut = std::stoull(sizeLine);
        } catch (...) {
            return false;
        }
        return true;
    } else if (status == "ERR") {
        std::string msg;
        if (!readLine(sock, msg)) return false;
        errMsg = msg;
        return false;
    } else {
        errMsg = "Unexpected response";
        return false;
    }
}

// Client interactive session
void run_client(const std::string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return;
    }

    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &servaddr.sin_addr) <= 0) {
        std::cerr << "inet_pton() failed for host " << host << "\n";
        close(sock);
        return;
    }

    if (connect(sock, (sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        std::cerr << "connect() failed: " << strerror(errno) << "\n";
        close(sock);
        return;
    }

    std::cout << "Connected to " << host << ":" << port << "\n";
    std::string cmd;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, cmd)) break;
        if (cmd.empty()) continue;

        if (cmd.rfind("LIST", 0) == 0) {
            if (!sendLine(sock, "LIST")) break;
            unsigned long long size = 0;
            std::string err;
            if (!recvResponseOKAndSize(sock, size, err)) {
                std::cerr << "Server error: " << err << "\n";
                continue;
            }
            std::vector<char> buf((size_t)size);
            if (size > 0) {
                if (recvExact(sock, buf.data(), (size_t)size) <= 0) {
                    std::cerr << "Failed to read listing\n";
                    continue;
                }
            }
            std::cout << "Server listing:\n";
            std::cout.write(buf.data(), (std::streamsize)size);
            std::cout << "\n";
        } else if (cmd.rfind("GET ", 0) == 0) {
            std::string filename = cmd.substr(4);
            if (filename.empty()) {
                std::cerr << "Usage: GET <filename>\n";
                continue;
            }
            if (!sendLine(sock, "GET " + filename)) break;
            unsigned long long size = 0;
            std::string err;
            if (!recvResponseOKAndSize(sock, size, err)) {
                std::cerr << "Server error: " << err << "\n";
                continue;
            }
            std::ofstream ofs(filename, std::ios::binary);
            if (!ofs) {
                std::cerr << "Failed to open local file for writing\n";
                // drain incoming bytes
                unsigned long long remaining = size;
                std::vector<char> tmp(4096);
                while (remaining > 0) {
                    size_t chunk = (remaining > tmp.size()) ? tmp.size() : (size_t)remaining;
                    if (recvExact(sock, tmp.data(), chunk) <= 0) break;
                    remaining -= chunk;
                }
                continue;
            }
            std::vector<char> buf(BUFFER_SIZE);
            unsigned long long remaining = size;
            while (remaining > 0) {
                size_t chunk = (remaining > buf.size()) ? buf.size() : (size_t)remaining;
                ssize_t got = recvExact(sock, buf.data(), chunk);
                if (got <= 0) {
                    std::cerr << "Connection error during download\n";
                    break;
                }
                ofs.write(buf.data(), got);
                remaining -= (unsigned long long)got;
            }
            ofs.close();
            std::cout << "Downloaded " << filename << " (" << size << " bytes)\n";
        } else if (cmd.rfind("PUT ", 0) == 0) {
            std::string filename = cmd.substr(4);
            if (filename.empty()) {
                std::cerr << "Usage: PUT <filename>\n";
                continue;
            }
            if (!fs::exists(filename) || !fs::is_regular_file(filename)) {
                std::cerr << "Local file not found: " << filename << "\n";
                continue;
            }
            unsigned long long fsize = fs::file_size(filename);
            if (!sendLine(sock, "PUT " + filename)) break;
            // send size header
            if (!sendLine(sock, std::to_string((unsigned long long)fsize))) break;
            // send file bytes
            std::ifstream ifs(filename, std::ios::binary);
            if (!ifs) {
                std::cerr << "Failed to open local file for reading\n";
                // inform server? We already sent header; attempt to close.
                continue;
            }
            std::vector<char> buf(BUFFER_SIZE);
            while (ifs) {
                ifs.read(buf.data(), buf.size());
                std::streamsize r = ifs.gcount();
                if (r > 0) {
                    if (sendAll(sock, buf.data(), (size_t)r) < 0) {
                        std::cerr << "Send error\n";
                        break;
                    }
                }
            }
            // read server response
            std::string status;
            if (!readLine(sock, status)) {
                std::cerr << "No response after PUT\n";
                break;
            }
            if (status == "OK") {
                std::cout << "Upload successful\n";
            } else if (status == "ERR") {
                std::string msg;
                readLine(sock, msg);
                std::cerr << "Server error: " << msg << "\n";
            } else {
                std::cerr << "Unexpected server response: " << status << "\n";
            }
        } else if (cmd.rfind("QUIT", 0) == 0) {
            sendLine(sock, "QUIT");
            break;
        } else {
            std::cout << "Unknown command. Supported: LIST, GET <file>, PUT <file>, QUIT\n";
        }
    }

    close(sock);
    std::cout << "Disconnected.\n";
}

#else // NO_NETWORK

// When NO_NETWORK is defined we provide a local mode that simulates client operations
// by directly operating on a serve_dir on the filesystem. This compiles in environments
// with no socket headers available (e.g., online editors that restrict networking).

// Sanitize filename: disallow path separators and parent traversal
bool isSafeFilename(const std::string& fn) {
    if (fn.empty()) return false;
    if (fn.find('/') != std::string::npos) return false;
    if (fn.find('\\') != std::string::npos) return false;
    if (fn.find("..") != std::string::npos) return false;
    return true;
}

void do_list(fs::path serve_dir) {
    try {
        if (!fs::exists(serve_dir)) fs::create_directories(serve_dir);
    } catch (...) {}
    for (auto& entry : fs::directory_iterator(serve_dir)) {
        std::string name = entry.path().filename().string();
        if (entry.is_regular_file()) {
            std::cout << name << "\tfile\n";
        } else if (entry.is_directory()) {
            std::cout << name << "\tdir\n";
        } else {
            std::cout << name << "\tother\n";
        }
    }
}

void do_get(fs::path serve_dir, const std::string& filename) {
    if (!isSafeFilename(filename)) {
        std::cerr << "Invalid filename\n";
        return;
    }
    fs::path src = serve_dir / filename;
    if (!fs::exists(src) || !fs::is_regular_file(src)) {
        std::cerr << "File not found on server: " << filename << "\n";
        return;
    }
    std::ifstream ifs(src, std::ios::binary);
    if (!ifs) {
        std::cerr << "Failed to open server file\n";
        return;
    }
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to open local file for writing\n";
        return;
    }
    std::vector<char> buf(BUFFER_SIZE);
    while (ifs) {
        ifs.read(buf.data(), buf.size());
        std::streamsize r = ifs.gcount();
        if (r > 0) ofs.write(buf.data(), r);
    }
    std::cout << "Downloaded " << filename << " (" << fs::file_size(src) << " bytes)\n";
}

void do_put(fs::path serve_dir, const std::string& filename) {
    if (!isSafeFilename(filename)) {
        std::cerr << "Invalid filename\n";
        return;
    }
    fs::path src = filename;
    if (!fs::exists(src) || !fs::is_regular_file(src)) {
        std::cerr << "Local file not found: " << filename << "\n";
        return;
    }
    try {
        if (!fs::exists(serve_dir)) fs::create_directories(serve_dir);
    } catch (...) {}
    fs::path dst = serve_dir / filename;
    std::ifstream ifs(src, std::ios::binary);
    if (!ifs) {
        std::cerr << "Failed to open local file for reading\n";
        return;
    }
    std::ofstream ofs(dst, std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to create server file\n";
        return;
    }
    std::vector<char> buf(BUFFER_SIZE);
    while (ifs) {
        ifs.read(buf.data(), buf.size());
        std::streamsize r = ifs.gcount();
        if (r > 0) ofs.write(buf.data(), r);
    }
    std::cout << "Uploaded " << filename << " to server directory\n";
}

void run_local(fs::path serve_dir) {
    std::cout << "Running in local mode (NO_NETWORK). Serving directory: " << serve_dir << "\n";
    std::string cmd;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, cmd)) break;
        if (cmd.empty()) continue;

        if (cmd.rfind("LIST", 0) == 0) {
            do_list(serve_dir);
        } else if (cmd.rfind("GET ", 0) == 0) {
            std::string filename = cmd.substr(4);
            do_get(serve_dir, filename);
        } else if (cmd.rfind("PUT ", 0) == 0) {
            std::string filename = cmd.substr(4);
            do_put(serve_dir, filename);
        } else if (cmd.rfind("QUIT", 0) == 0) {
            break;
        } else {
            std::cout << "Unknown command. Supported: LIST, GET <file>, PUT <file>, QUIT\n";
        }
    }
    std::cout << "Local mode exited.\n";
}

#endif // NO_NETWORK

// Simple argument parser
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n"
#ifndef NO_NETWORK
                  << "  Server: " << argv[0] << " --server [--port <port>] [--dir <serve_dir>]\n"
                  << "  Client: " << argv[0] << " --client <host> [--port <port>]\n";
#else
                  << "  Local (no-network) mode: " << argv[0] << " --local [--dir <serve_dir>]\n";
#endif
        return 0;
    }

    std::string mode = argv[1];
#ifndef NO_NETWORK
    if (mode == "--server") {
        int port = DEFAULT_PORT;
        fs::path dir = fs::current_path();
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--port" && i + 1 < argc) {
                port = std::stoi(argv[++i]);
            } else if (a == "--dir" && i + 1 < argc) {
                dir = argv[++i];
            }
        }
        run_server(port, dir);
    } else if (mode == "--client") {
        if (argc < 3) {
            std::cerr << "Client requires host argument\n";
            return 1;
        }
        std::string host = argv[2];
        int port = DEFAULT_PORT;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--port" && i + 1 < argc) {
                port = std::stoi(argv[++i]);
            }
        }
        run_client(host, port);
    } else {
        std::cerr << "Unknown mode. Use --server or --client\n";
        return 1;
    }
#else
    if (mode == "--local") {
        fs::path dir = fs::current_path();
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--dir" && i + 1 < argc) {
                dir = argv[++i];
            }
        }
        run_local(dir);
    } else {
        std::cerr << "Unknown mode. Use --local (compiled with NO_NETWORK)\n";
        return 1;
    }
#endif

    return 0;
}
