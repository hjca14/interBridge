#include "mqtt_smoke_state.h"

#include <algorithm>

namespace interbridge {

DevMqttSmokeState::DevMqttSmokeState(uint32_t initialRetryMs, uint32_t maxRetryMs)
    : state_(DevSmokeState::WaitingForWifi), initialRetryMs_(initialRetryMs), maxRetryMs_(maxRetryMs),
      retryDelayMs_(initialRetryMs), retryAtMs_(0), actionIssued_(false) {}

bool DevMqttSmokeState::deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

void DevMqttSmokeState::enter(DevSmokeState state, uint32_t nowMs) {
    state_ = state;
    retryDelayMs_ = initialRetryMs_;
    retryAtMs_ = nowMs;
    actionIssued_ = false;
}

void DevMqttSmokeState::scheduleRetry(uint32_t nowMs) {
    retryAtMs_ = nowMs + retryDelayMs_;
    retryDelayMs_ = std::min(maxRetryMs_, retryDelayMs_ > maxRetryMs_ / 2 ? maxRetryMs_ : retryDelayMs_ * 2);
    actionIssued_ = false;
}

DevSmokeAction DevMqttSmokeState::update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected) {
    if (!wifiConnected) {
        if (state_ != DevSmokeState::WaitingForWifi) enter(DevSmokeState::WaitingForWifi, nowMs);
        if (!actionIssued_ && deadlineReached(nowMs, retryAtMs_)) {
            actionIssued_ = true;
            scheduleRetry(nowMs);
            return DevSmokeAction::ConnectWifi;
        }
        return DevSmokeAction::None;
    }

    if (state_ == DevSmokeState::WaitingForWifi) enter(DevSmokeState::WaitingForDns, nowMs);
    if (state_ == DevSmokeState::WaitingForDns) {
        if (!actionIssued_ && deadlineReached(nowMs, retryAtMs_)) {
            actionIssued_ = true;
            return DevSmokeAction::ResolveDns;
        }
        return DevSmokeAction::None;
    }
    if (state_ == DevSmokeState::WaitingForTime) {
        if (timeValid) enter(DevSmokeState::WaitingForMqtt, nowMs);
        else if (!actionIssued_ && deadlineReached(nowMs, retryAtMs_)) {
            actionIssued_ = true;
            scheduleRetry(nowMs);
            return DevSmokeAction::ConfigureTime;
        } else return DevSmokeAction::None;
    }
    if (state_ == DevSmokeState::WaitingForMqtt) {
        if (!timeValid) { enter(DevSmokeState::WaitingForTime, nowMs); return DevSmokeAction::None; }
        if (mqttConnected) { enter(DevSmokeState::Online, nowMs); return DevSmokeAction::None; }
        if (!actionIssued_ && deadlineReached(nowMs, retryAtMs_)) {
            actionIssued_ = true;
            return DevSmokeAction::ConnectMqtt;
        }
    } else if (state_ == DevSmokeState::Online && !mqttConnected) {
        enter(timeValid ? DevSmokeState::WaitingForMqtt : DevSmokeState::WaitingForTime, nowMs);
    }
    return DevSmokeAction::None;
}

void DevMqttSmokeState::dnsResult(uint32_t nowMs, bool success) {
    if (state_ != DevSmokeState::WaitingForDns) return;
    if (success) enter(DevSmokeState::WaitingForTime, nowMs);
    else scheduleRetry(nowMs);
}

void DevMqttSmokeState::mqttResult(uint32_t nowMs, bool success) {
    if (state_ != DevSmokeState::WaitingForMqtt) return;
    if (success) enter(DevSmokeState::Online, nowMs);
    else scheduleRetry(nowMs);
}

DevSmokeState DevMqttSmokeState::state() const { return state_; }
uint32_t DevMqttSmokeState::retryDelayMs() const { return retryDelayMs_; }

} // namespace interbridge
