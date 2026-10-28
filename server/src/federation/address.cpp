// server/src/federation/address.cpp
#include "address.h"
#include <stdexcept>

AgentAddress AgentAddress::parse(const std::string& raw) {
    // Expected format: "@username:domain"
    if (raw.empty() || raw[0] != '@') {
        throw std::invalid_argument("Agent address must start with '@'");
    }
    auto atPos = raw.find(':');
    if (atPos == std::string::npos) {
        throw std::invalid_argument("Agent address must contain ':' separating username and domain");
    }
    AgentAddress addr;
    addr.username = raw.substr(1, atPos - 1); // skip '@'
    addr.domain = raw.substr(atPos + 1);
    if (addr.username.empty() || addr.domain.empty()) {
        throw std::invalid_argument("Invalid agent address components");
    }
    return addr;
}

std::string AgentAddress::toString() const {
    return "@" + username + ":" + domain;
}
