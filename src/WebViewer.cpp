#include "WebViewer.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#ifdef __linux__
#include <linux/sockios.h>
#endif

#include <QBuffer>
#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <iostream>

#include "Constants.h"
#include "GameController.h"

namespace spqr {

namespace {

// About every other frame the viewport draws (every 20 ms or so), up to 28 a second, at its own size up to Full HD
constexpr std::chrono::milliseconds kFrameInterval(35);
constexpr int kMaxFrameWidth = 1920;
constexpr int kJpegQuality = 75;
// Only the tiles that changed are sent, packed in rows of one JPEG: a game changes a few percent of the picture per frame
constexpr int kTileSize = 64;  // A multiple of JPEG's 16 x 16 blocks, so tiles are encoded alone
constexpr int kAtlasColumns = 16;
// A stream that shows a still picture gets an empty update this often, so the page knows it is alive
constexpr std::chrono::seconds kKeepAlive(2);
// A stream takes the next update while less than this is still unsent on it: about one update in line, so the next one
// is not skipped just because the previous is still being paced out, yet the picture stays about an update behind at most
constexpr int kMaxBacklog = 64 * 1024;
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
  <button id="restart" title="Restart Circus and its robots from scratch, as if launched again">Restart game</button>
</header>
<main><canvas id="view"></canvas></main>
<script>
const $ = id => document.getElementById(id);
const clock = s => Math.floor(s / 60) + ':' + String(Math.floor(s % 60)).padStart(2, '0');
const words = s => s.toLowerCase().replaceAll('_', ' ');
const SUBPHASES = {KICKOFF: 'kick-off', KICKIN: 'kick-in', CORNERKICK: 'corner kick', GOALKICK: 'goal kick',
                   PENALTYKICK: 'penalty kick', PUSHINGFREEKICK: 'pushing free kick'};
const view = $('view');
const context = view.getContext('2d');
const sleep = ms => new Promise(done => setTimeout(done, ms));

// Index of the first empty line ("\r\n\r\n") in bytes, -1 if none
function headerEnd(bytes) {
  for (let i = 0; i + 3 < bytes.length; i++)
    if (bytes[i] === 13 && bytes[i + 1] === 10 && bytes[i + 2] === 13 && bytes[i + 3] === 10)
      return i;
  return -1;
}

// Draws an update: the whole frame, or tiles packed in rows of the image, listed by their index in the frame
function draw(image, tiles, size) {
  if (tiles === 'full') {
    if (view.width !== image.width || view.height !== image.height) {
      view.width = image.width;
      view.height = image.height;
    }
    context.drawImage(image, 0, 0);
  } else {
    const columns = image.width / size, perRow = Math.ceil(view.width / size);
    tiles.split(',').forEach((tile, k) => context.drawImage(image, (k % columns) * size, Math.floor(k / columns) * size, size, size,
                                                            (tile % perRow) * size, Math.floor(tile / perRow) * size, size, size));
  }
  image.close();
}

// The updates come on one long response, each as soon as the previous one has left Circus: the connection never waits
// for the page, and a slow one gets fewer updates, never late ones. If they stop, the page connects again.
async function frames() {
  let count = 0, since = performance.now(), streams = 0;
  while (true) {
    const stream = ++streams;  // Updates still on their way from a stream that was dropped are not drawn
    const abort = new AbortController();
    let watchdog = setTimeout(() => abort.abort(), 5000);
    let drawn = Promise.resolve();
    try {
      const response = await fetch('/stream', {cache: 'no-store', signal: abort.signal});
      const tileSize = Number(response.headers.get('X-Tile-Size'));
      const reader = response.body.getReader();
      let buffer = new Uint8Array(0), start = 0, length = -1, tiles = '';
      while (true) {
        const {value, done} = await reader.read();
        if (done)
          throw new Error('stream ended');
        const joined = new Uint8Array(buffer.length + value.length);
        joined.set(buffer);
        joined.set(value, buffer.length);
        buffer = joined;
        while (true) {
          // A part: "--frame", its type, "Content-Length: N" and "X-Tiles", an empty line, then the N bytes of the JPEG
          if (length < 0) {
            const end = headerEnd(buffer);
            if (end < 0)
              break;
            const header = new TextDecoder().decode(buffer.subarray(0, end));
            length = Number(/Content-Length: (\d+)/.exec(header)[1]);
            tiles = /X-Tiles: ([^\r\n]*)/.exec(header)[1];
            start = end + 4;
          }
          if (buffer.length < start + length)
            break;
          clearTimeout(watchdog);
          watchdog = setTimeout(() => abort.abort(), 5000);
          if (tiles !== 'none') {
            // Each update builds on the ones before: they are drawn in order, and a broken one starts the stream over
            const jpeg = new Blob([buffer.subarray(start, start + length)], {type: 'image/jpeg'}), update = tiles;
            drawn = drawn.then(() => createImageBitmap(jpeg))
                         .then(image => stream === streams ? draw(image, update, tileSize) : image.close())
                         .catch(() => abort.abort());
            count++;
          }
          buffer = buffer.slice(start + length);
          length = -1;
          if (performance.now() - since > 2000) {
            $('fps').textContent = (count * 1000 / (performance.now() - since)).toFixed(0) + ' fps';
            count = 0;
            since = performance.now();
          }
        }
      }
    } catch (e) {
      await sleep(1000);
    } finally {
      clearTimeout(watchdog);
      abort.abort();
    }
  }
}

// Sim time when a restart was asked: until the new game answers, with its clock started over, the page says so
let simTime = 0, restartedAt = null;

$('restart').onclick = () => {
  if (!confirm('Restart Circus and its robots from scratch? The page picks the new game up when it is ready.'))
    return;
  fetch('/restart', {method: 'POST'}).catch(() => {});
  restartedAt = simTime;
  $('status').textContent = 'Restarting Circus';
};

async function poll() {
  try {
    const s = await (await fetch('/state', {cache: 'no-store'})).json();
    simTime = s.simTime;
    if (restartedAt !== null && simTime < restartedAt)
      restartedAt = null;
    $('red').textContent = s.score[0];
    $('blue').textContent = s.score[1];
    $('phase').textContent = words(s.phase) + ('phaseTimeLeft' in s ? ' ' + clock(s.phaseTimeLeft) : '');
    $('subphase').textContent = s.subPhase ? `${SUBPHASES[s.subPhase] || words(s.subPhase)} ${s.subPhaseTeam} ${clock(s.subPhaseTimeLeft)}` : '';
    $('subphase').className = 'value' + (s.subPhase ? ' team ' + s.subPhaseTeam : '');
    $('game').textContent = clock(s.gameTimeLeft);
    $('sim').textContent = clock(s.simTime);
    $('penalized').textContent = s.penalized.length
      ? 'Penalized: ' + s.penalized.map(p => `${p.team} ${p.number} (${words(p.penalty)}, ${clock(p.timeLeft)})`).join(', ') : '';
    $('status').textContent = restartedAt !== null ? 'Restarting Circus' : '';
  } catch (e) {
    $('status').textContent = restartedAt !== null ? 'Restarting Circus' : 'Circus is not answering';
  }
  setTimeout(poll, 500);
}
poll();
frames();
</script>
</body>
</html>
)html";

QByteArray toJpeg(const QImage& image) {
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", kJpegQuality);
    return jpeg;
}

int tileColumns(const QImage& image) {
    return (image.width() + kTileSize - 1) / kTileSize;
}

int tileCount(const QImage& image) {
    return tileColumns(image) * ((image.height() + kTileSize - 1) / kTileSize);
}

bool tileDiffers(const QImage& a, const QImage& b, int tile) {
    const int x = (tile % tileColumns(a)) * kTileSize;
    const int y = (tile / tileColumns(a)) * kTileSize;
    const int bytes = (std::min(x + kTileSize, a.width()) - x) * a.depth() / 8;
    for (int row = y; row < std::min(y + kTileSize, a.height()); ++row)
        if (std::memcmp(a.constScanLine(row) + x * a.depth() / 8, b.constScanLine(row) + x * b.depth() / 8, bytes) != 0)
            return true;
    return false;
}

QByteArray streamPart(const QByteArray& tiles, const QByteArray& jpeg) {
    return "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(jpeg.size()) + "\r\nX-Tiles: " + tiles + "\r\n\r\n" + jpeg
           + "\r\n";
}

// Whether a stream can take another update now: one that would wait behind too much waits for the next, which then brings
// the tiles of both, so a slow link gets fewer updates, never late ones
bool hasRoom(QTcpSocket* socket) {
    qint64 unsent = socket->bytesToWrite();
#ifdef SIOCOUTQNSD
    int unsentByKernel = 0;
    if (ioctl(static_cast<int>(socket->socketDescriptor()), SIOCOUTQNSD, &unsentByKernel) == 0)
        unsent += unsentByKernel;
#endif
    return unsent < kMaxBacklog;
}

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
    if (worker_.joinable())
        worker_.join();

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
    worker_ = std::thread(&WebViewer::processFrames, this);

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
    return !streamClients_.isEmpty() && !frameInFlight_ && std::chrono::steady_clock::now() - lastFrameTime_ >= kFrameInterval;
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

void WebViewer::processFrames() {
    QImage previous;
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

        // The tiles that differ from the previous frame: all of them when the size changes
        std::vector<int> changed;
        for (int tile = 0; tile < tileCount(image); ++tile)
            if (previous.size() != image.size() || tileDiffers(image, previous, tile))
                changed.push_back(tile);
        previous = image;

        // Most of the picture changed (the camera moved): the browsers will get it whole, encoded here, off the GUI thread
        QByteArray fullJpeg;
        if (2 * static_cast<int>(changed.size()) > tileCount(image))
            fullJpeg = toJpeg(image);

        // The sockets belong to the GUI thread
        QMetaObject::invokeMethod(
            this,
            [this, image, changed, fullJpeg] {
                newFrame(image, changed, fullJpeg);
                frameInFlight_ = false;
            },
            Qt::QueuedConnection);
        lock.lock();
    }
}

