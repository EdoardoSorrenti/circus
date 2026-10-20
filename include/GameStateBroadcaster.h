#pragma once

#include <netinet/in.h>

#include <cstdint>

namespace spqr {

class GameController;

// Sends the GameController's state to the robots as RoboCup GameController packets, broadcast on the
// Circus network: robots that follow the official GameController follow Circus's referee instead.
class GameStateBroadcaster {
    public:
        GameStateBroadcaster() = default;
        ~GameStateBroadcaster();

        GameStateBroadcaster(const GameStateBroadcaster&) = delete;
        GameStateBroadcaster& operator=(const GameStateBroadcaster&) = delete;

        void open();
        void close();
        bool isOpen() const {
            return socket_ >= 0;
        }

        // Sends one packet with the current game state
        void send(const GameController& gc);

    private:
        int socket_ = -1;
        sockaddr_in destination_{};
        uint8_t packetNumber_ = 0;
        bool sendFailed_ = false;  // Report a failing send once, not twice a second
};

}  // namespace spqr
