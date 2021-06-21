//! Helpers for GVariant until we can use a newer glib crate with https://github.com/gtk-rs/glib/pull/651
// SPDX-License-Identifier: Apache-2.0 OR MIT

use std::borrow::Cow;

use glib::translate::*;
use glib::ToVariant;

// These constants should really be in gtk-rs
lazy_static::lazy_static! {
    pub(crate) static ref TY_S: &'static glib::VariantTy = {
        glib::VariantTy::new("s").unwrap()
    };
    pub(crate) static ref TY_B: &'static glib::VariantTy = {
        glib::VariantTy::new("b").unwrap()
    };
    pub(crate) static ref TY_U: &'static glib::VariantTy = {
        glib::VariantTy::new("u").unwrap()
    };
}

pub(crate) fn variant_tuple_get(v: &glib::Variant, n: usize) -> Option<glib::Variant> {
    let v = v.to_glib_none();
    let l = unsafe { glib_sys::g_variant_n_children(v.0) };
    if n >= l {
        None
    } else {
        unsafe { from_glib_full(glib_sys::g_variant_get_child_value(v.0, n)) }
    }
}

pub(crate) fn new_variant_array(ty: &glib::VariantTy, children: &[glib::Variant]) -> glib::Variant {
    unsafe {
        let r = glib_sys::g_variant_new_array(
            ty.as_ptr() as *const _,
            children.to_glib_none().0,
            children.len(),
        );
        glib_sys::g_variant_ref_sink(r);
        from_glib_full(r)
    }
}

pub(crate) fn new_variant_strv(strv: &[impl AsRef<str>]) -> glib::Variant {
    let v: Vec<glib::Variant> = strv.iter().map(|s| s.as_ref().to_variant()).collect();
    new_variant_array(&TY_S, &v)
}

pub(crate) fn is_container(v: &glib::Variant) -> bool {
    unsafe { glib_sys::g_variant_is_container(v.to_glib_none().0) != glib_sys::GFALSE }
}

pub(crate) fn n_children(v: &glib::Variant) -> usize {
    assert!(is_container(v));
    unsafe { glib_sys::g_variant_n_children(v.to_glib_none().0) }
}

/// Find a string value in a GVariantDict
pub(crate) fn variant_dict_lookup_str(v: &glib::VariantDict, k: &str) -> Option<String> {
    // Unwrap safety: Passing the GVariant type string gives us the right value type
    v.lookup_value(k, Some(*TY_S))
        .map(|v| v.get_str().unwrap().to_string())
}

/// Find a boolean value in a GVariantDict
pub(crate) fn variant_dict_lookup_bool(v: &glib::VariantDict, k: &str) -> Option<bool> {
    // Unwrap safety: Passing the GVariant type string gives us the right value type
    v.lookup_value(k, Some(*TY_B)).map(|v| v.get().unwrap())
}

pub(crate) fn byteswap_be_to_native(v: &glib::Variant) -> Cow<glib::Variant> {
    if cfg!(target_endian = "big") {
        Cow::Borrowed(v)
    } else {
        unsafe {
            let r = glib_sys::g_variant_byteswap(v.to_glib_none().0);
            Cow::Owned(from_glib_full(r))
        }
    }
}
