#![allow(dead_code)]
// Compile the original demuxer modules in an isolated small harness so their
// existing unit tests run without resolving unrelated player/device members.
#[path = "../../symphonia-bundle-mp3/src/common.rs"]
mod common;
#[path = "../../symphonia-bundle-mp3/src/header.rs"]
mod header;
#[path = "../../symphonia-bundle-mp3/src/demuxer.rs"]
mod demuxer;
