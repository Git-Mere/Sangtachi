#include "hamychi/network/wsa.hpp"

#include <cstdio>

int main() {
    try {
        const hamychi::network::WsaContext wsa;
        std::fprintf(stderr, "INFO wsa.init version=%s\n",
                     wsa.negotiated_version().c_str());
    } catch (const hamychi::network::WsaStartupError& e) {
        std::fprintf(stderr, "ERROR wsa.init code=%d\n", e.code());
        return 2;
    }
    return 0;
}
