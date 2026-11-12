use std::io::Cursor;
use pdb::{PDB, StreamIndex};
fn u32_at(b: &mut [u8], p: usize, n: u32) { b[p..p+4].copy_from_slice(&n.to_le_bytes()); }
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let page = 512;
    let mut image = vec![0x91; page * 10];
    image[..32].copy_from_slice(b"Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\0\0\0");
    for (p,n) in [(32,512),(36,1),(40,10),(44,32),(48,0),(52,1),(page,2)] {
        u32_at(&mut image, p, n);
    }
    // Three streams: absent, empty, 515-byte payload with noncontiguous pages.
    for (i,n) in [3,u32::MAX,0,515,6,4,0,0].iter().enumerate() {
        u32_at(&mut image, 2*page+4*i, *n);
    }
    let expected: Vec<u8> = (0..515).map(|i| (i % 251) as u8).collect();
    image[6*page..7*page].copy_from_slice(&expected[..512]);
    image[4*page..4*page+3].copy_from_slice(&expected[512..]);
    let mut pdb = PDB::open(Cursor::new(image))?;
    assert!(pdb.raw_stream(StreamIndex(0)).is_err());
    assert_eq!(pdb.raw_stream(StreamIndex(1))?.unwrap().as_slice(), b"");
    let saved = pdb.raw_stream(StreamIndex(2))?.unwrap();
    assert_eq!(saved.as_slice(), expected);
    assert_eq!(pdb.raw_stream(StreamIndex(2))?.unwrap().as_slice(), expected);
    assert_eq!(saved.as_slice(), expected);
    println!("MSF_START_OK absent=1 empty=1 payload=515 retained=1");
    Ok(())
}
