#include <iostream>
#include <rohandb/engine/storage_engine.hpp>
int main(int argc, char **argv) {
    try {
        if (argc < 3) {
            std::cerr << "usage: rohandb_cli FILE put KEY VALUE|get KEY|erase KEY|scan "
                         "FROM|verify|checkpoint\n";
            return 2;
        }
        rohandb::StorageEngine engine;
        engine.open(argv[1]);
        const std::string command = argv[2];
        if (command == "put" && argc == 5)
            std::cout << (engine.put(std::stoull(argv[3]), argv[4]) ? "inserted" : "replaced")
                      << '\n';
        else if (command == "get" && argc == 4) {
            const auto value = engine.get(std::stoull(argv[3]));
            if (value)
                std::cout << *value << '\n';
            else
                std::cout << "not found\n";
        } else if (command == "erase" && argc == 4)
            std::cout << (engine.erase(std::stoull(argv[3])) ? "removed" : "not found") << '\n';
        else if (command == "scan" && argc == 4) {
            for (auto it = engine.scan(std::stoull(argv[3])); it.valid(); it.next())
                std::cout << it.key() << '\t' << it.value() << '\n';
        } else if (command == "verify") {
            engine.verify();
            std::cout << "verified\n";
        } else if (command == "checkpoint")
            engine.checkpoint();
        else {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        engine.close();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
