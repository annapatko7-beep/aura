// server/src/federation/address.h
#pragma once
#include <string>

// Simple decentralized address, e.g. "@alice:example.com"
struct AgentAddress {
    std::string username; // without leading @
    std::string domain;   // domain part

    static AgentAddress parse(const std::string& raw);
    std::string toString() const;
};
