// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <thread>
#include "common/common_types.h"

#ifdef _WIN32
#include <winsock2.h>
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
#endif

namespace Core::EmuLnk {

class EmuLinkServer {
public:
    static EmuLinkServer& Instance();

    void Start();
    void Stop();

private:
    EmuLinkServer() = default;
    ~EmuLinkServer();
    EmuLinkServer(const EmuLinkServer&) = delete;
    EmuLinkServer& operator=(const EmuLinkServer&) = delete;

    void ServerLoop();

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    socket_t m_socket{INVALID_SOCK};

    static constexpr u16 PORT = 55355;
    static constexpr std::size_t MAX_READ_SIZE = 1024;
    static constexpr u32 VIRTUAL_SERIAL_ADDR = 0x00200000;
};

} // namespace Core::EmuLnk
