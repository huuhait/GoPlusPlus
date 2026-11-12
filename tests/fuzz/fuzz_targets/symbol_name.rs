#![no_main]

use gopp_fuzz::target;
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Err(finding) = target::symbol_name(data) {
        panic!("{finding}");
    }
});
