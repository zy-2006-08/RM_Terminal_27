#include "video_relay.h"
#include "video_receiver.h"
#include "video_wall.h"

#include <QApplication>
#include <QHostAddress>
#include <QImage>
#include <QMouseEvent>
#include <QUdpSocket>

#include <cstdio>
#include <vector>

using namespace rm_terminal;

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) {
        std::printf("PASS %s\n", label);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

bool sixteen_by_nine(const QRect& tile) { return std::abs(tile.width() * 9 - tile.height() * 16) <= 16; }

bool tiles_fit(const std::vector<QRect>& tiles, QSize area) {
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (!QRect(QPoint(), area).contains(tiles[i]) || !sixteen_by_nine(tiles[i])) return false;
        for (std::size_t j = i + 1; j < tiles.size(); ++j)
            if (tiles[i].intersects(tiles[j])) return false;
    }
    return true;
}

void the_wall_shows_our_hero_and_infantry() {
    check(wall_robots(6) == std::vector<std::uint32_t>({1, 3, 4}), "red commander watches 1, 3 and 4");
    check(wall_robots(106) == std::vector<std::uint32_t>({101, 103, 104}), "blue commander watches 101, 103 and 104");
    check(wall_label(1) == QStringLiteral("1 号 · 重装") && wall_label(104) == QStringLiteral("4 号 · 步兵"),
          "tiles are labelled with number and class");
    check(wall_port(3340, 1) == 3341 && wall_port(3340, 103) == 3343 && wall_port(4340, 104) == 4344,
          "each robot has its own wall port, the same for both sides");
}

void tiles_are_as_large_as_the_area_allows() {
    // 信息页里那一小块(扁长):3 路排成一行,不排成 2 × 2 空一格。
    const QSize strip(454, 160);
    const std::vector<QRect> row = wall_tiles(strip, 3, 6);
    check(row.size() == 3 && tiles_fit(row, strip), "three tiles fit the strip at 16:9 without overlap");
    check(row[0].y() == row[1].y() && row[1].y() == row[2].y(), "a short wide strip holds one row");
    check(row[0].width() >= 140, "the row uses the strip's width");

    // 图传页(1920 × 1080 下):2 × 2 比一行大得多,最后一行那一路居中。
    const QSize page(1904, 864);
    const std::vector<QRect> grid = wall_tiles(page, 3, 8);
    check(grid.size() == 3 && tiles_fit(grid, page), "three tiles fit the full page");
    check(grid[0].y() == grid[1].y() && grid[2].y() > grid[0].y(), "the full page holds two rows");
    check(grid[0].width() >= 700, "full page tiles are large");
    check(std::abs(grid[2].center().x() - page.width() / 2) <= 1, "the lone tile on the last row is centred");

    check(wall_tiles(page, 0, 8).empty() && wall_tiles(QSize(4, 4), 3, 8).empty(),
          "nothing to show or no room gives no tiles");
}

void a_tile_says_why_it_has_no_picture() {
    const WallFeed replay = wall_feed(3, nullptr);
    check(replay.frame.isNull() && replay.status.contains(QStringLiteral("录像")), "replays have no team video");
    check(replay.label == QStringLiteral("3 号 · 步兵"), "the feed carries its label");

    VideoReceiver idle;
    const WallFeed waiting = wall_feed(1, &idle);
    check(waiting.frame.isNull() && waiting.status == QStringLiteral("还没收到转发"),
          "a receiver that never got a packet says so");

    QUdpSocket squatter;
    check(squatter.bind(QHostAddress::LocalHost, 0), "a socket holds a port");
    VideoReceiver blocked;
    VideoEndpoint endpoint;
    endpoint.port = squatter.localPort();
    check(!blocked.start(endpoint), "the wall port is taken");
    check(wall_feed(4, &blocked).status.contains(QStringLiteral("端口")), "a taken port is named as the cause");
}

QImage solid(const QColor& colour) {
    QImage image(VideoReceiver::frameWidth(), VideoReceiver::frameHeight(), QImage::Format_RGB32);
    image.fill(colour);
    return image;
}

void the_wall_draws_pictures_and_reasons() {
    VideoWall wall(VideoWall::Mode::Compact);
    wall.resize(600, 200);
    wall.setFeeds({{1, wall_label(1), solid(Qt::red), QString()},
                   {3, wall_label(3), QImage(), QStringLiteral("还没收到转发")},
                   {4, wall_label(4), solid(Qt::green), QString()}});
    QImage shot(wall.size(), QImage::Format_ARGB32);
    shot.fill(Qt::transparent);
    wall.render(&shot);
    const QColor first = shot.pixelColor(wall.tileRect(0).center() + QPoint(0, 15));
    const QColor second = shot.pixelColor(wall.tileRect(1).center() + QPoint(0, 15));
    const QColor third = shot.pixelColor(wall.tileRect(2).center() + QPoint(0, 15));
    check(first.red() > 200 && first.green() < 60, "the first tile shows robot 1's picture");
    check(second.red() < 60 && second.green() < 60, "a tile without a picture stays dark");
    check(third.green() > 200 && third.red() < 60, "the third tile shows robot 4's picture");
    check(wall.tileRect(2).bottom() < wall.height() - 20, "the compact wall keeps a caption line");

    VideoWall full(VideoWall::Mode::Full);
    full.resize(1600, 800);
    full.setFeeds(wall.feeds());
    check(full.tileRect(0).width() > wall.tileRect(0).width() * 2, "the full wall's tiles are much bigger");

    int clicks = 0;
    QObject::connect(&wall, &VideoWall::clicked, [&clicks] { ++clicks; });
    const QPointF at(wall.tileRect(1).center());
    QMouseEvent press(QEvent::MouseButtonPress, at, wall.mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&wall, &press);
    QMouseEvent right(QEvent::MouseButtonPress, at, wall.mapToGlobal(at), Qt::RightButton, Qt::RightButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&wall, &right);
    check(clicks == 1, "a left click anywhere on the wall asks to switch pages, a right click does not");
}

void the_relay_forwards_packets_unchanged() {
    QUdpSocket commander;
    check(commander.bind(QHostAddress::LocalHost, 0), "the commander listens");
    VideoRelay relay(QHostAddress::LocalHost, commander.localPort());
    const std::vector<QByteArray> packets{QByteArray(9, 'a'), QByteArray(700, 'b'), QByteArray(1400, '\0')};
    for (const QByteArray& packet : packets) relay.forward(packet);
    std::vector<QByteArray> received;
    while (received.size() < packets.size() && commander.waitForReadyRead(1000))
        while (commander.hasPendingDatagrams()) {
            QByteArray bytes(static_cast<int>(commander.pendingDatagramSize()), Qt::Uninitialized);
            commander.readDatagram(bytes.data(), bytes.size());
            received.push_back(bytes);
        }
    check(received == packets, "every packet arrives byte for byte, in order");
    check(relay.sent() == 3 && relay.failed() == 0, "the relay counts what it sent");
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    the_wall_shows_our_hero_and_infantry();
    tiles_are_as_large_as_the_area_allows();
    a_tile_says_why_it_has_no_picture();
    the_wall_draws_pictures_and_reasons();
    the_relay_forwards_packets_unchanged();
    if (failures != 0) {
        std::fprintf(stderr, "video_wall_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("video_wall_test: all checks passed\n");
    return 0;
}
