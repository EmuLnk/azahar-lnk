// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "common/logging/log.h"
#include "core/core.h"
#include "core/emulnk/emu_link_server.h"
#include "core/memory.h"

namespace Core::EmuLnk {

static void CloseSocket(socket_t sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

EmuLinkServer& EmuLinkServer::Instance() {
    static EmuLinkServer instance;
    return instance;
}

EmuLinkServer::~EmuLinkServer() {
    Stop();
}

void EmuLinkServer::Start() {
    if (m_running.exchange(true)) {
        return;
    }

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCK) {
        LOG_ERROR(Network, "EmuLinkServer: Failed to create socket");
        m_running = false;
        return;
    }

    // Allow address reuse
    int opt = 1;
#ifdef _WIN32
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt),
               sizeof(opt));
#else
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR(Network, "EmuLinkServer: Failed to bind on port {}", PORT);
        CloseSocket(m_socket);
        m_socket = INVALID_SOCK;
        m_running = false;
        return;
    }

    m_thread = std::thread([this] { ServerLoop(); });

    LOG_INFO(Network, "EmuLinkServer: Started on port {}", PORT);
}

void EmuLinkServer::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    if (m_socket != INVALID_SOCK) {
        CloseSocket(m_socket);
        m_socket = INVALID_SOCK;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_metadata_composite.clear();

    LOG_INFO(Network, "EmuLinkServer: Stopped");

#ifdef _WIN32
    WSACleanup();
#endif
}

void EmuLinkServer::ServerLoop() {
    constexpr std::size_t BUFFER_SIZE = 2048;
    u8 buffer[BUFFER_SIZE];

    while (m_running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        const auto received =
            recvfrom(m_socket, reinterpret_cast<char*>(buffer), BUFFER_SIZE, 0,
                     reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (received <= 0) {
            break;
        }

        const auto received_size = static_cast<std::size_t>(received);

        // EMLKV2 handshake: 6-byte magic -> respond with JSON
        if (received_size == 6 && std::memcmp(buffer, "EMLKV2", 6) == 0) {
            auto& system = Core::System::GetInstance();
            const u64 tid = system.GetTitleID();

            // Lazy-build metadata composite (cached)
            if (m_metadata_composite.empty()) {
                char composite[128];
                std::snprintf(composite, sizeof(composite), "%016llX:%s:%04X:%08X",
                              static_cast<unsigned long long>(tid),
                              system.GetProductCode().c_str(),
                              system.GetRemasterVersion(),
                              system.GetCodeCRC());
                m_metadata_composite = composite;
            }

            char tid_hex[17];
            std::snprintf(tid_hex, sizeof(tid_hex), "%016llX",
                          static_cast<unsigned long long>(tid));

            char json[512];
            std::snprintf(json, sizeof(json),
                "{\"emulator\":\"azahar\",\"game_id\":\"%s\","
                "\"game_hash\":\"%s\",\"platform\":\"3DS\"}",
                tid_hex, m_metadata_composite.c_str());

            sendto(m_socket, json, std::strlen(json), 0,
                   reinterpret_cast<sockaddr*>(&client_addr), client_len);
            continue;
        }

        // Memory read: 8 bytes (u32 address + u32 size)
        if (received_size == 8) {
            u32 address = 0;
            u32 size = 0;
            std::memcpy(&address, buffer, sizeof(u32));
            std::memcpy(&size, buffer + sizeof(u32), sizeof(u32));

            if (size == 0 || size > MAX_READ_SIZE) {
                continue;
            }

            u8 response[MAX_READ_SIZE];
            Core::System::GetInstance().Memory().ReadBlock(address, response, size);

            sendto(m_socket, reinterpret_cast<const char*>(response), size, 0,
                   reinterpret_cast<sockaddr*>(&client_addr), client_len);
            continue;
        }

        // Memory write: 8+ bytes (u32 address + u32 size + data)
        if (received_size > 8) {
            u32 address = 0;
            u32 size = 0;
            std::memcpy(&address, buffer, sizeof(u32));
            std::memcpy(&size, buffer + sizeof(u32), sizeof(u32));

            if (size == 0 || size > received_size - 8) {
                continue;
            }

            // Only allow writing to certain memory regions
            if ((address >= Memory::PROCESS_IMAGE_VADDR &&
                 address <= Memory::PROCESS_IMAGE_VADDR_END) ||
                (address >= Memory::HEAP_VADDR && address <= Memory::HEAP_VADDR_END) ||
                (address >= Memory::LINEAR_HEAP_VADDR &&
                 address <= Memory::LINEAR_HEAP_VADDR_END) ||
                (address >= Memory::N3DS_EXTRA_RAM_VADDR &&
                 address <= Memory::N3DS_EXTRA_RAM_VADDR_END)) {

                Core::System::GetInstance().Memory().WriteBlock(address, buffer + 8, size);
                Core::System::GetInstance().InvalidateCacheRange(address, size);
            }
            continue;
        }
    }
}

} // namespace Core::EmuLnk
