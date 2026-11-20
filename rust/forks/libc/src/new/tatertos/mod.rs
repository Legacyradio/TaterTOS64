//! TaterTOS native libc — "new"-style module tree.
//!
//! TaterTOS is an independent OS with its own libc (src/user/libc). That libc is
//! POSIX-source-compatible, so the portable POSIX surface routes through the
//! crate's shared `common::posix` layer (unistd, etc.). TaterTOS-specific ABI
//! (struct stat layout, SYS_* numbers, flag values) lives in
//! `crate::unix::tatertos` and is sourced from src/include/ headers — NOT Linux.
pub(crate) use crate::new::common::posix::*;
