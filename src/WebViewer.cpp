#include "WebViewer.h"

#include <sys/socket.h>

#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QUrlQuery>
#include <algorithm>
#include <iostream>
#include <utility>

#include "GameController.h"

namespace spqr {

namespace {

// Up to 15 frames a second, at the viewport's own size up to Full HD
constexpr std::chrono::milliseconds kFrameInterval(66);
constexpr int kMaxFrameWidth = 1920;
constexpr int kJpegQuality = 85;
// The viewport keeps grabbing frames while a browser has asked for one this recently, so the next is ready when asked
constexpr std::chrono::seconds kWatchTimeout(2);
constexpr qint64 kMaxRequestSize = 8192;
// Sent as one burst, a frame overflows the small buffers of a long path (a VPN from another network), and TCP then
// slows down for the losses. Paced at this rate it goes through: 4 Mbit/s unpaced, 23 paced, on such a path.
constexpr unsigned int kMaxPacingRate = 30'000'000 / 8;  // bytes per second

const QByteArray kPage = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Circus</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; height: 100vh; display: flex; flex-direction: column; background: #1a1a1a; color: #fff; font: 15px system-ui, sans-serif; }
  header { display: flex; flex-wrap: wrap; align-items: center; gap: 6px 20px; padding: 8px 16px; background: #333; }
  .label { color: #888; }
  .value { font-variant-numeric: tabular-nums; }
  #phase { text-transform: capitalize; }
  .score { font-size: 20px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .team { padding: 1px 8px; border-radius: 3px; }
  .red { background: #993546; }
  .blue { background: #108296; }
  #status { color: #e0a040; }
  button { margin-left: auto; padding: 4px 12px; border: 1px solid #666; border-radius: 3px; background: #444; color: #fff; font: inherit; cursor: pointer; }
  button:hover { background: #555; }
  main { flex: 1; min-height: 0; display: flex; align-items: center; justify-content: center; }
  #view { max-width: 100%; max-height: 100%; }
</style>
</head>
<body>
<header>
  <span class="score"><span class="team red">Red</span> <span id="red">0</span> - <span id="blue">0</span> <span class="team blue">Blue</span></span>
  <span><span class="label">Phase</span> <span id="phase" class="value"></span> <span id="subphase" class="value"></span></span>
  <span><span class="label">Game</span> <span id="game" class="value"></span></span>
  <span><span class="label">Sim</span> <span id="sim" class="value"></span></span>
  <span><span class="label">Video</span> <span id="fps" class="value"></span></span>
  <span id="penalized"></span>
  <span id="status"></span>
  <button id="restart" title="Put the robots and the ball back at their start positions and start a new game">Reset robots</button>
</header>
<main><img id="view" alt=""></main>
<script>
const $ = id => document.getElementById(id);
const clock = s => Math.floor(s / 60) + ':' + String(Math.floor(s % 60)).padStart(2, '0');
const words = s => s.toLowerCase().replaceAll('_', ' ');
const SUBPHASES = {KICKOFF: 'kick-off', KICKIN: 'kick-in', CORNERKICK: 'corner kick', GOALKICK: 'goal kick',
                   PENALTYKICK: 'penalty kick', PUSHINGFREEKICK: 'pushing free kick'};
const view = $('view');
const sleep = ms => new Promise(done => setTimeout(done, ms));

// Each frame is asked for once the previous one is on screen: a slow connection gets fewer frames, never late ones
async function frames() {
  let shown = null, frame = 0, count = 0, since = performance.now();
  while (true) {
    try {
      const response = await fetch('/frame?after=' + frame, {cache: 'no-store', signal: AbortSignal.timeout(5000)});
      if (!response.ok)
        throw new Error(response.status);
      frame = response.headers.get('X-Frame');
      const url = URL.createObjectURL(await response.blob());
      await new Promise(done => { view.onload = view.onerror = done; view.src = url; });
      if (shown)
        URL.revokeObjectURL(shown);
      shown = url;
      count++;
      if (performance.now() - since > 2000) {
        $('fps').textContent = (count * 1000 / (performance.now() - since)).toFixed(0) + ' fps';
        count = 0;
        since = performance.now();
      }
    } catch (e) {
      await sleep(1000);
    }
  }
}

$('restart').onclick = () => {
  if (confirm('Put the robots and the ball back at their start positions and start a new game?'))
    fetch('/restart', {method: 'POST'}).catch(() => {});
};

async function poll() {
  try {
    const s = await (await fetch('/state', {cache: 'no-store'})).json();
    $('red').textContent = s.score[0];
    $('blue').textContent = s.score[1];
    $('phase').textContent = words(s.phase) + ('phaseTimeLeft' in s ? ' ' + clock(s.phaseTimeLeft) : '');
    $('subphase').textContent = s.subPhase ? `${SUBPHASES[s.subPhase] || words(s.subPhase)} ${s.subPhaseTeam} ${clock(s.subPhaseTimeLeft)}` : '';
    $('subphase').className = 'value' + (s.subPhase ? ' team ' + s.subPhaseTeam : '');
    $('game').textContent = clock(s.gameTimeLeft);
    $('sim').textContent = clock(s.simTime);
    $('penalized').textContent = s.penalized.length
      ? 'Penalized: ' + s.penalized.map(p => `${p.team} ${p.number} (${words(p.penalty)}, ${clock(p.timeLeft)})`).join(', ') : '';
    $('status').textContent = '';
  } catch (e) {
    $('status').textContent = 'Circus is not answering';
  }
  setTimeout(poll, 500);
}
poll();
frames();
</script>
</body>
</html>
)html";

void reply(QTcpSocket* socket, const QByteArray& status, const QByteArray& contentType, const QByteArray& body) {
    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + contentType + "\r\nContent-Length: " + QByteArray::number(body.size())
                  + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();  // Once the reply is written
}

}  // namespace

WebViewer::WebViewer(QObject* parent) : QObject(parent) {
    connect(&server_, &QTcpServer::newConnection, this, &WebViewer::onNewConnection);
}

WebViewer::~WebViewer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    frameReady_.notify_one();
    if (encoder_.joinable())
        encoder_.join();

    // The sockets go with the server: they must not call back into this half-destroyed object
    for (QTcpSocket* socket : server_.findChildren<QTcpSocket*>())
        socket->disconnect(this);
}

bool WebViewer::listen(int port) {
    if (port < 1 || port > 65535) {
        std::cerr << "Web viewer: " << port << " is not a port" << std::endl;
        return false;
    }
    if (!server_.listen(QHostAddress::Any, static_cast<quint16>(port))) {
        std::cerr << "Web viewer: cannot listen on port " << port << ": " << server_.errorString().toStdString() << std::endl;
        return false;
    }
    encoder_ = std::thread(&WebViewer::encodeFrames, this);

    std::cout << "Web viewer: open http://localhost:" << port << " here";
    for (const QNetworkInterface& interface : QNetworkInterface::allInterfaces()) {
        // Skip the loopback and Docker's bridges: nobody browses from there
        const QString name = interface.name();
        if (!(interface.flags() & QNetworkInterface::IsUp) || (interface.flags() & QNetworkInterface::IsLoopBack) || name.startsWith("docker")
            || name.startsWith("br-") || name.startsWith("veth"))
            continue;
        for (const QNetworkAddressEntry& entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                std::cout << ", http://" << entry.ip().toString().toStdString() << ":" << port << " from another computer";
    }
    std::cout << std::endl;
    return true;
}

bool WebViewer::wantsFrame() const {
    const auto now = std::chrono::steady_clock::now();
    return now - lastFrameRequestTime_ < kWatchTimeout && !frameInFlight_ && now - lastFrameTime_ >= kFrameInterval;
}

void WebViewer::sendFrame(std::vector<unsigned char> rgb, int width, int height) {
    lastFrameTime_ = std::chrono::steady_clock::now();
    frameInFlight_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = std::move(rgb);
        frameWidth_ = width;
        frameHeight_ = height;
    }
    frameReady_.notify_one();
}

void WebViewer::encodeFrames() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        frameReady_.wait(lock, [this] { return stopping_ || !frame_.empty(); });
        if (stopping_)
            return;
        const std::vector<unsigned char> rgb = std::move(frame_);
        frame_.clear();
        const int width = frameWidth_;
        const int height = frameHeight_;
        lock.unlock();

        // OpenGL returns the bottom row first
        QImage image = QImage(rgb.data(), width, height, 3 * width, QImage::Format_RGB888).flipped(Qt::Vertical);
        if (image.width() > kMaxFrameWidth)
            image = image.scaledToWidth(kMaxFrameWidth, Qt::SmoothTransformation);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG", kJpegQuality);

        // The sockets belong to the GUI thread
        QMetaObject::invokeMethod(
            this,
            [this, jpeg] {
                broadcastFrame(jpeg);
                frameInFlight_ = false;
            },
            Qt::QueuedConnection);
        lock.lock();
    }
}

void WebViewer::broadcastFrame(const QByteArray& jpeg) {
    if (jpeg.isEmpty())
        return;
    latestFrame_ = jpeg;
    ++latestFrameNumber_;
    for (QTcpSocket* socket : std::exchange(frameClients_, {}))
        sendLatestFrame(socket);
}

// Frames go on a connection the browser keeps open (HTTP/1.1 keep-alive): no new connection, and no TCP slow start, per frame
void WebViewer::sendLatestFrame(QTcpSocket* socket) {
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(latestFrame_.size())
                  + "\r\nCache-Control: no-store\r\nX-Frame: " + QByteArray::number(latestFrameNumber_) + "\r\n\r\n" + latestFrame_);
}

void WebViewer::onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
#ifdef SO_MAX_PACING_RATE
        setsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_MAX_PACING_RATE, &kMaxPacingRate, sizeof(kMaxPacingRate));
#endif
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            frameClients_.removeOne(socket);
            socket->deleteLater();
        });
    }
}