void WebViewer::newFrame(const QImage& image, const std::vector<int>& changedTiles, const QByteArray& fullJpeg) {
    if (!changedTiles.empty()) {
        ++latestFrameNumber_;
        if (image.size() != latestImage_.size())
            tileChanged_.assign(tileCount(image), latestFrameNumber_);
        for (int tile : changedTiles)
            tileChanged_[tile] = latestFrameNumber_;
        latestImage_ = image;
        latestFullJpeg_ = fullJpeg;
    }

    const auto now = std::chrono::steady_clock::now();
    QHash<quint64, QByteArray> updates;  // Browsers in step get the same update
    for (StreamClient& client : streamClients_) {
        if (!hasRoom(client.socket))
            continue;
        if (client.frame < latestFrameNumber_) {
            if (!updates.contains(client.frame))
                updates.insert(client.frame, updateSince(client.frame));
            client.socket->write(updates.value(client.frame));
            client.frame = latestFrameNumber_;
            client.lastWrite = now;
        } else if (now - client.lastWrite >= kKeepAlive) {
            client.socket->write(streamPart("none", QByteArray()));
            client.lastWrite = now;
        }
    }
}

// What brings a browser from frame `frame` to the latest one: the tiles changed since, packed in rows of one JPEG, or the
// whole frame for a browser that has none yet or when most tiles changed
QByteArray WebViewer::updateSince(quint64 frame) {
    std::vector<int> tiles;
    for (int tile = 0; frame > 0 && tile < static_cast<int>(tileChanged_.size()); ++tile)
        if (tileChanged_[tile] > frame)
            tiles.push_back(tile);
    if (frame == 0 || 2 * tiles.size() > tileChanged_.size()) {
        if (latestFullJpeg_.isEmpty())
            latestFullJpeg_ = toJpeg(latestImage_);
        return streamPart("full", latestFullJpeg_);
    }

    const int columns = tileColumns(latestImage_);
    const int bytesPerPixel = latestImage_.depth() / 8;
    const int atlasColumns = std::min(static_cast<int>(tiles.size()), kAtlasColumns);
    const int atlasRows = (static_cast<int>(tiles.size()) + atlasColumns - 1) / atlasColumns;
    QImage atlas(atlasColumns * kTileSize, atlasRows * kTileSize, latestImage_.format());
    atlas.fill(Qt::black);
    QByteArray list;
    for (int k = 0; k < static_cast<int>(tiles.size()); ++k) {
        const int x = (tiles[k] % columns) * kTileSize;
        const int y = (tiles[k] / columns) * kTileSize;
        const int atlasX = (k % atlasColumns) * kTileSize;
        const int atlasY = (k / atlasColumns) * kTileSize;
        const int bytes = (std::min(x + kTileSize, latestImage_.width()) - x) * bytesPerPixel;
        for (int row = 0; row < std::min(kTileSize, latestImage_.height() - y); ++row)
            std::memcpy(atlas.scanLine(atlasY + row) + atlasX * bytesPerPixel, latestImage_.constScanLine(y + row) + x * bytesPerPixel, bytes);
        if (k > 0)
            list += ',';
        list += QByteArray::number(tiles[k]);
    }
    return streamPart(list, toJpeg(atlas));
}

