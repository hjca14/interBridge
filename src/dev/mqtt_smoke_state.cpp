#include "mqtt_smoke_state.h"

#include <algorithm>

namespace interbridge {

DevMqttSmokeState::DevMqttSmokeState(uint32_t initialRetryMs, uint32_t maxRetryMs, uint32_t ntpAttemptTimeoutMs,
                                     uint32_t wifiRecoveryThreshold, uint32_t wifiRecoveryCooldownMs)
    : state_(DevSmokeState::WaitingForWifi), initialRetryMs_(initialRetryMs), maxRetryMs_(maxRetryMs),
      ntpAttemptTimeoutMs_(ntpAttemptTimeoutMs), retryDelayMs_(initialRetryMs), retryAtMs_(0), actionIssued_(false),
      wifiRecoveryThreshold_(wifiRecoveryThreshold), wifiRecoveryCooldownMs_(wifiRecoveryCooldownMs) {}

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

void DevMqttSmokeState::recordConnectivityFailure(uint32_t nowMs) {
    ++consecutiveConnectivityFailures_;
    if (consecutiveConnectivityFailures_ < wifiRecoveryThreshold_) return;
    // Reset immediately so a still-broken interface needs a full fresh run
    // of failures (not just one more) before the cooldown check below is
    // even consulted again.
    consecutiveConnectivityFailures_ = 0;
    if (wifiRecoveryCooldownActive_ && !deadlineReached(nowMs, wifiRecoveryCooldownUntilMs_)) {
        // Still cooling down from a recent recovery - never escalate again
        // before it elapses, no matter how many more failures accumulate.
        // The ordinary per-stage backoff (DNS/MQTT) keeps retrying on its
        // own in the meantime.
        return;
    }
    wifiRecoveryCooldownActive_ = true;
    wifiRecoveryCooldownUntilMs_ = nowMs + wifiRecoveryCooldownMs_;
    wifiRecoveryRequested_ = true;
}

DevSmokeAction DevMqttSmokeState::update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected) {
    if (wifiRecoveryRequested_) {
        // Preempts the normal per-stage cascade below: a stronger recovery
        // was authorized by recordConnectivityFailure(), so force a full
        // Wi-Fi re-association regardless of the current stage. The caller
        // must tear the transport down and call WiFi.disconnect() (without
        // erasing credentials) in response; the ordinary ConnectWifi/backoff
        // flow then brings the interface back up unchanged.
        wifiRecoveryRequested_ = false;
        enter(DevSmokeState::WaitingForWifi, nowMs);
        ntpAttemptInFlight_ = false;
        return DevSmokeAction::RecoverWifi;
    }

    if (!wifiConnected) {
        if (state_ != DevSmokeState::WaitingForWifi) {
            enter(DevSmokeState::WaitingForWifi, nowMs);
            // Wi-Fi loss invalidates any NTP attempt that was in flight too -
            // it cannot complete without the link, and DNS/time gating are
            // all re-established from scratch once Wi-Fi returns.
            ntpAttemptInFlight_ = false;
        }
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
        if (timeValid) {
            // The real completion signal: the caller observed its own
            // sync-complete callback and timeValid became true because of
            // it. Whatever attempt was in flight is now resolved.
            ntpAttemptInFlight_ = false;
            enter(DevSmokeState::WaitingForMqtt, nowMs);
        } else {
            if (ntpAttemptInFlight_ && deadlineReached(nowMs, ntpAttemptDeadlineMs_)) {
                // The in-flight attempt's own bounded timeout elapsed
                // without ever completing - stop treating it as in flight so
                // a fresh attempt can be considered below. This is the only
                // way an in-flight attempt is abandoned without a real
                // completion signal.
                ntpAttemptInFlight_ = false;
            }
            if (ntpAttemptInFlight_) {
                // Still within its own timeout - never reissue configTime()
                // while a previous attempt might still complete, regardless
                // of the ordinary backoff deadline. sntp_get_sync_status()
                // is deliberately not consulted here: it can stay
                // reset/idle for a while after configTime() is called, so it
                // cannot serve as the sole "is it safe to retry" signal.
                return DevSmokeAction::None;
            }
            if (!actionIssued_ && deadlineReached(nowMs, retryAtMs_)) {
                actionIssued_ = true;
                scheduleRetry(nowMs);
                ntpAttemptInFlight_ = true;
                ntpAttemptDeadlineMs_ = nowMs + ntpAttemptTimeoutMs_;
                return DevSmokeAction::ConfigureTime;
            }
            return DevSmokeAction::None;
        }
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
    if (success) {
        enter(DevSmokeState::WaitingForTime, nowMs);
    } else {
        scheduleRetry(nowMs);
        recordConnectivityFailure(nowMs);
    }
}

void DevMqttSmokeState::networkPreflightFailed(uint32_t nowMs) {
    if (state_ != DevSmokeState::WaitingForMqtt) return;
    // WaitingForTime's own "if (timeValid) ... enter(WaitingForMqtt)" guard
    // means a still-valid clock is never re-synchronized just because this
    // route happens to pass back through WaitingForTime on its way to
    // WaitingForMqtt again - NTP is only ever restarted if timeValid is
    // itself no longer true.
    enter(DevSmokeState::WaitingForDns, nowMs);
    recordConnectivityFailure(nowMs);
}

void DevMqttSmokeState::mqttResult(uint32_t nowMs, bool success) {
    if (state_ != DevSmokeState::WaitingForMqtt) return;
    if (success) {
        enter(DevSmokeState::Online, nowMs);
        // A full connect+subscribe is the strongest possible signal that
        // the interface/DNS/TLS path is healthy end to end.
        consecutiveConnectivityFailures_ = 0;
        wifiRecoveryCooldownActive_ = false;
        wifiRecoveryRequested_ = false;
    } else {
        scheduleRetry(nowMs);
        recordConnectivityFailure(nowMs);
    }
}

DevSmokeState DevMqttSmokeState::state() const { return state_; }
uint32_t DevMqttSmokeState::retryDelayMs() const { return retryDelayMs_; }
uint32_t DevMqttSmokeState::retryAtMs() const { return retryAtMs_; }
bool DevMqttSmokeState::ntpAttemptInFlight() const { return ntpAttemptInFlight_; }
uint32_t DevMqttSmokeState::consecutiveConnectivityFailures() const { return consecutiveConnectivityFailures_; }
bool DevMqttSmokeState::wifiRecoveryCooldownActive() const { return wifiRecoveryCooldownActive_; }
uint32_t DevMqttSmokeState::wifiRecoveryCooldownUntilMs() const { return wifiRecoveryCooldownUntilMs_; }

} // namespace interbridge
