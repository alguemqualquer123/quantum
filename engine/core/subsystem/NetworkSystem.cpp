#include "NetworkSystem.h"
#include "../Scene.h"
#include "../component/Transform.h"
#include "../ecs/components/NetworkComponent.h"
#include "../ecs/Entity.h"
#include <iostream>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace doriax {

NetworkSystem::NetworkSystem(Scene* scene) : SubSystem(scene) {}
NetworkSystem::~NetworkSystem() { destroy(); }

void NetworkSystem::startServer(uint16_t port) {
    if (mode != Mode::None) return;
    mode = Mode::Server;
    listenPort = port;
    running = true;
    networkThread = std::thread(&NetworkSystem::networkLoop, this);
    std::cout << "[Network] Server started on port " << port << "\n";
}

void NetworkSystem::startClient(const std::string& address, uint16_t port) {
    if (mode != Mode::None) return;
    mode = Mode::Client;
    serverAddress = address;
    listenPort = port;
    running = true;
    networkThread = std::thread(&NetworkSystem::networkLoop, this);
    std::cout << "[Network] Client connecting to " << address << ":" << port << "\n";
}

void NetworkSystem::registerEntity(Entity entity) {
    std::lock_guard<std::mutex> lock(entityMutex);
    replicatedEntities.push_back(entity);
}

void NetworkSystem::update(double dt) {
    // In a real implementation we would process packets here, but for now we just
    // ensure the background thread runs. All heavy work is done in networkLoop().
    (void)dt; // silence unused warning
}

void NetworkSystem::destroy() {
    running = false;
    if (networkThread.joinable()) networkThread.join();
    // Cleanup sockets – omitted for brevity.
}

void NetworkSystem::networkLoop() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        std::cerr << "[Network] WSAStartup failed\n";
        return;
    }
#endif
    // Simple UDP socket for demonstration.
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "[Network] socket creation failed\n";
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listenPort);
    if (mode == Mode::Server) {
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (sockaddr*) &addr, sizeof(addr)) < 0) {
            std::cerr << "[Network] bind failed\n";
#if defined(_WIN32)
            closesocket(sock);
            WSACleanup();
#else
            close(sock);
#endif
            return;
        }
    }
    // Simple loop – receives packets and prints size.
    while (running) {
        char buffer[1024];
        sockaddr_in src{};
        socklen_t srcLen = sizeof(src);
        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*) &src, &srcLen);
        if (received > 0) {
            // For now just log packet size.
            std::cout << "[Network] Received packet " << received << " bytes\n";
        }
        // Sleep a bit to avoid busy‑wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#if defined(_WIN32)
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
}

void NetworkSystem::processIncoming() {
    // Placeholder for packet parsing and entity state updates.
}

void NetworkSystem::sendUpdates() {
    // Placeholder for sending entity transforms to peers.
}

}
