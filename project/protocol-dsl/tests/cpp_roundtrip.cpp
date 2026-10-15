#include "telemetry_protocol.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
    telemetry::Sample sample;
    sample.header.sequence = 513;
    sample.header.kind = telemetry::MessageKind::sample;
    sample.header.flags = 0xa5;
    sample.timestamp = 0x01020304U;
    sample.payload_length = 3;
    sample.payload = {0x10, 0x20, 0x30};
    sample.checksum = 0xbeef;

    const std::vector<std::uint8_t> expected = {
        0x01, 0x02, 0x02, 0xa5, 0x04, 0x03, 0x02, 0x01,
        0x03, 0x00, 0x10, 0x20, 0x30, 0xef, 0xbe,
    };
    const auto encoded = sample.encode();
    if (encoded != expected) return 1;
    const auto decoded = telemetry::Sample::decode(encoded);
    if (decoded.header.sequence != sample.header.sequence ||
        decoded.header.kind != telemetry::MessageKind::sample ||
        decoded.payload != sample.payload || decoded.checksum != sample.checksum)
        return 2;
    std::cout << "010202a5040302010300102030efbe\n";
    return 0;
}
