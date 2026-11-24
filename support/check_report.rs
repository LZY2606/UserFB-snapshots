// Structural report check only. The independent semantic comparisons belong in the new contract.
use std::{fs,path::Path};
fn n(s:&str)->u64 {if let Some(h)=s.strip_prefix("0x"){u64::from_str_radix(h,16).unwrap()}else{s.parse().unwrap()}}
fn main(){
 let a:Vec<_>=std::env::args().collect();let fixture=Path::new(&a[1]);
 let q=fs::read_to_string(&a[2]).unwrap();let queries:Vec<_>=q.lines().skip(1).map(|l|{let x:Vec<_>=l.split('\t').collect();assert_eq!(x.len(),2);(n(x[0]),n(x[1]))}).collect();assert!(!queries.is_empty());
 let text=fs::read_to_string(&a[3]).unwrap();let mut ls=text.lines();
 assert_eq!(ls.next(),Some("query\tselector\toffset\tcu\tcu_name\tfunction\tdie\tsegment_die\tfile\tline\taranges\ttuple\tline_program\tsequence\trow\tbank\tbank_offset"));
 let mut seen=vec![false;queries.len()];let mut last=0;let mut resolved=0;
 for l in ls {let v:Vec<_>=l.split('\t').collect();assert_eq!(v.len(),17);assert!(v.iter().all(|s|!s.is_empty()));let i=n(v[0]) as usize;assert!(i<queries.len());assert!(i>=last);last=i;assert_eq!((n(v[1]),n(v[2])),queries[i]);seen[i]=true;
  if v[3]!="-" {resolved+=1;for k in [3,10,11]{let _=n(v[k]);}assert_ne!(v[4],"-");}else{for k in 4..15{assert_eq!(v[k],"-","unmapped CU cannot invent debug evidence");}}
  if v[5]!="-"{let _=n(v[6]);if v[7]=="-"{assert_eq!(n(v[1]),0,"non-flat function requires segment provider");}else{let _=n(v[7]);}}
  if v[8]!="-"{assert!(n(v[9])>0);for k in [12,13,14]{let _=n(v[k]);}}
  if v[15]!="-"{let p=Path::new(v[15]);assert_eq!(p.components().count(),1);let b=fs::read(fixture.join(p)).unwrap();assert!((n(v[16]) as usize)<b.len());}else{assert_eq!(v[16],"-");}
 }
 assert!(seen.into_iter().all(|s|s),"missing query rows");assert!(resolved>0,"all-unknown report");
 println!("REPORT_STRUCTURE_OK queries={}",queries.len());
}
