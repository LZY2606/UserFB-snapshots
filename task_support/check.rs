use std::sync::atomic::{AtomicUsize, Ordering};
/// Records executed checks and normal completion for a named Rust test group.
pub struct Group { name: &'static str, checks: AtomicUsize }
impl Group {
    pub fn new(name: &'static str) -> Self { Self { name, checks: AtomicUsize::new(0) } }
    pub fn check(&self, condition: bool, message: &str) {
        self.checks.fetch_add(1, Ordering::Relaxed);
        assert!(condition, "{}: {}", self.name, message);
    }
    pub fn finish(self) {
        let n=self.checks.load(Ordering::Relaxed);
        assert!(n>0,"{} ran no checks",self.name);
        println!("TASK_GROUP_DONE {} checks={}",self.name,n);
    }
}
