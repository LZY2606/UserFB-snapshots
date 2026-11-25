// Compile binding only. The implementation is supplied by the requested change.
fn require_transport(_: &rpt::mirrors::MirrorTransport) {}
fn require_panel(_: &rpt::mirrors::MirrorPanel) {}
fn main() { let _ = require_transport; let _ = require_panel; }