void WebViewer::onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
#ifdef SO_MAX_PACING_RATE
        setsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_MAX_PACING_RATE, &kMaxPacingRate, sizeof(kMaxPacingRate));
#endif
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            streamClients_.removeIf([socket](const StreamClient& client) { return client.socket == socket; });
            socket->deleteLater();
        });
    }
}

void WebViewer::onRequest(QTcpSocket* socket) {
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

    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);  // One request per connection
    if (request.size() < 2) {
        reply(socket, "400 Bad Request", "text/plain", "Bad request\n");
        return;
    }
    if (method == "POST" && path == "/restart") {
        reply(socket, "200 OK", "text/plain", "Restarting Circus\n");
        // main() starts Circus again once it has shut down; the reply goes out first
        QTimer::singleShot(200, [] { QCoreApplication::exit(restartExitCode); });
    } else if (method != "GET") {
        reply(socket, "405 Method Not Allowed", "text/plain", "Only GET, and POST /restart\n");
    } else if (path == "/") {
        reply(socket, "200 OK", "text/html; charset=utf-8", kPage);
    } else if (path == "/state") {
        reply(socket, "200 OK", "application/json", stateJson());
    } else if (path == "/stream") {
        // Plain bytes for the page to split: Safari would split a multipart/x-mixed-replace response itself, out of the page's reach
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Content-Type-Options: nosniff\r\n"
                      "Cache-Control: no-store\r\nX-Tile-Size: "
                      + QByteArray::number(kTileSize) + "\r\n\r\n");
        StreamClient client{socket};
        if (!latestImage_.isNull()) {
            client.socket->write(updateSince(0));
            client.frame = latestFrameNumber_;
        }
        client.lastWrite = std::chrono::steady_clock::now();
        streamClients_.append(client);
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
