//! C ABI bridge exposing `victory-shader` to the C++20 native runtime.
//!
//! Keeps the Rust shader compiler (`GFX10 -> SSA IR -> DXIL SM 6.6`) intact
//! while allowing `victory_native.dll` to request translations over a stable C
//! interface matching `native/include/victory_abi.h`.

use std::ffi::{c_char, c_void};
use std::ptr;

use victory_shader::emit::HostShader;
use victory_shader::{HwStage, RootType, TRANSLATOR_VERSION};

pub const VICTORY_ABI_VERSION: u32 = 1;

pub const VICTORY_OK: i32 = 0;
pub const VICTORY_ERR_INVALID_ARG: i32 = -1;
pub const VICTORY_ERR_TRANSLATE: i32 = -3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VictoryUserData {
    pub index: u32,
    pub dword: u32,
    pub other_code: u32,
    pub slot: u8,
    pub root_type: u8,
    pub reserved: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VictoryStaticSampler {
    pub filter: u32,
    pub address: [u32; 3],
    pub mip_lod_bias: f32,
    pub max_anisotropy: u32,
    pub comparison: u32,
    pub border: u32,
    pub min_lod: f32,
    pub max_lod: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VictoryRootIndexedView {
    pub constant_dword: u32,
    pub byte_offset: u32,
    pub table_slot: u8,
    pub constant_slot: u8,
    pub shift: u8,
    pub reserved: u8,
}

#[repr(C)]
pub struct VictoryTranslatedShader {
    pub abi_version: u32,
    pub translator_version: u32,
    pub stage: u8,
    pub reserved: [u8; 3],
    pub user_data_dwords: u32,
    pub dxil_ptr: *const u8,
    pub dxil_len: usize,
    pub user_data_ptr: *const VictoryUserData,
    pub user_data_len: usize,
    pub samplers_words_ptr: *const u32,
    pub static_samplers_ptr: *const VictoryStaticSampler,
    pub samplers_len: usize,
    pub root_cbvs_ptr: *const u8,
    pub root_cbvs_len: usize,
    pub root_srvs_ptr: *const u8,
    pub root_srvs_len: usize,
    pub root_uavs_ptr: *const u8,
    pub root_uavs_len: usize,
    pub root_indexed_views_ptr: *const VictoryRootIndexedView,
    pub root_indexed_views_len: usize,
    pub opaque_owner: *mut c_void,
}

struct OwnedTranslation {
    dxil: Vec<u8>,
    user_data: Vec<VictoryUserData>,
    samplers_words: Vec<u32>,
    static_samplers: Vec<VictoryStaticSampler>,
    root_cbvs: Vec<u8>,
    root_srvs: Vec<u8>,
    root_uavs: Vec<u8>,
    root_indexed_views: Vec<VictoryRootIndexedView>,
}

fn map_stage(stage: HwStage) -> u8 {
    match stage {
        HwStage::Ls => 0,
        HwStage::Hs => 1,
        HwStage::Es => 2,
        HwStage::Gs => 3,
        HwStage::Vs => 4,
        HwStage::Ps => 5,
        HwStage::Ms => 6,
        HwStage::Cs => 7,
    }
}

fn map_root_type(root: RootType) -> (u8, u32) {
    match root {
        RootType::Constants => (0, 0),
        RootType::Descriptor => (1, 0),
        RootType::Table => (2, 0),
        RootType::ImmediateSampler => (3, 0),
        RootType::ResourceHeap => (4, 0),
        RootType::SamplerHeap => (5, 0),
        RootType::Other(code) => (6, code),
    }
}

unsafe fn write_error(buf: *mut c_char, len: usize, msg: &str) {
    if buf.is_null() || len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let copy_len = bytes.len().min(len - 1);
    // SAFETY: caller guarantees `buf` is valid for `len` bytes.
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), buf.cast::<u8>(), copy_len);
        *buf.add(copy_len) = 0;
    }
}

