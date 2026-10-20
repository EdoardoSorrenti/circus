#include "GameStateBroadcaster.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "CircusNetwork.h"
#include "GameController.h"
#include "RoboCupGameControlData.h"

namespace spqr {

namespace {

// Every robot container is on the Circus network, so one broadcast reaches all of them
constexpr const char* kBroadcastAddress = UAN_SEVEN_CIU "255.255";

std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

template <typename T>
T clampTo(double value) {
    return static_cast<T>(std::clamp(value, static_cast<double>(std::numeric_limits<T>::min()), static_cast<double>(std::numeric_limits<T>::max())));
}

uint8_t toGcState(GamePhase phase) {
    switch (phase) {
        case READY:
            return STATE_READY;
        case SET:
            return STATE_SET;
        case PLAYING:
            return STATE_PLAYING;
        case FINISH:
            return STATE_FINISHED;
        default:
            return STATE_INITIAL;
    }
}

// Kick-off is not a set play for the GameController: only the kicking team tells it
uint8_t toGcSetPlay(GameSubPhase subPhase) {
    switch (subPhase) {
        case KICKIN:
            return SET_PLAY_THROW_IN;
        case CORNERKICK:
            return SET_PLAY_CORNER_KICK;
        case GOALKICK:
            return SET_PLAY_GOAL_KICK;
        case PENALTYKICK:
            return SET_PLAY_PENALTY_KICK;
        case PUSHINGFREEKICK:
            return SET_PLAY_DIRECT_FREE_KICK;
        default:
            return SET_PLAY_NONE;
    }
}

// The GameController has no generic foul: a foul is sent as pushing
uint8_t toGcPenalty(Penalty penalty) {
    switch (penalty) {
        case LEAVING_THE_FIELD:
            return PENALTY_LEAVING_THE_FIELD;
        case PUSHING:
        case FOUL:
            return PENALTY_PUSHING;
        case ILLEGAL_POSITION:
            return PENALTY_ILLEGAL_POSITIONING;
        default:
            return PENALTY_NONE;
    }
}

// Seconds left in READY, in SET or in the current kick-off or set play, like the GameController's secondary timer
double secondaryTime(const GameController& gc) {
    switch (gc.getCurrentPhase()) {
        case READY:
            return gc.getReadyPhaseDuration() - gc.getCurrentPhaseElapsedTime();
        case SET:
            return gc.getSetPhaseDuration() - gc.getCurrentPhaseElapsedTime();
        case PLAYING:
            if (gc.getCurrentSubPhase() == KICKOFF)
                return gc.getKickOffSubPhaseDuration() - gc.getCurrentSubPhaseElapsedTime();
            if (gc.getCurrentSubPhase() != BALLFREE)
                return gc.getSubPhaseDuration() - gc.getCurrentSubPhaseElapsedTime();
            return 0.0;
        default:
            return 0.0;
    }
}

}  // namespace

GameStateBroadcaster::~GameStateBroadcaster() {
    close();
}

void GameStateBroadcaster::open() {
    if (isOpen())
        return;

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0)
        throw std::runtime_error(std::string("GameStateBroadcaster: socket() failed: ") + std::strerror(errno));

    int enable = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0) {
        const std::string error = std::strerror(errno);
        close();
        throw std::runtime_error("GameStateBroadcaster: SO_BROADCAST failed: " + error);
    }

    destination_ = {};
    destination_.sin_family = AF_INET;
    destination_.sin_port = htons(GAMECONTROLLER_DATA_PORT);
    inet_pton(AF_INET, kBroadcastAddress, &destination_.sin_addr);
    sendFailed_ = false;

    std::cout << "Sending the game state to the robots at " << kBroadcastAddress << ":" << GAMECONTROLLER_DATA_PORT << std::endl;
}

void GameStateBroadcaster::close() {
    if (isOpen()) {
        ::close(socket_);
        socket_ = -1;
    }
}

void GameStateBroadcaster::send(const GameController& gc) {
    if (!isOpen())
        return;

    RoboCupGameControlData packet{};
    std::memcpy(packet.header, GAMECONTROLLER_STRUCT_HEADER, sizeof(packet.header));
    packet.version = GAMECONTROLLER_STRUCT_VERSION;
    packet.packetNumber = packetNumber_++;
    packet.competitionType = COMPETITION_TYPE_LARGE;  // The division the Booster T1 plays in
    packet.gamePhase = GAME_PHASE_NORMAL;
    packet.state = toGcState(gc.getCurrentPhase());
    packet.setPlay = toGcSetPlay(gc.getCurrentSubPhase());
    packet.firstHalf = gc.isFirstHalf() ? 1 : 0;
    packet.secsRemaining = clampTo<int16_t>(gc.getGameDuration() - gc.getGameElapsedTime());
    packet.secondaryTime = clampTo<int16_t>(std::max(0.0, secondaryTime(gc)));

    std::string kickingTeam = "none";
    if (gc.getCurrentSubPhase() == KICKOFF)
        kickingTeam = gc.getKickOffTeam();
    else if (gc.getCurrentSubPhase() != BALLFREE)
        kickingTeam = gc.getCurrentSubPhaseTeam();
    packet.kickingTeam = KICKING_TEAM_NONE;

    int highestPlayerNumber = 0;
    for (const TeamInGame& teamInGame : gc.getTeamsInGame()) {
        const std::shared_ptr<Team> team = teamInGame.getTeam();
        const std::string teamName = toLower(team->name);

        // The GameController lists the team that defends the left goal first: red in Circus
        int index;
        if (teamName == "red")
            index = 0;
        else if (teamName == "blue")
            index = 1;
        else
            continue;

        if (teamName == toLower(kickingTeam))
            packet.kickingTeam = static_cast<uint8_t>(team->number);

        ::TeamInfo& info = packet.teams[index];
        info.teamNumber = static_cast<uint8_t>(team->number);
        info.fieldPlayerColour = (index == 0) ? TEAM_RED : TEAM_BLUE;
        info.goalkeeperColour = info.fieldPlayerColour;
        info.score = static_cast<uint8_t>(teamInGame.getScore());
        info.messageBudget = 12000;  // Circus does not count team messages: the Large division's full budget

        for (const RobotInGame& robotInGame : teamInGame.getRobotsInGame()) {
            const std::shared_ptr<Robot> robot = robotInGame.getRobot();
            if (robot->number < 1 || robot->number > MAX_NUM_PLAYERS)
                continue;

            highestPlayerNumber = std::max(highestPlayerNumber, static_cast<int>(robot->number));
            if (toLower(robot->role) == "goalkeeper")
                info.goalkeeper = robot->number;

            ::RobotInfo& player = info.players[robot->number - 1];
            player.penalty = toGcPenalty(robotInGame.getPenalty());
            if (robotInGame.getIsPenalized()) {
                const double elapsed = robotInGame.getPenalizationElapsedTime(static_cast<int>(gc.getGameElapsedTime()));
                player.secsTillUnpenalised = clampTo<uint8_t>(gc.getPenaltyDuration() - elapsed);
            }
        }
    }
    // Robots read their own entry by player number, so count up to the highest number in the scene
    packet.playersPerTeam = static_cast<uint8_t>(highestPlayerNumber);

    const ssize_t sent
        = sendto(socket_, &packet, sizeof(packet), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
    if (sent < 0 && !sendFailed_) {
        std::cerr << "Failed to send the game state to the robots: " << std::strerror(errno) << std::endl;
        sendFailed_ = true;
    }
}

}  // namespace spqr
