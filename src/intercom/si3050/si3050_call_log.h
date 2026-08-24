#pragma once

#include <string>
#include <vector>

namespace interbridge {

// Shared, ordered call log used only by the Si3050 fakes (FakeSi3050Bus,
// FakePcmClock, FakeSi3050Reset, FakeDelayProvider) so a single native
// test can assert the exact interleaved call order across all four
// collaborators - e.g. "SCLK held high before RESET is released" spans
// two different fakes, and a boolean flag on either one alone cannot
// express "happened strictly before". Not used by any real (Esp32*)
// implementation or by production code.
using Si3050CallLog = std::vector<std::string>;

} // namespace interbridge