fn into_c_shader(shader: HostShader) -> VictoryTranslatedShader {
    let user_data_dwords = shader.user_data_dwords();
    let stage = map_stage(shader.stage);
    let user_data: Vec<VictoryUserData> = shader
        .user_data
        .into_iter()
        .map(|u| {
            let (root_type, other_code) = map_root_type(u.root);
            VictoryUserData {
                index: u.index,
                dword: u.dword,
                other_code,
                slot: u.slot,
                root_type,
                reserved: [0; 2],
            }
        })
        .collect();
    let mut samplers_words = Vec::with_capacity(shader.samplers.len() * 4);
    for w in &shader.samplers {
        samplers_words.extend_from_slice(w);
    }
    let static_samplers: Vec<VictoryStaticSampler> = shader
        .static_samplers
        .into_iter()
        .map(|s| VictoryStaticSampler {
            filter: s.filter,
            address: s.address,
            mip_lod_bias: s.mip_lod_bias,
            max_anisotropy: s.max_anisotropy,
            comparison: s.comparison,
            border: s.border,
            min_lod: s.min_lod,
            max_lod: s.max_lod,
        })
        .collect();
    let root_indexed_views: Vec<VictoryRootIndexedView> = shader
        .root_indexed_views
        .into_iter()
        .map(|v| VictoryRootIndexedView {
            constant_dword: v.constant_dword,
            byte_offset: v.byte_offset,
            table_slot: v.table_slot,
            constant_slot: v.constant_slot,
            shift: v.shift,
            reserved: 0,
        })
        .collect();

    let mut owned = Box::new(OwnedTranslation {
        dxil: shader.dxil,
        user_data,
        samplers_words,
        static_samplers,
        root_cbvs: shader.root_cbvs,
        root_srvs: shader.root_srvs,
        root_uavs: shader.root_uavs,
        root_indexed_views,
    });

    let samplers_len = owned.static_samplers.len();
    let out = VictoryTranslatedShader {
        abi_version: VICTORY_ABI_VERSION,
        translator_version: TRANSLATOR_VERSION,
        stage,
        reserved: [0; 3],
        user_data_dwords,
        dxil_ptr: owned.dxil.as_ptr(),
        dxil_len: owned.dxil.len(),
        user_data_ptr: owned.user_data.as_ptr(),
        user_data_len: owned.user_data.len(),
        samplers_words_ptr: owned.samplers_words.as_ptr(),
        static_samplers_ptr: owned.static_samplers.as_ptr(),
        samplers_len,
        root_cbvs_ptr: owned.root_cbvs.as_ptr(),
        root_cbvs_len: owned.root_cbvs.len(),
        root_srvs_ptr: owned.root_srvs.as_ptr(),
        root_srvs_len: owned.root_srvs.len(),
        root_uavs_ptr: owned.root_uavs.as_ptr(),
        root_uavs_len: owned.root_uavs.len(),
        root_indexed_views_ptr: owned.root_indexed_views.as_ptr(),
        root_indexed_views_len: owned.root_indexed_views.len(),
        opaque_owner: (&raw mut *owned).cast::<c_void>(),
    };
    let _ = Box::into_raw(owned);
    out
}

#[unsafe(no_mangle)]
pub extern "C" fn victory_shader_abi_version() -> u32 {
    VICTORY_ABI_VERSION
}

#[unsafe(no_mangle)]
pub extern "C" fn victory_shader_translator_version() -> u32 {
    TRANSLATOR_VERSION
}

/// Translates an Xbox Series DXBC container into SM 6.6 DXIL + host binding metadata.
///
/// # Safety
/// `container_ptr` must be valid for `container_len` bytes, `out_shader` must be non-null
/// and writable, and if `err_buf` is non-null it must be writable for `err_buf_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn victory_shader_translate(
    container_ptr: *const u8,
    container_len: usize,
    host_max_wave: u32,
    host_rdna: u8,
    out_shader: *mut VictoryTranslatedShader,
    err_buf: *mut c_char,
    err_buf_len: usize,
) -> i32 {
    if container_ptr.is_null() || container_len == 0 || out_shader.is_null() {
        // SAFETY: `err_buf` contract verified by caller.
        unsafe { write_error(err_buf, err_buf_len, "null argument or zero length") };
        return VICTORY_ERR_INVALID_ARG;
    }
    victory_shader::emit::set_host_max_wave(host_max_wave);
    victory_shader::emit::set_host_rdna(host_rdna != 0);
    // SAFETY: `container_ptr` is valid for `container_len` bytes per caller contract.
    let bytes = unsafe { std::slice::from_raw_parts(container_ptr, container_len) };
    match victory_shader::emit::translate(bytes) {
        Ok(shader) => {
            // SAFETY: `out_shader` is non-null and writable.
            unsafe { out_shader.write(into_c_shader(shader)) };
            VICTORY_OK
        }
        Err(err) => {
            let msg = err.to_string();
            // SAFETY: `err_buf` contract verified by caller.
            unsafe { write_error(err_buf, err_buf_len, &msg) };
            VICTORY_ERR_TRANSLATE
        }
    }
}

/// Frees a `VictoryTranslatedShader` returned by `victory_shader_translate`.
///
/// # Safety
/// `shader` must either be null or point to a struct populated by `victory_shader_translate`
/// that has not yet been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn victory_shader_free(shader: *mut VictoryTranslatedShader) {
    if shader.is_null() {
        return;
    }
    // SAFETY: `shader` was initialized by `victory_shader_translate`.
    let owner = unsafe { (*shader).opaque_owner.cast::<OwnedTranslation>() };
    if !owner.is_null() {
        // SAFETY: `owner` came from `Box::into_raw` in `into_c_shader`.
        drop(unsafe { Box::from_raw(owner) });
    }
    // SAFETY: clear pointers so double-free is a no-op.
    unsafe { ptr::write_bytes(shader, 0, 1) };
}
