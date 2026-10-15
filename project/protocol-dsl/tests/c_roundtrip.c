#include "telemetry_protocol.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
    static const uint8_t payload[] = {0x10, 0x20, 0x30};
    static const uint8_t expected[] = {
        0x01, 0x02, 0x02, 0xa5, 0x04, 0x03, 0x02, 0x01,
        0x03, 0x00, 0x10, 0x20, 0x30, 0xef, 0xbe,
    };
    uint8_t buffer[32];
    size_t written = 0;
    telemetry_error error = TELEMETRY_ERROR_NONE;
    telemetry_Sample sample;
    telemetry_Sample decoded;

    sample.header.sequence = 513;
    sample.header.kind = TELEMETRY_MESSAGE_KIND_SAMPLE;
    sample.header.flags = 0xa5;
    sample.timestamp = UINT32_C(0x01020304);
    sample.payload_length = 3;
    sample.payload.data = payload;
    sample.payload.size = sizeof(payload);
    sample.checksum = UINT16_C(0xbeef);
    if (!telemetry_Sample_encode(&sample, buffer, sizeof(buffer), &written, &error)) return 1;
    if (written != sizeof(expected) || memcmp(buffer, expected, sizeof(expected)) != 0) return 2;
    if (!telemetry_Sample_decode(buffer, written, &decoded, &error)) return 3;
    if (decoded.header.sequence != 513 || decoded.payload.size != 3 ||
        memcmp(decoded.payload.data, payload, sizeof(payload)) != 0) return 4;
    return 0;
}
