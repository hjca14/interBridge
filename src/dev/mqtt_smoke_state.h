#pragma once

#include <cstdint>

namespace interbridge {

enum class DevSmokeState { WaitingForWifi, WaitingForDns, WaitingForTime, WaitingForMqtt, Online };
enum class DevSmokeAction { None, ConnectWifi, ResolveDns, ConfigureTime, ConnectMqtt, RecoverWifi };

// Hardware-independent coordinator. Deadlines use signed subtraction so they
// remain correct when the 32-bit Arduino millis() counter wraps.
class DevMqttSmokeState {
public:
    // ntpAttemptTimeoutMs: bounded, configurable ceiling on how long a single
    // ConfigureTime/SNTP attempt is trusted to still be "in flight" - see
    // update() and ntpAttemptInFlight().
    // wifiRecoveryThreshold: consecutive DNS/TLS connectivity failures (with
    // Wi-Fi already associated) before a single Wi-Fi interface recovery
    // (RecoverWifi) is authorized - see networkPreflightFailed()/
    // mqttResult() and consecutiveConnectivityFailures(). Deliberately
    // conservative: a real-hardware run went ~110 minutes online before a
    // transient DNS/TLS outage occurred, so this must not fire on a single
    // blip.
    // wifiRecoveryCooldownMs: minimum time between two Wi-Fi recoveries, so
    // a long-running local network/ISP outage does not keep bouncing the
    // radio - see wifiRecoveryCooldownActive()/wifiRecoveryCooldownUntilMs().
    // wifiAssociationTimeoutMs: bounded, configurable ceiling on how long a
    // single ConnectWifi/WiFi.begin() attempt is trusted to still be
    // "in flight" before it is abandoned even without an explicit
    // wifiAssociationResult() call - see update() and
    // wifiAttemptInFlight(). Deliberately separate from
    // initialRetryMs/maxRetryMs (the backoff *between* attempts): this is
    // the ceiling on a single attempt's own duration.
    DevMqttSmokeState(uint32_t initialRetryMs = 1000, uint32_t maxRetryMs = 300000,
                      uint32_t ntpAttemptTimeoutMs = 15000,
                      uint32_t wifiRecoveryThreshold = 3,
                      uint32_t wifiRecoveryCooldownMs = 600000,
                      uint32_t wifiAssociationTimeoutMs = 15000);

