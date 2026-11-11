use rbpf::{assembler::assemble, EbpfVmNoData};
#[test]
fn existing_stack_paths() {
    let good=assemble("stdw [r10-8], 42\nldxdw r0, [r10-8]\nexit").unwrap();
    assert_eq!(EbpfVmNoData::new(Some(&good)).unwrap().execute_program().unwrap(),42);
    let unchecked=assemble("ldxdw r0, [r10-8]\nexit").unwrap();
    EbpfVmNoData::new(Some(&unchecked)).unwrap(); // only statically inspect this program
    println!("BASELINE_STACK_STORE_LOAD=42 UNWRITTEN_LOAD=accepted_by_syntax_verifier");
}
