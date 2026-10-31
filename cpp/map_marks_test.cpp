#include "map_marks.h"
#include "rm_terminal.pb.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

void pixels_and_metres_round_trip() {
    const FieldExtent field = default_field_extent();
    const MapViewport view = fit_viewport(field, 1120, 600);
    for (const auto& [x, y] : std::vector<std::pair<double, double>>{
             {0.0, 0.0}, {field.width_m, 0.0}, {0.0, field.height_m},
             {field.width_m, field.height_m}, {field.width_m / 2, field.height_m / 2}, {3.3, 11.7}}) {
        const Projected pixel = project(field, view, x, y);
        const auto back = unproject(field, view, pixel.point.x, pixel.point.y);
        check(back && std::abs(back->first - x) < 0.01 && std::abs(back->second - y) < 0.01,
              "pixel -> metres -> pixel stays within 1 cm");
    }
    check(!unproject(field, view, view.x - 5.0, view.y + 10.0), "a click left of the field is off-field");
    check(!unproject(field, view, view.x + 10.0, view.y + view.height + 3.0), "a click below the field is off-field");
}

void letters_follow_the_official_client() {
    for (char c : std::string("CDEFGHIJKLNOQRSTUVWXYZ")) check(valid_mark_letter(c), "allowed letter");
    for (char c : std::string("ABMP0a")) check(!valid_mark_letter(c), "letter the official client rejects");
    MarkDraft draft;
    draft.kind = MarkKind::Custom;
    draft.ascii = 'M';
    draft.x_m = 5;
    draft.y_m = 5;
    check(encode_map_click(draft, default_field_extent()).rejection == MarkRejection::BadLetter,
          "a custom mark with a forbidden letter is not sent");
}

void encoding_keeps_mode_and_type_apart() {
    MarkDraft draft;
    draft.kind = MarkKind::Defend;
    draft.target = MarkTarget::EnemyRobot;
    draft.enemy_id = 103;
    draft.scope = MarkScope::Specific;
    draft.recipients = {7, 0, 0, 0, 0, 0, 0};
    draft.x_m = 12.5;
    draft.y_m = 4.25;
    const MarkEncoding encoded = encode_map_click(draft, default_field_extent());
    check(encoded.rejection == MarkRejection::None, "a valid mark encodes");
    rm::MapClickCmd message;
    check(message.ParseFromString(encoded.bytes), "the bytes are a MapClickCmd");
    check(message.mode() == 2, "mode carries the mark kind (defend)");
    check(message.type() == 2, "type carries the mark target (enemy robot)");
    check(message.enemy_id() == 103 && message.is_send_all() == 0, "enemy id and specific scope");
    check(message.robot_id().size() == 7 && static_cast<unsigned char>(message.robot_id()[0]) == 7 &&
              message.robot_id()[1] == '\0',
          "recipients are a fixed 7-byte list padded with zero");
    check(std::abs(message.map_x() - 12.5f) < 1e-6 && std::abs(message.map_y() - 4.25f) < 1e-6,
          "coordinates in metres");

    draft.recipients = {};
    check(encode_map_click(draft, default_field_extent()).rejection == MarkRejection::NoRecipient,
          "a specific mark needs at least one recipient");
    draft.scope = MarkScope::IncludeSentry;
    draft.x_m = -1;
    check(encode_map_click(draft, default_field_extent()).rejection == MarkRejection::OffField,
          "an off-field point is not sent");
}

void marks_are_rate_limited() {
    MarkGate gate;
    check(gate.allow(1000), "the first mark goes");
    check(!gate.allow(1400), "a second mark within 0.5 s is blocked");
    check(gate.allow(1500), "after 0.5 s the next mark goes");
}

void marks_fade_and_expire() {
    std::vector<MapMark> received(1);
    received[0].mode = 1;
    received[0].x = 10;
    received[0].y = 5;
    received[0].received_at = 0;
    std::vector<std::pair<MarkDraft, MonotonicMs>> sent{{MarkDraft{}, 20000}};
    auto marks = visible_marks(received, sent, 10000, 30000);
    check(marks.size() == 1 && marks[0].opacity == 1.0, "a fresh mark is fully visible; a future one is not drawn");
    marks = visible_marks(received, sent, 27500, 30000);
    check(marks.size() == 2 && std::abs(marks[0].opacity - 0.5) < 1e-9, "the last 5 s fade out");
    check(marks[1].sent_by_us, "our own marks are flagged");
    marks = visible_marks(received, sent, 30000, 30000);
    check(marks.size() == 1 && marks[0].sent_by_us, "an expired mark disappears");
    check(mark_glyph(MarkKind::Custom, 'Q') == QStringLiteral("Q") &&
              mark_glyph(MarkKind::Attack, 0) == QStringLiteral("攻"),
          "glyphs");
}

}  // namespace

int main() {
    try {
        pixels_and_metres_round_trip();
        letters_follow_the_official_client();
        encoding_keeps_mode_and_type_apart();
        marks_are_rate_limited();
        marks_fade_and_expire();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "map marks OK\n";
    return 0;
}
