use std::io::Cursor;
use symphonia_bundle_mp3::MpaReader;
use symphonia_core::formats::prelude::*;
use symphonia_core::io::MediaSourceStream;
fn main() {
    let mut bytes = Vec::new();
    // MPEG1 Layer3, 128 kbps/44.1kHz, 417 bytes including header.
    for _ in 0..12 {
        let mut frame = vec![0u8; 417];
        frame[..4].copy_from_slice(&[0xff,0xfb,0x90,0x00]);
        bytes.extend(frame);
    }
    let src = MediaSourceStream::new(Box::new(Cursor::new(bytes)), Default::default());
    let mut r = MpaReader::try_new(src, FormatOptions::default()).unwrap();
    let mut count=0;
    while let Some(packet)=r.next_packet().unwrap() {
        assert_eq!(packet.data.len(), 417);
        assert_eq!(packet.pts, Timestamp::from(count*1152));
        count+=1;
    }
    assert_eq!(count,12);
    println!("SYMPHONIA_START_OK packets=12 duration=1152");
}
