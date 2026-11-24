#include "seat.h"

#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

}  // namespace

int main() {
    try {
        for (const int id : {1, 3, 4, 6, 101, 103, 104, 106})
            check(Seat::fromRobotId(id).has_value(), "every 2027 operator seat is legal");
        for (const int id : {0, 2, 5, 7, 8, 100, 102, 105, 107, 111, -3})
            check(!Seat::fromRobotId(id).has_value(), "no other id is a seat");

        const Seat red3 = *Seat::fromRobotId(3);
        check(red3.faction() == 1 && red3.role() == Role::Infantry, "red 3 is red infantry");
        check(red3.label() == QStringLiteral("红方 3 号 · 步兵"), "red 3 label");
        check(red3.mqttClientId() == QStringLiteral("3"), "red 3 logs in as 3");

        const Seat blue3 = *Seat::fromRobotId(103);
        check(blue3.faction() == 2 && blue3.label() == QStringLiteral("蓝方 3 号 · 步兵"),
              "blue 103 shows its local number");
        check(blue3.mqttClientId() == QStringLiteral("103"), "blue 3 logs in as 103");

        check(Seat::fromRobotId(1)->role() == Role::Hero && Seat::fromRobotId(101)->role() == Role::Hero,
              "1 and 101 are the hero seats");
        check(Seat::fromRobotId(4)->role() == Role::Infantry, "4 is infantry");
        check(Seat::fromRobotId(106)->role() == Role::Commander, "106 is the commander");
        check(Seat::fromRobotId(6)->label() == QStringLiteral("红方 6 号 · 指挥手"), "commander label");
        check(role_name(Role::Hero) == QStringLiteral("hero") &&
                  role_name(Role::Commander) == QStringLiteral("commander") &&
                  role_name(Role::Infantry) == QStringLiteral("infantry"),
              "role names for logs and layout dumps");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "seat model OK\n";
    return 0;
}
