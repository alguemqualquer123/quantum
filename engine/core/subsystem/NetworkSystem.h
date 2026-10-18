#pragma once

#include "../ecs/SubSystem.h"
#include "../component/Transform.h"
#include "../ecs/components/NetworkComponent.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace doriax {

class NetworkSystem : public SubSystem {
public:
    NetworkSystem(Scene* scene);
    ~NetworkSystem();

    // Initialize as client or server based on command‑line args.
    void startServer(uint16_t port);
    void startClient(const std::string& address, uint16_t port);

    // Called each frame to process incoming packets and send updates.
    void update(double dt) override;
    void load() override {}
    void draw() override {}
    void destroy() override;

    // Register a new entity for replication.
    void registerEntity(Entity entity);

private:
    enum class Mode { None, Server, Client } mode = Mode::None;
    uint16_t listenPort = 0;
    std::string serverAddress;
    std::thread networkThread;
    std::atomic<bool> running{false};
    std::mutex entityMutex;
    std::vector<Entity> replicatedEntities;

    void networkLoop(); // runs in background thread
    void processIncoming(); // stub – to be filled with real packet handling
    void sendUpdates(); // stub – send transformed data to peers
};

}
