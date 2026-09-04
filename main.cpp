#include "kvstore.h"
#include <iostream>
#include <sstream>
#include <string>

// A tiny command loop so you can play with the store by typing commands:
//   set <key> <value>
//   get <key>
//   del <key>
//   exit
//
// The data lives in a file called "data.db" in the current folder.

int main() {
    KVStore store("data.db");

    std::cout << "kvstore (stage 3) - commands: set <k> <v> | get <k> | del <k> | compact | exit\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break; // Ctrl-D ends the loop

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "set") {
            std::string key, value;
            iss >> key;
            std::getline(iss, value);                  // rest of the line = value
            if (!value.empty() && value[0] == ' ')
                value.erase(0, 1);                     // drop the leading space
            store.set(key, value);
            std::cout << "OK\n";
        } else if (cmd == "get") {
            std::string key, out;
            iss >> key;
            if (store.get(key, out)) std::cout << out << "\n";
            else                     std::cout << "(nil)\n";
        } else if (cmd == "del") {
            std::string key;
            iss >> key;
            store.del(key);
            std::cout << "OK\n";
        } else if (cmd == "compact") {
            uint64_t before = store.fileSize();
            store.compact();
            uint64_t after = store.fileSize();
            std::cout << "compacted: " << before << " bytes -> " << after << " bytes\n";
        } else if (cmd == "exit" || cmd == "quit") {
            break;
        } else if (!cmd.empty()) {
            std::cout << "unknown command: " << cmd << "\n";
        }
    }
    return 0;
}
