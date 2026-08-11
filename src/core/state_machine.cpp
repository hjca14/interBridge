#include "state_machine.h"

namespace interbridge {

const char* toString(State state) {
    switch (state) {
        case State::Boot: return "BOOT";
        case State::Idle: return "IDLE";
        case State::Ringing: return "RINGING";
        case State::InCall: return "IN_CALL";
        case State::Error: return "ERROR";
    }
    return "UNKNOWN_STATE";
}

StateMachine::StateMachine() : state_(State::Boot), onTransition_(nullptr) {}

State StateMachine::getState() const {
    return state_;
}

void StateMachine::setTransitionCallback(StateTransitionCallback callback) {
    onTransition_ = callback;
}

void StateMachine::finishBoot() {
    if (state_ == State::Boot) {
        transitionTo(State::Idle);
    }
}

void StateMachine::reportFault() {
    transitionTo(State::Error);
}

bool StateMachine::handleEvent(const Event& event) {
    switch (state_) {
        case State::Boot:
            // Events are ignored while booting; call finishBoot() once
            // initialization completes.
            return false;

        case State::Idle:
            if (event.type == EventType::RingDetected) {
                transitionTo(State::Ringing);
                return true;
            }
            return false;

        case State::Ringing:
            if (event.type == EventType::OffHook) {
                transitionTo(State::InCall);
                return true;
            }
            if (event.type == EventType::OnHook) {
                // Caller hung up before the call was answered.
                transitionTo(State::Idle);
                return true;
            }
            return false;

        case State::InCall:
            if (event.type == EventType::OnHook) {
                transitionTo(State::Idle);
                return true;
            }
            return false;

        case State::Error:
            // No recovery path implemented yet. See CONTEXT.md > Open
            // Questions (watchdog / recovery strategy).
            return false;
    }
    return false;
}

void StateMachine::transitionTo(State newState) {
    State previous = state_;
    state_ = newState;
    if (onTransition_) {
        onTransition_(previous, newState);
    }
}

} // namespace interbridge
