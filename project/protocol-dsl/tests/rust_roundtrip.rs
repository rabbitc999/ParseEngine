#[path = "../build/generated/telemetry_protocol.rs"]
mod protocol;
#[path = "../build/control-generated/control_protocol.rs"]
mod control;

use protocol::{Header, MessageKind, Sample};

fn main() {
    let payload = [0x10, 0x20, 0x30];
    let sample = Sample {
        header: Header { sequence: 513, kind: MessageKind::sample, flags: 0xa5 },
        timestamp: 0x01020304,
        payload_length: 3,
        payload: &payload,
        checksum: 0xbeef,
    };
    let expected = [
        0x01, 0x02, 0x02, 0xa5, 0x04, 0x03, 0x02, 0x01,
        0x03, 0x00, 0x10, 0x20, 0x30, 0xef, 0xbe,
    ];
    let mut output = [0u8; 32];
    let written = sample.encode(&mut output).unwrap();
    assert_eq!(&output[..written], &expected);
    assert_eq!(Sample::decode(&expected).unwrap(), sample);

    let control_frame = control::ControlFrame {
        delta: -2,
        magic: &[0xaa, 0x55],
        state: control::State::ready,
    };
    let mut control_output = [0u8; 8];
    let control_size = control_frame.encode(&mut control_output).unwrap();
    assert_eq!(&control_output[..control_size], &[0xff, 0xfe, 0xaa, 0x55, 0x12, 0x34]);
    assert_eq!(control::ControlFrame::decode(&control_output[..control_size]).unwrap(), control_frame);
}
