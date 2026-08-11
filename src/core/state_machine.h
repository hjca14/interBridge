#pragma once

#include "events.h"

namespace interbridge {

// Initial state set covering only the basic InterBridge call cycle. This
// is NOT the final state machine - states like CONNECTING_WIFI, READY,
// OPENING_DOOR or RECOVERING are expected to be added once networking and
// door-actuation behavior are implemented. See CONTEXT.md.
enum class State {
    Boot,
    Idle,
    Ringing,
    InCall,
    Error,
};

const char* toString(State state);

// Function-pointer callback so StateMachine has no dependency on any
// concrete logging implementation (keeps this module testable in
// isolation, see test/test_state_machine).
using StateTransitionCallback = void (*)(State from, State to);

class StateMachine {
public:
    StateMachine();

    State getState() const;

    // Invoked on every successful transition. Pass nullptr to disable.
    void setTransitionCallback(StateTransitionCallback callback);

    // Moves Boot -> Idle. Called once by initializeStateMachine() after all
    // other modules have finished initializing. This is a deliberate,
    // explicit call rather than an event because "boot finished" is not
    // currently modeled as one of the events in events.h.
    void finishBoot();

    // Forces a transition to Error from any state. Used when a module
    // detects an unrecoverable problem. Error recovery (a RECOVERING
    // state, automatic retries, etc.) is not implemented yet.
    void reportFault();

    // Applies an event to the current state. Returns true if it caused a
    // transition, false if the event was invalid/ignored in this state.
    bool handleEvent(const Event& event);

private:
    void transitionTo(State newState);

    State state_;
    StateTransitionCallback onTransition_;
};

} // namespace interbridge
