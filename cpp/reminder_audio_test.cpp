#include "macos_speech_backend.h"
#include "reminder_playback.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

struct Outcome {
    bool fired = false;
    quint64 token = 0;
    bool ok = false;
    QString error;
};

// 等一个真实子进程的结束信号。有界等待,超时即失败:挂住的 say 必须表现为
// 失败而不是让测试永久阻塞。
// 信号必须在触发动作**之前**连接:失败路径(缺文件、无此声音)是同步发出的,
// 动作之后再连接会永远等不到那一次信号。
class Awaiter {
public:
    Awaiter(SpeechBackend* backend, bool playback) {
        auto signal =
            playback ? &SpeechBackend::playbackFinished : &SpeechBackend::synthesizeFinished;
        QObject::connect(backend, signal, &loop_,
                         [this](quint64 token, bool ok, const QString& error) {
                             out_ = {true, token, ok, error};
                             loop_.quit();
                         });
    }

    Outcome wait(int timeout_ms) {
        if (out_.fired) return out_;
        QTimer::singleShot(timeout_ms, &loop_, &QEventLoop::quit);
        loop_.exec();
        return out_;
    }

private:
    QEventLoop loop_;
    Outcome out_;
};

void real_synthesis(const QString& dir) {
    MacosSpeechBackend backend;
    if (!backend.available()) {
        std::cout << "SKIP no chinese voice on this machine: "
                  << backend.unavailableReason().toStdString() << '\n';
        check(!backend.unavailableReason().isEmpty(),
              "unavailable backend reports an actionable reason");
        return;
    }
    check(!backend.voice().isEmpty(), "a zh_CN voice was selected from the system");

    const auto path = QDir(dir).filePath(QStringLiteral("one.aiff"));
    Awaiter synth_wait(&backend, false);
    backend.synthesize(QStringLiteral("剩余一分钟，收缩防线"), path, 7);
    auto synth = synth_wait.wait(30000);
    check(synth.fired && synth.ok && synth.token == 7,
          "real say synthesises chinese text and reports the original token");
    check(QFileInfo(path).size() > 1000, "generated audio file is non-trivial");

    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "generated file is readable");
    const auto header = file.read(12);
    // AIFF-C 容器头。断言的是「afplay 能解的真实音频」,不是「文件非空」。
    check(header.startsWith(QByteArrayLiteral("FORM")) &&
          header.mid(8, 4) == QByteArrayLiteral("AIFC"),
          "generated file is a decodable AIFF-C container, not an empty placeholder");

    QElapsedTimer timer;
    Awaiter play_wait(&backend, true);
    timer.start();
    backend.play(path, 11);
    auto played = play_wait.wait(30000);
    check(played.fired && played.ok && played.token == 11,
          "real afplay plays the generated file to completion");
    check(timer.elapsed() > 200, "playback actually took audible time");

    // 停止必须立刻生效:关总开关时操作手期待马上安静。
    backend.play(path, 12);
    backend.stop();
    check(true, "stop() returns promptly after killing the player");

    Awaiter regen_wait(&backend, false);
    backend.synthesize(QStringLiteral("第二条战术"), path, 13);
    synth = regen_wait.wait(30000);
    check(synth.ok && synth.token == 13, "regenerating over an existing path succeeds");
}

void injection(const QString& dir) {
    MacosSpeechBackend backend;
    if (!backend.available()) return;
    // 文本作为独立 argv 传入,不经 shell。含 shell 元字符的战术只会被读出来,
    // 不会被执行 —— 这里用一个会创建文件的命令替换来证明它没有被求值。
    const auto canary = QDir(dir).filePath(QStringLiteral("canary.txt"));
    const auto payload = QStringLiteral("撤退 $(touch %1) `touch %1` ; rm -rf x").arg(canary);
    const auto path = QDir(dir).filePath(QStringLiteral("inject.aiff"));
    Awaiter waiter(&backend, false);
    backend.synthesize(payload, path, 21);
    const auto synth = waiter.wait(30000);
    check(synth.fired && synth.ok, "text with shell metacharacters synthesises as plain speech");
    check(!QFileInfo::exists(canary), "no command substitution executed: the canary was never created");
}

void missing_inputs(const QString& dir) {
    MacosSpeechBackend backend;
    if (!backend.available()) return;
    Awaiter waiter(&backend, true);
    backend.play(QDir(dir).filePath(QStringLiteral("absent.aiff")), 31);
    const auto played = waiter.wait(5000);
    check(played.fired && !played.ok && played.token == 31,
          "playing a missing file fails loudly instead of silently succeeding");
    check(!played.error.isEmpty(), "the failure names the missing file");
}