void WebViewer::onRequest(QTcpSocket* socket) {
    if (frameClients_.contains(socket))
        return;  // Still waiting for its frame

    // A request ends with an empty line, and the ones the page sends carry no body: only the first line matters
    const qint64 end = socket->peek(kMaxRequestSize).indexOf("\r\n\r\n");
    if (end < 0) {
        if (socket->bytesAvailable() > kMaxRequestSize)
            socket->abort();
        return;
    }
    const QByteArray head = socket->read(end + 4);
    const QList<QByteArray> request = head.left(head.indexOf("\r\n")).split(' ');
    const QList<QByteArray> target = request.size() >= 2 ? request[1].split('?') : QList<QByteArray>();
    const QByteArray method = request[0];
    const QByteArray path = target.isEmpty() ? QByteArray() : target[0];

    // The page says which frame it shows: it gets the latest one if that is newer, the next one otherwise
    if (method == "GET" && path == "/frame") {
        lastFrameRequestTime_ = std::chrono::steady_clock::now();
        const QString shown = QUrlQuery(QString::fromLatin1(target.value(1))).queryItemValue("after");
        if (!latestFrame_.isEmpty() && shown != QString::number(latestFrameNumber_))
            sendLatestFrame(socket);
        else
            frameClients_.append(socket);
        return;
    }

    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);  // The other replies close the connection
    if (request.size() < 2) {
        reply(socket, "400 Bad Request", "text/plain", "Bad request\n");
        return;
    }
    if (method == "POST" && path == "/restart") {
        GameController::instance().requestRestartFromStart();
        reply(socket, "200 OK", "text/plain", "Restarting from the start positions\n");
    } else if (method != "GET") {
        reply(socket, "405 Method Not Allowed", "text/plain", "Only GET, and POST /restart\n");
    } else if (path == "/") {
        reply(socket, "200 OK", "text/html; charset=utf-8", kPage);
    } else if (path == "/state") {
        reply(socket, "200 OK", "application/json", stateJson());
    } else {
        reply(socket, "404 Not Found", "text/plain", "Not found\n");
    }
}

