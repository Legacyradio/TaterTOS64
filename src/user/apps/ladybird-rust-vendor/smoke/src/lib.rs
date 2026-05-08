// TaterTOS64v3 — Phase R-4 build-std smoke crate.
// Origin log: logs/fry847.txt
//
// Goal: prove cargo + rustc + the x86_64-unknown-tatertos target spec
// can compile core (and later alloc) for the custom target.
// Exposes one extern "C" symbol so a C++ unit test can verify the .a
// produced is link-compatible with our cross-toolchain.

#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub extern "C" fn tater_rust_smoke(x: u64) -> u64 {
    x.wrapping_mul(3).wrapping_add(7)
}
