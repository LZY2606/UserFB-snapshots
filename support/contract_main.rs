use std::{collections::BTreeSet,fmt::Debug};
pub mod support {
 use super::*;
 pub struct Check { count:usize }
 impl Check {
  pub fn equal<T:PartialEq+Debug>(&mut self,actual:T,expected:T){assert_eq!(actual,expected);self.count+=1;}
  pub fn require(&mut self,value:bool,reason:&str){assert!(value,"{reason}");self.count+=1;}
 }
 pub struct Cases { items:Vec<(&'static str,fn(&mut Check))> }
 impl Cases {pub fn add(&mut self,name:&'static str,run:fn(&mut Check)){self.items.push((name,run));}}
 pub fn run(register:fn(&mut Cases)) {
  let required=BTreeSet::from(["aranges_alignment","line_selector","sequence_identity","flat_compatibility","bank_attribution"]);
  let mut cases=Cases{items:Vec::new()};register(&mut cases);
  let names:BTreeSet<_>=cases.items.iter().map(|(n,_)|*n).collect();
  assert_eq!(names,required,"register all five cases exactly once");assert_eq!(cases.items.len(),required.len());
  let mut total=0;
  for (name,run) in cases.items {let mut c=Check{count:0};run(&mut c);assert!(c.count>0,"zero comparisons: {name}");total+=c.count;println!("CASE_OK {name} comparisons={}",c.count);}
  let receipt=std::env::var("UFB_COMPLETION_PATH").expect("runner receipt path");
  let token=std::env::var("UFB_COMPLETION_TOKEN").expect("runner receipt token");
  std::fs::write(receipt,format!("{token} cases=5 comparisons={total}\n")).unwrap();
 }
}
#[path="../tests/segmented_contract.rs"] mod contract;
fn main(){support::run(contract::register);}