// What the GUI's header shows, read the same way
QByteArray WebViewer::stateJson() const {
    const GameController& gc = GameController::instance();
    QJsonObject state;
    state["simTime"] = gc.getSimTime();
    state["gameTimeLeft"] = std::max(0.0, gc.getGameDuration() - gc.getGameElapsedTime());

    const GamePhase phase = gc.getCurrentPhase();
    state["phase"] = QString::fromStdString(gamePhaseToString(phase));
    double phaseDuration = -1;
    if (phase == INITIAL)
        phaseDuration = gc.getInitialPhaseDuration();
    else if (phase == READY)
        phaseDuration = gc.getReadyPhaseDuration();
    else if (phase == SET)
        phaseDuration = gc.getSetPhaseDuration();
    if (phaseDuration >= 0)
        state["phaseTimeLeft"] = std::max(0.0, phaseDuration - gc.getCurrentPhaseElapsedTime());

    const GameSubPhase subPhase = gc.getCurrentSubPhase();
    if (subPhase != BALLFREE) {
        const double subPhaseDuration = (subPhase == KICKOFF) ? gc.getKickOffSubPhaseDuration() : gc.getSubPhaseDuration();
        state["subPhase"] = QString::fromStdString(gameSubPhaseToString(subPhase));
        state["subPhaseTeam"] = QString::fromStdString(subPhase == KICKOFF ? gc.getKickOffTeam() : gc.getCurrentSubPhaseTeam());
        state["subPhaseTimeLeft"] = std::max(0.0, subPhaseDuration - gc.getCurrentSubPhaseElapsedTime());
    }

    const auto [redScore, blueScore] = gc.getScore();
    state["score"] = QJsonArray{redScore, blueScore};

    QJsonArray penalized;
    for (const TeamInGame& team : gc.getTeamsInGame()) {
        for (const RobotInGame& robot : team.getRobotsInGame()) {
            if (!robot.getIsPenalized())
                continue;
            const double elapsed = robot.getPenalizationElapsedTime(static_cast<int>(gc.getGameElapsedTime()));
            penalized.append(QJsonObject{{"team", QString::fromStdString(team.getTeam()->name)},
                                         {"number", robot.getRobot()->number},
                                         {"penalty", QString::fromStdString(penaltyToString(robot.getPenalty()))},
                                         {"timeLeft", std::max(0.0, gc.getPenaltyDuration() - elapsed)}});
        }
    }
    state["penalized"] = penalized;

    return QJsonDocument(state).toJson(QJsonDocument::Compact);
}

}  // namespace spqr
