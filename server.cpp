// server.cpp — Stage 4: turn the KV store into a network server.
//
// Instead of typing commands into the program, this version LISTENS on a TCP
// port. Other programs (or you, via a tool like telnet / netcat / a Python
// script) connect over the network and send the same commands:
//     SET <key> <value>
//     GET <key>
//     DEL <key>
//     COMPACT
// Each connected client gets its own thread, so many clients are served at once.
//
// This file is cross-platform: it uses Winsock on Windows and BSD sockets on
// Linux/Mac. The #ifdef blocks pick the right one automatically.

#include "kvstore.h"
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;                 // Windows socket handle type
  #define CLOSESOCK closesocket
  #define BAD_SOCKET INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int socket_t;                    // on Linux/Mac a socket is just an int
  #define CLOSESOCK close
  #define BAD_SOCKET (-1)
#endif

static const int PORT = 6380; // the port clients connect to (Redis uses 6379)

// Handle ONE connected client, start to finish, on its own thread.
// `store` is shared by all clients — the KVStore's internal mutex keeps it safe.
void handleClient(socket_t client, KVStore* store) {
    std::string buffer;            // accumulates bytes until we have a full line
    char chunk[1024];

    while (true) {
        int n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) break;         // client disconnected or error -> done
        buffer.append(chunk, n);

        // TCP is a stream, not neat lines. So we pull out one complete line
        // (everything up to a '\n') at a time and process it.
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back(); // Windows CRLF

            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            std::string reply;
            if (cmd == "SET" || cmd == "set") {
                std::string key, value;
                iss >> key;
                std::getline(iss, value);
                if (!value.empty() && value[0] == ' ') value.erase(0, 1);
                store->set(key, value);
                reply = "OK\n";
            } else if (cmd == "GET" || cmd == "get") {
                std::string key, out;
                iss >> key;
                reply = store->get(key, out) ? out + "\n" : "(nil)\n";
            } else if (cmd == "DEL" || cmd == "del") {
                std::string key;
                iss >> key;
                store->del(key);
                reply = "OK\n";
            } else if (cmd == "COMPACT" || cmd == "compact") {
                store->compact();
                reply = "OK\n";
            } else if (cmd == "QUIT" || cmd == "quit") {
                CLOSESOCK(client);
                return;
            } else if (!cmd.empty()) {
                reply = "ERR unknown command\n";
            }

            if (!reply.empty()) send(client, reply.data(), (int)reply.size(), 0);
        }
    }
    CLOSESOCK(client);
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa); // Windows requires starting up the network library
#endif

    KVStore store("data.db"); // same store, same file — recovers on startup as before

    // 1. Create a listening socket.
    socket_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == BAD_SOCKET) { std::cerr << "socket() failed\n"; return 1; }

    // Allow quick restart on the same port (avoids "address in use").
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // 2. Bind it to our port on all network interfaces.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "bind() failed on port " << PORT << "\n"; return 1;
    }

    // 3. Start listening for connections.
    listen(server, 16);
    std::cout << "kvstore server (stage 4) listening on port " << PORT << std::endl;

    // 4. Accept clients forever; hand each one to its own thread.
    while (true) {
        socket_t client = accept(server, nullptr, nullptr);
        if (client == BAD_SOCKET) continue;
        std::thread(handleClient, client, &store).detach();
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
