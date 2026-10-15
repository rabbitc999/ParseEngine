#include "control_protocol.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
    static const uint8_t magic[] = {0xaa, 0x55};
    static const uint8_t expected[] = {0xff, 0xfe, 0xaa, 0x55, 0x12, 0x34};
    uint8_t output[16];
    size_t written = 0;
    control_error error = CONTROL_ERROR_NONE;
    control_ControlFrame frame;
    control_ControlFrame decoded;
    frame.delta = -2;
    frame.magic.data = magic;
    frame.magic.size = sizeof(magic);
    frame.state = CONTROL_STATE_READY;
    if (!control_ControlFrame_encode(&frame, output, sizeof(output), &written, &error)) return 1;
    if (written != sizeof(expected) || memcmp(output, expected, sizeof(expected)) != 0) return 2;
    if (!control_ControlFrame_decode(output, written, &decoded, &error)) return 3;
    return decoded.delta == -2 && decoded.state == CONTROL_STATE_READY ? 0 : 4;
}
