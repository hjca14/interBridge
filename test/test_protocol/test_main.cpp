#include <unity.h>

#include "../../src/network/protocol.h"

using namespace interbridge;

void setUp() {}
void tearDown() {}

void test_null_protocol_reports_disconnected() {
    NullProtocol protocol;
    TEST_ASSERT_FALSE(protocol.isConnected());
}

void test_null_protocol_send_returns_false() {
    NullProtocol protocol;
    uint8_t payload[] = {1, 2, 3};
    ProtocolMessage message{payload, sizeof(payload)};
    TEST_ASSERT_FALSE(protocol.send(message));
}

void test_null_protocol_update_does_not_crash() {
    NullProtocol protocol;
    protocol.update();
    TEST_ASSERT_FALSE(protocol.isConnected());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_null_protocol_reports_disconnected);
    RUN_TEST(test_null_protocol_send_returns_false);
    RUN_TEST(test_null_protocol_update_does_not_crash);
    return UNITY_END();
}
