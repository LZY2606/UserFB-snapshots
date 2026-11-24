use gimli::{DebugAranges,LittleEndian};
fn main(){
 // DWARF32 header aligned to 8-byte tuples; one legal range and null tuple.
 let mut b=Vec::new();b.extend_from_slice(&28u32.to_le_bytes());b.extend_from_slice(&2u16.to_le_bytes());
 b.extend_from_slice(&0x40u32.to_le_bytes());b.extend_from_slice(&[4,0]);b.extend_from_slice(&[0;4]);
 b.extend_from_slice(&0x1000u32.to_le_bytes());b.extend_from_slice(&0x20u32.to_le_bytes());b.extend_from_slice(&[0;8]);
 let d=DebugAranges::new(&b,LittleEndian);let mut hs=d.headers();let h=hs.next().unwrap().unwrap();
 assert_eq!(h.debug_info_offset().0,0x40);let mut it=h.entries();let e=it.next().unwrap().unwrap();
 assert_eq!(e.range().begin,0x1000);assert_eq!(e.range().end,0x1020);assert!(it.next().unwrap().is_none());
 println!("BASELINE cu=0x40 range=0x1000..0x1020");
}
