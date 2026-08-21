#include <cstring>
#include <unity.h>
#include "../../src/protocol/messages.h"
using namespace interbridge;
namespace { const char* device="ib-0123456789abcdef0123456789abcdef"; const char* id="0123456789abcdef0123456789abcdef";
std::string valid(){return std::string(R"({"protocol_version":1,"device_id":")")+device+R"(","command_id":")"+id+R"(","command":"OPEN_DOOR","parameters":{},"issued_at":1000,"expires_at":1010})";} CommandParseResult parse(const std::string& s){return parseCommand(s,device);} }
void setUp(){} void tearDown(){}
void test_valid(){TEST_ASSERT_EQUAL((int)CommandParseStatus::Ok,(int)parse(valid()).status);}
void test_bad_json(){TEST_ASSERT_EQUAL((int)CommandParseStatus::InvalidPayload,(int)parse("{").status);}
void test_large(){TEST_ASSERT_EQUAL((int)CommandParseStatus::PayloadTooLarge,(int)parse(std::string(kMaxJsonPayloadBytes+1,'x')).status);}
void test_version(){auto s=valid();s.replace(s.find(":1"),2,":2");TEST_ASSERT_EQUAL((int)CommandParseStatus::UnsupportedProtocolVersion,(int)parse(s).status);}
void test_device(){auto s=valid();s.replace(s.find(device),strlen(device),"ib-other");TEST_ASSERT_EQUAL((int)CommandParseStatus::InvalidPayload,(int)parse(s).status);}
void test_id(){auto s=valid();s.replace(s.find(id),strlen(id),"BAD");TEST_ASSERT_EQUAL((int)CommandParseStatus::InvalidPayload,(int)parse(s).status);}
void test_unknown(){auto s=valid();s.replace(s.find("OPEN_DOOR"),9,"UNKNOWN");auto r=parse(s);TEST_ASSERT_EQUAL((int)CommandType::Unknown,(int)r.command.type);}
void test_parameters(){auto s=valid();s.replace(s.find("{}"),2,"{\"dtmf\":\"1\"}");TEST_ASSERT_EQUAL((int)CommandParseStatus::InvalidPayload,(int)parse(s).status);}
void test_physical(){auto s=valid();s.insert(s.size()-1,",\"gpio\":2");TEST_ASSERT_EQUAL((int)CommandParseStatus::InvalidPayload,(int)parse(s).status);}
void test_timestamps(){auto s=valid();s.replace(s.find("\"issued_at\":1000,"),17,"");TEST_ASSERT_EQUAL((int)CommandParseStatus::InvalidPayload,(int)parse(s).status);}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_valid);RUN_TEST(test_bad_json);RUN_TEST(test_large);RUN_TEST(test_version);RUN_TEST(test_device);RUN_TEST(test_id);RUN_TEST(test_unknown);RUN_TEST(test_parameters);RUN_TEST(test_physical);RUN_TEST(test_timestamps);return UNITY_END();}
