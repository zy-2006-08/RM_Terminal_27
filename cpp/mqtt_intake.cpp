#include "mqtt_intake.h"
#include "clock.h"
#include "decoder.h"
#include <mosquitto.h>
#include <atomic>

namespace rm_terminal {
// `connected` 是 atomic:mosquitto 的回调跑在它自己的网络线程上,而 GUI 的定时
// tick 在主线程读它。加锁没必要,但普通 bool 的跨线程读写是数据竞争。
struct MqttIntake::Impl { MqttIntake* owner; Store& store; QString host; int port; mosquitto* client{}; Decoder decoder; std::atomic<bool> connected{false}; Impl(MqttIntake* o, Store& s, QString h, int p):owner(o),store(s),host(std::move(h)),port(p),decoder(s,RobotId{3}){} };
MqttIntake::MqttIntake(Store& s, QString h, int p, QObject* parent): QObject(parent), impl_(new Impl{this,s,std::move(h),p}) {}
MqttIntake::~MqttIntake() { if (impl_->client) { mosquitto_loop_stop(impl_->client, true); mosquitto_destroy(impl_->client); } delete impl_; mosquitto_lib_cleanup(); }
bool MqttIntake::connected() const { return impl_->connected.load(); }
static void on_connect(mosquitto* c, void* data, int rc) { auto* i=static_cast<MqttIntake::Impl*>(data); if(rc==0) { mosquitto_subscribe(c,nullptr,"GameStatus",0); mosquitto_subscribe(c,nullptr,"RobotDynamicStatus",0); mosquitto_subscribe(c,nullptr,"RobotModuleStatus",0); mosquitto_subscribe(c,nullptr,"RobotPosition",0); mosquitto_subscribe(c,nullptr,"Event",0); mosquitto_subscribe(c,nullptr,"RobotTelemetry",0); mosquitto_subscribe(c,nullptr,"BlindStatus",0); mosquitto_subscribe(c,nullptr,"RobotPositionSet",0); mosquitto_subscribe(c,nullptr,"RobotHealthSet",0);
  // 订阅成功之后才算连上:连上但还没订阅时一条消息都收不到,那段时间里报「已连接」
  // 会让战术提醒以为数据只是暂时没到,而不是根本没在听。
  i->connected.store(true); Q_EMIT i->owner->connectionChanged(true); Q_EMIT i->owner->diagnostic(QStringLiteral("MQTT subscribed (readonly)")); } else Q_EMIT i->owner->diagnostic(QStringLiteral("MQTT reconnect pending")); }
static void on_disconnect(mosquitto*, void* data, int rc) { auto* i=static_cast<MqttIntake::Impl*>(data);
  // 断线必须报出去。战术提醒据此暂停本局:看不见比赛时继续按最后已知的倒计时播报,
  // 会在错误的时刻喊出战术。
  const bool was=i->connected.exchange(false); if(was) Q_EMIT i->owner->connectionChanged(false);
  Q_EMIT i->owner->diagnostic(rc==0?QStringLiteral("MQTT disconnected"):QStringLiteral("MQTT connection lost")); }
static void on_message(mosquitto*, void* data, const mosquitto_message* m) { auto* i=static_cast<MqttIntake::Impl*>(data); const std::string topic=m->topic; const std::string bytes(static_cast<const char*>(m->payload),m->payloadlen); const bool ok=i->decoder.accept(topic,bytes,monotonic_now());
  if(!ok) Q_EMIT i->owner->diagnostic(QStringLiteral("ignored malformed, stale, invalid, or unknown MQTT message: ")+QString::fromStdString(topic)); }
bool MqttIntake::start(){mosquitto_lib_init();impl_->client=mosquitto_new("rm-terminal-readonly",true,impl_);if(!impl_->client)return false;mosquitto_connect_callback_set(impl_->client,on_connect);mosquitto_disconnect_callback_set(impl_->client,on_disconnect);mosquitto_message_callback_set(impl_->client,on_message);return mosquitto_connect_async(impl_->client,impl_->host.toUtf8().constData(),impl_->port,30)==MOSQ_ERR_SUCCESS&&mosquitto_loop_start(impl_->client)==MOSQ_ERR_SUCCESS;}
}
