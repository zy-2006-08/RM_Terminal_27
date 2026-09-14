#include "mqtt_intake.h"
#include "clock.h"
#include "decoder.h"
#include <mosquitto.h>

namespace rm_terminal {
struct MqttIntake::Impl { MqttIntake* owner; Store& store; QString host; int port; mosquitto* client{}; Decoder decoder; Impl(MqttIntake* o, Store& s, QString h, int p):owner(o),store(s),host(std::move(h)),port(p),decoder(s,RobotId{3}){} };
MqttIntake::MqttIntake(Store& s, QString h, int p, QObject* parent): QObject(parent), impl_(new Impl{this,s,std::move(h),p}) {}
MqttIntake::~MqttIntake() { if (impl_->client) { mosquitto_loop_stop(impl_->client, true); mosquitto_destroy(impl_->client); } delete impl_; mosquitto_lib_cleanup(); }
static void connected(mosquitto* c, void* data, int rc) { auto* i=static_cast<MqttIntake::Impl*>(data); if(rc==0) { mosquitto_subscribe(c,nullptr,"GameStatus",0); mosquitto_subscribe(c,nullptr,"RobotDynamicStatus",0); mosquitto_subscribe(c,nullptr,"RobotModuleStatus",0); mosquitto_subscribe(c,nullptr,"RobotPosition",0); mosquitto_subscribe(c,nullptr,"Event",0); mosquitto_subscribe(c,nullptr,"RobotTelemetry",0); mosquitto_subscribe(c,nullptr,"BlindStatus",0); mosquitto_subscribe(c,nullptr,"RobotPositionSet",0); Q_EMIT i->owner->diagnostic(QStringLiteral("MQTT subscribed (readonly)")); } else Q_EMIT i->owner->diagnostic(QStringLiteral("MQTT reconnect pending")); }
static void message(mosquitto*, void* data, const mosquitto_message* m) { auto* i=static_cast<MqttIntake::Impl*>(data); const std::string topic=m->topic; const std::string bytes(static_cast<const char*>(m->payload),m->payloadlen); const bool ok=i->decoder.accept(topic,bytes,monotonic_now());
  if(!ok) Q_EMIT i->owner->diagnostic(QStringLiteral("ignored malformed, stale, invalid, or unknown MQTT message: ")+QString::fromStdString(topic)); }
bool MqttIntake::start(){mosquitto_lib_init();impl_->client=mosquitto_new("rm-terminal-readonly",true,impl_);if(!impl_->client)return false;mosquitto_connect_callback_set(impl_->client,connected);mosquitto_message_callback_set(impl_->client,message);return mosquitto_connect_async(impl_->client,impl_->host.toUtf8().constData(),impl_->port,30)==MOSQ_ERR_SUCCESS&&mosquitto_loop_start(impl_->client)==MOSQ_ERR_SUCCESS;}
}
