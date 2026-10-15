#include "control_protocol.hpp"

#include <cstdint>
#include <vector>

int main()
{
    control::ControlFrame frame;
    frame.delta = -2;
    frame.magic = {0xaa, 0x55};
    frame.state = control::State::ready;
    const std::vector<std::uint8_t> expected = {0xff, 0xfe, 0xaa, 0x55, 0x12, 0x34};
    if (frame.encode() != expected) return 1;
    const auto decoded = control::ControlFrame::decode(expected);
    return decoded.delta == -2 && decoded.magic == frame.magic &&
                   decoded.state == control::State::ready
               ? 0
               : 2;
}
