use goblin::pe::{exception::{ExceptionData,UnwindInfo},data_directories::DataDirectory,options::ParseOptions};
fn main() {
 let mut opts=ParseOptions::default(); opts.resolve_rva=false;
 let normal=[0x10,0,0,0,0x20,0,0,0,0x30,0,0,0];
 let dir=DataDirectory{virtual_address:0,size:12};
 let ed=ExceptionData::parse_with_opts(&normal,dir,&[],0x200,&opts).unwrap();
 assert_eq!(ed.find_function(0x18).unwrap().unwrap().begin_address,0x10);
 assert!(ed.find_function(0x20).unwrap().is_none());
 println!("NORMAL_FUNCTION_OK range=10..20");
 let truncated=[0u8;4];
 let bad=ExceptionData::parse_with_opts(&truncated,dir,&[],0x200,&opts).unwrap();
 std::panic::set_hook(Box::new(|_|{}));
 assert!(std::panic::catch_unwind(||bad.functions().count()).is_err());
 println!("CURRENT_DIRECTORY_ACCEPTS_TRUNCATION_THEN_PANICS");
 let info=UnwindInfo::parse(&[1,4,1,0,4,1,0,0],0).unwrap();
 let mut it=info.unwind_codes();
 assert!(it.next().unwrap().is_err());
 assert!(it.next().unwrap().is_err());
 println!("CURRENT_UNWIND_ERROR_REPEATS");
}