    // Do not gate this on sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS
    // from the caller: on real hardware that status can stay reset/idle for a
    // while after configTime() is called, so a caller-supplied "in progress"
    // flag is not reliable as the sole guard. Instead, once a ConfigureTime
    // action is issued, this class tracks that attempt as in flight itself
    // (see ntpAttemptInFlight()) until either timeValid becomes true (the
    // real completion signal, via the caller observing its own sync-complete
    // callback) or ntpAttemptTimeoutMs elapses without that happening -
    // whichever comes first. While in flight, ConfigureTime is never
    // reissued, even past the ordinary backoff deadline. On timeout, the
    // attempt is treated as failed and exactly one fresh retry is allowed
    // once the ordinary backoff deadline is also reached.
    //
    // May return RecoverWifi instead of the state's ordinary next action if
    // enough consecutive connectivity failures have accumulated - see
    // networkPreflightFailed()/mqttResult(). The caller must tear its
    // transport down and call WiFi.disconnect() without erasing credentials
    // in response.
    //
    // WiFi.disconnect() is asynchronous: the caller's very next wifiConnected
    // read can still report true for a tick or more after RecoverWifi was
    // issued. After RecoverWifi, this class waits for wifiConnected to
    // actually report false before letting the ordinary
    // ConnectWifi/ResolveDns/.../ConnectMqtt cascade proceed - while still
    // (falsely) connected, update() returns None and the state does not
    // advance past WaitingForWifi, so DNS/MQTT can never be reattempted over
    // the stale association and ConnectWifi (WiFi.begin()) is guaranteed to
    // actually fire once the disconnect takes effect. See
    // awaitingWifiRecoveryDisconnect().
    //
    // Wi-Fi association itself (ConnectWifi/WiFi.begin()) is asynchronous
    // on real hardware, exactly like NTP/SNTP - the ESP32 Wi-Fi driver
    // actively rejects (and effectively restarts) a WiFi.begin() call
    // issued while a previous association attempt is still outstanding
    // ("wifi:sta is connecting, return error" / "WiFiSTA.cpp begin():
    // connect failed!" on real hardware), which a naive "just retry once
    // the ordinary backoff elapses, regardless of wifiConnected" policy
    // can trigger before the first attempt ever had a chance to resolve.
    // Once ConnectWifi is issued, this class tracks that attempt as in
    // flight itself (see wifiAttemptInFlight()) and never reissues it
    // until exactly one of three things happens: wifiConnected becomes
    // true (the ordinary success path, unchanged), the caller reports an
    // explicit outcome via wifiAssociationResult() (forwarded from a real
    // ARDUINO_EVENT_WIFI_STA_CONNECTED/GOT_IP/DISCONNECTED event), or
    // wifiAssociationTimeoutMs elapses without either happening. A
    // reported/observed failure or a timeout schedules the next retry
    // with the ordinary backoff at that point - never at the moment
    // ConnectWifi was issued, so a fast-failing attempt does not consume
    // backoff time it was never actually given.
    DevSmokeAction update(uint32_t nowMs, bool wifiConnected, bool timeValid, bool mqttConnected);
    // Re-arms the in-flight attempt's own association timeout deadline
    // from nowMs - call this immediately alongside the caller's actual
    // WiFi.begin(), after any pre-association work that might take real
    // time (e.g. a diagnostic Wi-Fi scan performed between the
    // ConnectWifi action and the real WiFi.begin() call). Without this,
    // update() itself provisionally arms the deadline from the moment
    // ConnectWifi was issued (see update()'s implementation) - a blocking
    // scan performed after that would otherwise silently consume part of
    // the association timeout before association even started. A no-op
    // if no attempt is currently in flight (mirrors
    // wifiAssociationResult()'s guard) - safe to call unconditionally
    // right before every WiFi.begin().
    void wifiAssociationStarted(uint32_t nowMs);
    // Reports an explicit, caller-observed Wi-Fi association outcome for
    // the attempt currently in flight (see wifiAttemptInFlight()) -
    // forwarded from a real Wi-Fi event, never invoked directly from
    // inside that event callback itself (the callback may run in a
    // different task/context on real hardware; the caller must only
    // record a minimal signal there and forward it into this method from
    // its own single-threaded main loop).
    //
    // success=true must be forwarded ONLY from ARDUINO_EVENT_WIFI_STA_GOT_IP
    // - never from ARDUINO_EVENT_WIFI_STA_CONNECTED (L2 association only,
    // before DHCP). A real-hardware run showed the two being conflated:
    // the caller forwarded plain L2-connect as "success" too, which -
    // combined with a subsequent drop before DHCP ever completed - could
    // permanently strand the machine in WaitingForWifi with no further
    // ConnectWifi ever issued again (wifiAttemptInFlight() already false,
    // but wifiConnected/WL_CONNECTED never became true either), requiring
    // a manual reboot to recover. This method no longer trusts success=true
    // as the final word by itself: it starts a bounded "awaiting confirmed
    // connect" window (see wifiConnectConfirmationPending()) instead of
    // immediately going idle, so even a caller that still (incorrectly)
    // forwards L2-connect, or a GOT_IP that is itself followed by an
    // unreported drop, can never wedge this state machine - see update()'s
    // handling of that window for the guaranteed recovery path.
    //
    // success=false ends the attempt and immediately schedules the next
    // Wi-Fi retry (see retryDelayMs()/retryAtMs() below - while
    // state()==WaitingForWifi they report this dedicated Wi-Fi retry
    // ladder, not the shared per-stage one DNS/Time/MQTT use), rather
    // than waiting for the full wifiAssociationTimeoutMs to elapse. A call
    // with no attempt
    // currently in flight (state_ is not WaitingForWifi, or no
    // ConnectWifi has been issued yet for this attempt) is a no-op - safe
    // to call from an event handler that may fire outside any pending
    // attempt (e.g. a late disconnect event after Online, which the
    // ordinary wifiConnected-driven "!wifiConnected -> WaitingForWifi"
    // path in update() already handles on its own).
    void wifiAssociationResult(uint32_t nowMs, bool success);
    // True from a success signal (see wifiAssociationResult()) until
    // either wifiConnected genuinely becomes true (the real completion -
    // ordinary cascade proceeds) or this window's own bounded timeout
    // elapses without that happening, at which point it is treated as a
    // failed attempt and the ordinary Wi-Fi retry backoff resumes. This is
    // the safety net described on wifiAssociationResult() above.
    bool wifiConnectConfirmationPending() const;
    // Explicit DNS resolution during the initial WaitingForDns bootstrap
    // stage (before NTP). success=false counts as a connectivity failure -
    // see consecutiveConnectivityFailures().
    void dnsResult(uint32_t nowMs, bool success);
    // Reports that the caller's own DNS preflight check - performed
    // immediately before attempting the actual TLS/socket MQTT connect, on
    // every ConnectMqtt action - failed. The caller must not attempt
    // transport.connect() when this is called. Transitions back to
    // WaitingForDns (that stage's own backoff, not the MQTT connect
    // backoff) and counts as a connectivity failure. Never call this during
    // the initial WaitingForDns bootstrap stage - use dnsResult() there.
    void networkPreflightFailed(uint32_t nowMs);
    // Reports the outcome of the actual TLS/socket MQTT connect (+
    // subscribe) attempt - only ever called after a successful preflight.
    // success=true (a full connect+subscribe) clears the connectivity
    // failure counter and any active Wi-Fi recovery cooldown, and is also
    // the ONLY thing that resets the dedicated Wi-Fi reconnection backoff
    // (see retryDelayMs()) back to its floor - a full end-to-end success
    // is the strongest available signal that the interface is genuinely
    // stable, not merely a momentary association that may drop again
    // immediately. success=false counts as a connectivity failure.
    void mqttResult(uint32_t nowMs, bool success);
    DevSmokeState state() const;
    // While state()==WaitingForWifi, these report the dedicated Wi-Fi
    // reconnection backoff (grows on every Wi-Fi-level failure/timeout;
    // does NOT reset merely because the interface briefly reassociated -
    // see mqttResult()'s doc comment for the only thing that resets it).
    // In every other state, they report the ordinary shared per-stage
    // backoff DNS/Time/MQTT retries use (reset to initialRetryMs on every
    // clean stage transition, exactly as before this class's call-session
    // recovery-hardening pass).
    uint32_t retryDelayMs() const;
    uint32_t retryAtMs() const;
    // Whether a ConfigureTime action's SNTP attempt is currently considered
    // in flight (issued, not yet resolved by timeValid or its own timeout).
    bool ntpAttemptInFlight() const;
    // Whether a ConnectWifi action's association attempt is currently
    // considered in flight (issued, not yet resolved by wifiConnected,
    // wifiAssociationResult(), or its own timeout) - see update().
    bool wifiAttemptInFlight() const;
    // Consecutive DNS-preflight/bootstrap or TLS/socket connectivity
    // failures since the last full MQTT connect+subscribe success. Publish
    // failures are never counted here - RemoteCommandProcessor/
    // Esp32AwsIotTransport already have their own outbox/session-
    // invalidation flow for those, independent of this interface-level
    // recovery ladder.
    uint32_t consecutiveConnectivityFailures() const;
    bool wifiRecoveryCooldownActive() const;
    uint32_t wifiRecoveryCooldownUntilMs() const;
    // True from the moment RecoverWifi is issued until update() actually
    // observes wifiConnected==false - see update()'s contract above.
    bool awaitingWifiRecoveryDisconnect() const;

    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs);

