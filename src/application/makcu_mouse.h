#pragma once

#include "pid.h"
#include <cstddef>
#include <string>

// Minimal MAKCU host client using the documented ASCII protocol.
class MakcuMouse {
public:
    MakcuMouse(std::string port, int baud, int ack_timeout_ms = 30, std::string protocol = "ascii");
    ~MakcuMouse();
    MakcuMouse(const MakcuMouse&) = delete;
    MakcuMouse& operator=(const MakcuMouse&) = delete;
    void connect();
    bool move(const Movement& movement);
    void release_all() noexcept;
    const std::string& port() const noexcept { return port_; }
    const std::string& version() const noexcept { return version_; }
private:
    bool write_command(const std::string& command, bool wait_for_reply);
    bool write_bytes(const void* data, std::size_t size);
    bool read_reply(std::string& reply);
    void close() noexcept;
    std::string port_, version_, protocol_;
    int baud_, ack_timeout_ms_;
    void* handle_ = nullptr;
};
