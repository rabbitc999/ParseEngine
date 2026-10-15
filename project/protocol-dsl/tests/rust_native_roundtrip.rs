#[path = "../build/native-rust-control/control_protocol.rs"]
mod control;
#[path = "../build/native-rust-out/telemetry_protocol.rs"]
mod telemetry;

fn main() {
    let payload = [0x10, 0x20, 0x30];
    let sample = telemetry::Sample {
        header: telemetry::Header {
            sequence: 513,
            kind: telemetry::MessageKind::sample,
            flags: 0xa5,
        },
        timestamp: 0x01020304,
        payload_length: 3,
        payload: &payload,
        checksum: 0xbeef,
    };
    let expected = [
        0x01, 0x02, 0x02, 0xa5, 0x04, 0x03, 0x02, 0x01, 0x03, 0x00, 0x10, 0x20, 0x30, 0xef, 0xbe,
    ];
    let mut output = [0u8; 32];
    let size = sample.encode(&mut output).unwrap();
    assert_eq!(&output[..size], &expected);
    assert_eq!(telemetry::Sample::decode(&expected).unwrap(), sample);

    let frame = control::ControlFrame {
        delta: -2,
        magic: &[0xaa, 0x55],
        state: control::State::ready,
    };
    let mut big = [0u8; 8];
    let size = frame.encode(&mut big).unwrap();
    assert_eq!(&big[..size], &[0xff, 0xfe, 0xaa, 0x55, 0x12, 0x34]);
    assert_eq!(control::ControlFrame::decode(&big[..size]).unwrap(), frame);
}
