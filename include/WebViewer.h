#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace spqr {

// A web page that shows the simulation (--web [PORT]): the viewport as JPEG frames and the game state, so a
// browser on another computer can follow Circus without seeing its screen. Its one control puts the robots back at
// their start positions for a new game (POST /restart).
class WebViewer : public QObject {
        Q_OBJECT

    public:
        explicit WebViewer(QObject* parent = nullptr);
        ~WebViewer();

        // Returns false if the port cannot be opened
        bool listen(int port);

        // Whether the viewport should hand over its next frame: a browser is watching and the last frame is encoded
        bool wantsFrame() const;
        // A frame read from the viewport: RGB rows, bottom row first, as OpenGL returns them
        void sendFrame(std::vector<unsigned char> rgb, int width, int height);

    private:
        void onNewConnection();
        void onRequest(QTcpSocket* socket);
        void broadcastFrame(const QByteArray& jpeg);
        void sendLatestFrame(QTcpSocket* socket);
        void encodeFrames();
        QByteArray stateJson() const;

        QTcpServer server_;
        QList<QTcpSocket*> frameClients_;  // Browsers waiting for a frame newer than the latest
        QByteArray latestFrame_;
        quint64 latestFrameNumber_ = 0;
        std::chrono::steady_clock::time_point lastFrameTime_;
        std::chrono::steady_clock::time_point lastFrameRequestTime_;

        // Frames are encoded to JPEG on their own thread, one at a time, so the viewport does not wait for it
        std::thread encoder_;
        std::mutex mutex_;
        std::condition_variable frameReady_;
        std::vector<unsigned char> frame_;
        int frameWidth_ = 0;
        int frameHeight_ = 0;
        bool stopping_ = false;
        std::atomic<bool> frameInFlight_{false};
};

}  // namespace spqr