void unavailable_voice() {
    // 指定一个系统里不存在的声音:say 会以非零码退出,准备失败必须传播出来,
    // 而不是被当成 Ready。
    MacosSpeechBackend backend(nullptr, QStringLiteral("NoSuchVoice_zzz"));
    QTemporaryDir dir;
    const auto path = QDir(dir.path()).filePath(QStringLiteral("x.aiff"));
    Awaiter waiter(&backend, false);
    backend.synthesize(QStringLiteral("测试"), path, 41);
    const auto synth = waiter.wait(15000);
    check(synth.fired && !synth.ok, "a nonexistent voice reports synthesis failure");
    check(!synth.error.isEmpty(), "synthesis failure carries a diagnostic message");
}

// 默认声音必须挑 (Enhanced)。同一发音人的高音质版本,战术播报要在场地噪声里被听清;
// 系统里两个版本并存时 `say -v '?'` 的顺序不保证哪个在前,所以不能靠取第一项。
void default_voice_prefers_enhanced() {
    const auto voices = MacosSpeechBackend::chineseVoices();
    if (voices.isEmpty()) {
        std::cout << "SKIP no chinese voice on this machine\n";
        return;
    }
    const auto chosen = MacosSpeechBackend::preferredVoice(voices);
    check(voices.contains(chosen), "the chosen default is one of the enumerated system voices");

    bool has_enhanced = false;
    for (const auto& name : voices)
        if (name.contains(QStringLiteral("(Enhanced)"))) has_enhanced = true;
    if (has_enhanced)
        check(chosen.contains(QStringLiteral("(Enhanced)")),
              "when an enhanced voice exists it is chosen over the plain one, whatever the order");

    MacosSpeechBackend backend;
    check(backend.voice() == chosen,
          "the backend actually starts on that voice, not merely reports it as preferred");
}

// 裸名(`Eddy`)匹配的是**同名的英文声音**,不是中文的 `Eddy (中文（中国大陆）)`。
// say 对它不报错,而是静默用英文声音以 0 退出 —— 到点播出的是英文音节。
// 所以切换必须先核对完整名字在 zh_CN 枚举里。
void a_bare_voice_name_is_refused() {
    MacosSpeechBackend backend;
    if (!backend.available()) {
        std::cout << "SKIP no chinese voice on this machine\n";
        return;
    }
    const auto original = backend.voice();
    check(!backend.setVoiceAndRate(QStringLiteral("Eddy"), kDefaultReminderRateWpm),
          "a bare voice name is refused rather than silently falling back to an english voice");
    check(backend.voice() == original, "the refused switch left the working voice in place");

    check(!backend.setVoiceAndRate(QStringLiteral("NoSuchVoice_zzz"), kDefaultReminderRateWpm),
          "a voice that does not exist at all is refused too");
    check(backend.voice() == original, "and that refusal also preserves the working voice");

    // 枚举里的完整名字必须能切过去,否则这道校验就把可用的声音也挡掉了。
    const auto voices = MacosSpeechBackend::chineseVoices();
    for (const auto& name : voices) {
        if (name == original) continue;
        check(backend.setVoiceAndRate(name, 250),
              "a full zh_CN name from the enumeration is accepted");
        check(backend.voice() == name && backend.rateWpm() == 250,
              "the accepted switch really changed the voice and rate");
        break;
    }
}

void cache_reuse(const QString& dir) {
    MacosSpeechBackend backend;
    if (!backend.available()) return;
    const auto key = reminder_audio_key("缓存复用", backend.voice().toStdString(), 1);
    const auto path = QDir(dir).filePath(QString::fromStdString(key));
    Awaiter first_wait(&backend, false);
    backend.synthesize(QStringLiteral("缓存复用"), path, 51);
    check(first_wait.wait(30000).ok, "cache-keyed path synthesises");
    const auto first = QFileInfo(path).size();
    Awaiter second_wait(&backend, false);
    backend.synthesize(QStringLiteral("缓存复用"), path, 52);
    check(second_wait.wait(30000).ok, "regeneration to the same key succeeds");
    check(QFileInfo(path).size() == first, "same text and voice produce the same audio size");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        if (!dir.isValid()) throw std::runtime_error("cannot create temp dir");
        real_synthesis(dir.path());
        injection(dir.path());
        missing_inputs(dir.path());
        unavailable_voice();
        default_voice_prefers_enhanced();
        a_bare_voice_name_is_refused();
        cache_reuse(dir.path());
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "reminder_audio: all checks passed\n";
    return 0;
}
