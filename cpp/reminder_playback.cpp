#include "reminder_playback.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace rm_terminal {

std::string reminder_audio_key(const std::string& text, const std::string& voice, int version) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // 分隔符不可省:没有它时 ("ab","c") 和 ("a","bc") 会哈希到同一个文件,
    // 于是换声音后可能播出上一个声音的音频。
    hash.addData(QByteArray::number(version));
    hash.addData(QByteArrayLiteral("\x1f"));
    hash.addData(QByteArray::fromStdString(voice));
    hash.addData(QByteArrayLiteral("\x1f"));
    hash.addData(QByteArray::fromStdString(text));
    return (QString::fromLatin1(hash.result().toHex()) + QStringLiteral(".aiff")).toStdString();
}

void ReminderPlayback::enqueue(const ReminderItem& item, const std::string& path) {
    queue_.push_back({item.id, path, 1});
}

std::optional<PlayCommand> ReminderPlayback::tick(MonotonicMs now, bool allow) {
    if (active_ || !allow) return std::nullopt;

    // 第二遍优先于队列里的下一条:同一条的两遍不允许被别的提醒插进来。
    if (repeat_ && repeat_at_) {
        if (now < *repeat_at_) return std::nullopt;
        auto utterance = *repeat_;
        repeat_.reset();
        repeat_at_.reset();
        active_ = Active{utterance, next_token_++};
        return PlayCommand{utterance.id, utterance.path, active_->token, utterance.pass};
    }
    if (repeat_) return std::nullopt;
    if (queue_.empty()) return std::nullopt;

    auto utterance = queue_.front();
    queue_.pop_front();
    active_ = Active{utterance, next_token_++};
    return PlayCommand{utterance.id, utterance.path, active_->token, utterance.pass};
}

void ReminderPlayback::finished(std::uint64_t token, bool ok, MonotonicMs now) {
    // 代次不符 = 这是取消之前发出的播放的回调。丢弃它,否则一次取消后旧回调
    // 会给已经作废的内容排上第二遍。
    if (!active_ || active_->token != token) return;
    const auto utterance = active_->utterance;
    const bool repeat_allowed = active_->repeat_allowed;
    active_.reset();
    if (!ok || !repeat_allowed) return;
    if (utterance.pass >= kPlaysPerReminder) return;
    repeat_ = Utterance{utterance.id, utterance.path, utterance.pass + 1};
    repeat_at_ = now + kRepeatGapMs;
}

void ReminderPlayback::cancelPending() {
    queue_.clear();
    repeat_.reset();
    repeat_at_.reset();
    if (active_) active_->repeat_allowed = false;
}

void ReminderPlayback::cancelAll() {
    cancelPending();
    // 作废当前播放的令牌:随后回来的完成回调会因代次不符被丢弃。
    active_.reset();
}

}
