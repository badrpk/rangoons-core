#include "rangoons.h"
#include <cstdlib>
#include <iostream>

static std::string getenv_str(const char* k, const char* def="") {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(def);
}

int main() {
    Config cfg;
    cfg.db_path = getenv_str("RANGOONS_DB", "rangoons.db");
    cfg.host    = getenv_str("RANGOONS_HOST", "0.0.0.0");
    cfg.port    = (unsigned short) std::stoi(getenv_str("RANGOONS_PORT", "8080"));
    cfg.admin_key = getenv_str("ADMIN_KEY", "");

    if (cfg.admin_key.empty()) {
        std::cerr << "WARNING: ADMIN_KEY not set. Admin panel will be locked.\n";
    }
    return run_server(cfg);
}
