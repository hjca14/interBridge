#include <unity.h>
#include "../../src/hardware/clock.h"
#include "../../src/hardware/gpio.h"
#include "../../src/hardware/system_control.h"
#include "../../src/intercom/intercom.h"
#include "../../src/protocol/command_handler.h"
using namespace interbridge;
namespace {
class Hardware : public IHardwareIO { public: int calls=0; bool readLineState() override { return false; } bool setDoorOutput(bool) override { ++calls; return true; } };
DeviceCommand command(int64_t issued=995, int64_t expires=1005) { DeviceCommand c; c.type=CommandType::OpenDoor; c.rawCommand="OPEN_DOOR"; c.commandId="0123456789abcdef0123456789abcdef"; c.deviceId="ib-test"; c.issuedAtUnixSeconds=issued; c.expiresAtUnixSeconds=expires; c.hasIssuedAt=c.hasExpiresAt=true; return c; }
}
void setUp() {} void tearDown() {}
void test_disabled_two_phase_no_hardware() { Hardware hw; Intercom i(hw); FakeClock clock; clock.setUnixTimeSeconds(1000); InMemoryDedupCache cache; FakeSystemControl sys; CommandHandler h("ib-test",clock,cache,i,sys); auto r=h.handle(command()); TEST_ASSERT_EQUAL(2,r.size()); TEST_ASSERT_EQUAL((int)CommandStatus::Accepted,(int)r[0].status); TEST_ASSERT_EQUAL((int)CommandStatus::Rejected,(int)r[1].status); TEST_ASSERT_EQUAL((int)ProtocolErrorCode::CapabilityDisabled,(int)r[1].error->code); TEST_ASSERT_EQUAL(0,hw.calls); }
void test_duplicate_terminal_only() { Hardware hw; Intercom i(hw); FakeClock clock; clock.setUnixTimeSeconds(1000); InMemoryDedupCache cache; FakeSystemControl sys; CommandHandler h("ib-test",clock,cache,i,sys); h.handle(command()); auto r=h.handle(command()); TEST_ASSERT_EQUAL(1,r.size()); TEST_ASSERT_EQUAL((int)CommandStatus::Rejected,(int)r[0].status); TEST_ASSERT_EQUAL(0,hw.calls); }
void test_expired() { Hardware hw; Intercom i(hw); FakeClock clock; clock.setUnixTimeSeconds(2000); InMemoryDedupCache cache; FakeSystemControl sys; CommandHandler h("ib-test",clock,cache,i,sys); auto r=h.handle(command()); TEST_ASSERT_EQUAL((int)ProtocolErrorCode::CommandExpired,(int)r[0].error->code); TEST_ASSERT_EQUAL(0,hw.calls); }
void test_future() { Hardware hw; Intercom i(hw); FakeClock clock; clock.setUnixTimeSeconds(900); InMemoryDedupCache cache; FakeSystemControl sys; CommandHandler h("ib-test",clock,cache,i,sys); auto r=h.handle(command()); TEST_ASSERT_EQUAL((int)ProtocolErrorCode::InvalidTimestamp,(int)r[0].error->code); }
void test_untrusted_clock() { Hardware hw; Intercom i(hw); FakeClock clock; InMemoryDedupCache cache; FakeSystemControl sys; CommandHandler h("ib-test",clock,cache,i,sys); auto r=h.handle(command()); TEST_ASSERT_EQUAL((int)ProtocolErrorCode::ClockNotTrustworthy,(int)r[0].error->code); }
int main(int,char**) { UNITY_BEGIN(); RUN_TEST(test_disabled_two_phase_no_hardware); RUN_TEST(test_duplicate_terminal_only); RUN_TEST(test_expired); RUN_TEST(test_future); RUN_TEST(test_untrusted_clock); return UNITY_END(); }
