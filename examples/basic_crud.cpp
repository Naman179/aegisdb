// ─────────────────────────────────────────────────────────────────────────────
// basic_crud.cpp – Example demonstrating C++ Client SDK usage (CRUD & CAS)
// ─────────────────────────────────────────────────────────────────────────────
#include <aegis/client.h>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string seed = "127.0.0.1:7001";
    if (argc > 1) {
        seed = argv[1];
    }

    std::cout << "Connecting AegisDB Client SDK to seed node: " << seed << "\n";

    aegis::ClientConfig config;
    config.clusterAddrs = {seed};
    config.clientId = "basic_crud_example";
    config.timeoutMs = 2000;

    aegis::AegisClient client(config);

    // 1. Put
    std::string key = "account:1001";
    std::string value = "{\"owner\": \"Alice\", \"balance\": 2500}";
    uint64_t v1 = 0;
    std::cout << "[1] Inserting key '" << key << "' ...\n";
    if (client.put(key, value, &v1)) {
        std::cout << "    -> Put OK (version: " << v1 << ")\n";
    } else {
        std::cerr << "    -> Put failed\n";
        return 1;
    }

    // 2. Linearizable Get
    std::string readVal;
    uint64_t vRead = 0;
    std::cout << "[2] Reading key '" << key << "' via ReadIndex ...\n";
    if (client.get(key, readVal, &vRead)) {
        std::cout << "    -> Read value: " << readVal << " (version: " << vRead << ")\n";
    } else {
        std::cerr << "    -> Key not found\n";
        return 1;
    }

    // 3. Compare-And-Swap (OCC) - Intentional version mismatch
    std::cout << "[3] Testing CAS mismatch (expecting version 999999) ...\n";
    if (!client.cas(key, "{\"owner\": \"Alice\", \"balance\": 3000}", 999999)) {
        std::cout << "    -> Correctly rejected mismatched version\n";
    } else {
        std::cerr << "    -> Unexpected CAS success!\n";
        return 1;
    }

    // 4. Compare-And-Swap (OCC) - Matching version
    uint64_t vNew = 0;
    std::cout << "[4] Testing CAS match (version: " << v1 << ") ...\n";
    if (client.cas(key, "{\"owner\": \"Alice\", \"balance\": 3000}", v1, &vNew)) {
        std::cout << "    -> CAS succeeded, updated to version: " << vNew << "\n";
    } else {
        std::cerr << "    -> CAS failed\n";
        return 1;
    }

    // 5. Delete
    std::cout << "[5] Deleting key '" << key << "' ...\n";
    if (client.del(key)) {
        std::cout << "    -> Key deleted successfully\n";
    } else {
        std::cerr << "    -> Delete failed\n";
        return 1;
    }

    // 6. Verify Delete
    if (!client.get(key, readVal)) {
        std::cout << "[6] Confirmed key is absent after delete.\n";
    }

    std::cout << "\nAll AegisDB client CRUD operations completed successfully!\n";
    return 0;
}
