//! Cargo binding to Impacto's native VM, renderer and session owner.
//! The distributable core is the CMake-built shared library in core-build.json.
use shinki_emu_abi::{CoreApi, Query, Status};

unsafe extern "C" {
    #[link_name = "shinki_emu_impacto_query"]
    fn native_query(version: u32, output: *mut *const CoreApi) -> Status;
}

/// Query the linked native Impacto implementation.
///
/// # Safety
/// `output` must be null or point to writable storage for a core API pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn shinki_emu_query(version: u32, output: *mut *const CoreApi) -> Status {
    unsafe { native_query(version, output) }
}

pub const QUERY: Query = shinki_emu_query;
