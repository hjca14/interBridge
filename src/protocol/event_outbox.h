#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "../storage/persistent_store.h"

namespace interbridge {

constexpr size_t kDefaultEventOutboxCapacity = 32;

// A single queued event, ready to publish/replay. event_id is preserved
// exactly as originally generated (see core/random_id.h), even across a
// store/reload cycle, so downstream consumers can dedupe on event_id
// regardless of how many times a replay occurs.
struct OutboxEntry {
    std::string eventId;
    std::string eventJson; // full serialized DeviceEvent payload
};

// Queues events that must survive a temporary MQTT/network outage and be
// replayed on reconnect (RING_DETECTED, CALL_STARTED, CALL_ENDED,
// DOOR_OPENED, DOOR_OPEN_FAILED, OTA_FAILED, security-relevant ERROR -
// see docs/communication-protocol.md > Event Outbox). Bounded capacity;
// oldest entries are evicted first (FIFO) once full. Enqueuing an
// event_id that is already queued replaces that entry rather than
// duplicating it.
class IEventOutbox {
public:
    virtual ~IEventOutbox() = default;

    virtual void enqueue(const std::string& eventId, const std::string& eventJson) = 0;

    // All currently queued entries, oldest first. Entries are not
    // removed until dequeue() is called (normally after a successful
    // publish) - see device_transport.* for the reconnect-flush flow.
    virtual std::vector<OutboxEntry> pending() const = 0;

    virtual void dequeue(const std::string& eventId) = 0;

    virtual size_t size() const = 0;
};

class MemoryEventOutbox : public IEventOutbox {
public:
    explicit MemoryEventOutbox(size_t capacity = kDefaultEventOutboxCapacity);

    void enqueue(const std::string& eventId, const std::string& eventJson) override;
    std::vector<OutboxEntry> pending() const override;
    void dequeue(const std::string& eventId) override;
    size_t size() const override;

private:
    size_t capacity_;
    std::deque<OutboxEntry> entries_;
};

// Persists the outbox to an IPersistentStore as a single serialized blob,
// written only when the queue changes. Real ESP32 NVS wear/size behavior
// still needs on-hardware validation - see CONTEXT.md > Hardware
// Dependencies.
class PersistentEventOutbox : public IEventOutbox {
public:
    explicit PersistentEventOutbox(IPersistentStore& store, size_t capacity = kDefaultEventOutboxCapacity);

    void enqueue(const std::string& eventId, const std::string& eventJson) override;
    std::vector<OutboxEntry> pending() const override;
    void dequeue(const std::string& eventId) override;
    size_t size() const override;

private:
    void load();
    void save() const;

    IPersistentStore& store_;
    MemoryEventOutbox memory_;
};

} // namespace interbridge
