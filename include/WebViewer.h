#pragma once

#include <QByteArray>
#include <QImage>
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

// A web page that shows the simulation (--web [PORT]): the viewport, as the tiles that change from frame to frame, and
// the game state, so a browser on another computer can follow Circus without seeing its screen. Its one control starts
// the game over: Circus restarts from scratch, robots included, and the page picks it up again (POST /restart).
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
        // A browser watching the stream, and the latest frame it was sent
        struct StreamClient {
                QTcpSocket* socket;
                quint64 frame = 0;
                std::chrono::steady_clock::time_point lastWrite;
        };

        void onNewConnection();
        void onRequest(QTcpSocket* socket);
        void newFrame(const QImage& image, const std::vector<int>& changedTiles, const QByteArray& fullJpeg);
        QByteArray updateSince(quint64 frame);
        void processFrames();
        QByteArray stateJson() const;

        QTcpServer server_;
        QList<StreamClient> streamClients_;
        std::chrono::steady_clock::time_point lastFrameTime_;

        // The latest frame, its number, and for each of its tiles the number of the frame that last changed it
        QImage latestImage_;
        quint64 latestFrameNumber_ = 0;
        std::vector<quint64> tileChanged_;
        QByteArray latestFullJpeg_;  // The latest frame whole, once encoded

        // Frames are compared tile by tile on their own thread, one at a time, so the viewport does not wait for it
        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable frameReady_;
        std::vector<unsigned char> frame_;
        int frameWidth_ = 0;
        int frameHeight_ = 0;
        bool stopping_ = false;
        std::atomic<bool> frameInFlight_{false};
};

}  // namespace spqr