    // Wrap-safe "how many ms remain until deadlineMs" for DIAGNOSTIC
    // LOGGING ONLY (e.g. the callers' own "delay_ms=..." lines) - never
    // used for any retry/backoff decision, which remains solely
    // deadlineReached()'s job. Saturates at 0 once the deadline has
    // already been reached, instead of underflowing to a huge number via
    // plain unsigned subtraction (deadlineMs - nowMs) when the deadline is
    // even a few ms in the past - real hardware showed this exact bug as
    // `delay_ms=4294967291` once ConnectWifi started being issued right
    // at (or fractionally after) its own backoff deadline, which is
    // always true by construction the moment that action fires. Same
    // signed-subtraction wraparound technique as deadlineReached(), so it
    // shares that function's ~2^31 "already close on the 32-bit circle"
    // caveat.
    static uint32_t millisUntil(uint32_t deadlineMs, uint32_t nowMs);

private:
    void enter(DevSmokeState state, uint32_t nowMs);
    void scheduleRetry(uint32_t nowMs);
    void scheduleWifiRetry(uint32_t nowMs);
    void recordConnectivityFailure(uint32_t nowMs);

    DevSmokeState state_;
    uint32_t initialRetryMs_;
    uint32_t maxRetryMs_;
    uint32_t ntpAttemptTimeoutMs_;
    uint32_t retryDelayMs_;
    uint32_t retryAtMs_;
    bool actionIssued_;
    bool ntpAttemptInFlight_ = false;
    uint32_t ntpAttemptDeadlineMs_ = 0;
    uint32_t wifiAssociationTimeoutMs_;
    bool wifiAttemptInFlight_ = false;
    uint32_t wifiAttemptDeadlineMs_ = 0;
    // "Awaiting confirmed connect" window - see wifiAssociationResult()/
    // wifiConnectConfirmationPending()'s doc comments.
    bool wifiConfirmPending_ = false;
    uint32_t wifiConfirmDeadlineMs_ = 0;
    // Dedicated Wi-Fi reconnection backoff - deliberately separate from
    // retryDelayMs_/retryAtMs_ below (which DNS/Time/MQTT continue to use
    // exactly as before). Only the DEADLINE (wifiRetryAtMs_) is reseeded
    // from nowMs on a fresh loss-of-connectivity transition (a prompt
    // first reconnect attempt is correct behavior, and avoids a 32-bit
    // millis() wraparound hazard on a long-untouched deadline); the
    // GROWTH (this field) is never reset merely by that transition - only
    // an explicit failed/timed-out reconnect ATTEMPT grows it further, and
    // only mqttResult(true) (a full, genuinely stable success) resets it
    // back to the floor. See retryDelayMs()/retryAtMs()'s doc comments for
    // the context-sensitive public accessors.
    uint32_t wifiRetryDelayMs_;
    uint32_t wifiRetryAtMs_ = 0;

    uint32_t wifiRecoveryThreshold_;
    uint32_t wifiRecoveryCooldownMs_;
    uint32_t consecutiveConnectivityFailures_ = 0;
    bool wifiRecoveryCooldownActive_ = false;
    uint32_t wifiRecoveryCooldownUntilMs_ = 0;
    bool wifiRecoveryRequested_ = false;
    bool awaitingWifiRecoveryDisconnect_ = false;
};

} // namespace interbridge
