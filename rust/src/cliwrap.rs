//! Implements the `cliwrap` treefile option which intercepts/proxies
//! other binaries like `/usr/bin/rpm`.

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, Result};
use fn_error_context::context;
use openat_ext::OpenatDirExt;
use rayon::prelude::*;
use std::io::prelude::*;
use std::path::Path;
mod cliutil;
mod dracut;
mod grubby;
mod rpm;
mod yumdnf;
use crate::cxxrsutil::CxxResult;
use crate::ffiutil::*;

/// Location for the underlying (not wrapped) binaries.
pub const CLIWRAP_DESTDIR: &str = "usr/libexec/rpm-ostree/wrapped";

/// Binaries that will be wrapped if they exist.
static WRAPPED_BINARIES: &[&str] = &["usr/bin/rpm", "usr/bin/dracut", "usr/sbin/grubby"];

/// Binaries we will wrap, or create if they don't exist.
static MUSTWRAP_BINARIES: &[&str] = &["usr/bin/yum", "usr/bin/dnf"];

#[derive(Debug, PartialEq)]
pub(crate) enum RunDisposition {
    Ok,
    Warn,
    Notice(String),
}

/// Main entrypoint for cliwrap
pub fn entrypoint(args: &[&str]) -> Result<()> {
    // Skip the initial bits
    let args = &args[2..];
    // We'll panic here if the vector is empty, but that is intentional;
    // the outer code should always pass us at least one arg.
    let name = args[0];
    let name = match std::path::Path::new(name).file_name() {
        Some(name) => name,
        None => return Err(anyhow!("Invalid wrapped binary: {}", name).into()),
    };
    // We know we had a string from above
    let name = name.to_str().unwrap();

    // And now these are the args for the command
    let args = &args[1..];

    // If we're not booted into ostree, just run the child directly.
    if !cliutil::is_ostree_booted() {
        Ok(cliutil::exec_real_binary(name, &args)?)
    } else {
        match name {
            "rpm" => Ok(self::rpm::main(&args)?),
            "yum" | "dnf" => Ok(self::yumdnf::main(&args)?),
            "dracut" => Ok(self::dracut::main(&args)?),
            "grubby" => Ok(self::grubby::main(&args)?),
            _ => Err(anyhow!("Unknown wrapped binary: {}", name).into()),
        }
    }
}

#[context("Writing wrapper for {:?}", binpath)]
fn write_one_wrapper(rootfs_dfd: &openat::Dir, binpath: &Path, allow_noent: bool) -> Result<()> {
    let exists = rootfs_dfd.exists(binpath)?;
    if !exists && allow_noent {
        return Ok(());
    }

    let name = binpath.file_name().unwrap().to_str().unwrap();

    if exists {
        let destpath = format!("{}/{}", CLIWRAP_DESTDIR, name);
        rootfs_dfd.local_rename(binpath, destpath.as_str())?;
        rootfs_dfd.write_file_with(binpath, 0o755, |w| {
            indoc::writedoc! {w, r#"
#!/bin/sh
# Wrapper created by rpm-ostree to override
# behavior of the underlying binary.  For more
# information see `man rpm-ostree`.  The real
# binary is now located at: {}
exec /usr/bin/rpm-ostree cliwrap $0 "$@"
"#,  destpath }
        })?;
    } else {
        rootfs_dfd.write_file_with(binpath, 0o755, |w| {
            indoc::writedoc! {w, r#"
#!/bin/sh
# Wrapper created by rpm-ostree to implement this CLI interface.
# For more information see `man rpm-ostree`.
exec /usr/bin/rpm-ostree cliwrap $0 "$@"
"#}
        })?;
    }
    Ok(())
}

/// Move the real binaries to a subdir, and replace them with
/// a shell script that calls our wrapping code.
fn write_wrappers(rootfs_dfd: &openat::Dir) -> Result<()> {
    let destdir = std::path::Path::new(CLIWRAP_DESTDIR);
    rootfs_dfd.ensure_dir(destdir.parent().unwrap(), 0o755)?;
    rootfs_dfd.ensure_dir(destdir, 0o755)?;

    WRAPPED_BINARIES
        .par_iter()
        .map(|p| (Path::new(p), true))
        .chain(MUSTWRAP_BINARIES.par_iter().map(|p| (Path::new(p), false)))
        .try_for_each(|(binpath, allow_noent)| write_one_wrapper(rootfs_dfd, binpath, allow_noent))
}

pub(crate) fn cliwrap_write_wrappers(rootfs_dfd: i32) -> CxxResult<()> {
    Ok(write_wrappers(&ffi_view_openat_dir(rootfs_dfd))?)
}

pub(crate) fn cliwrap_destdir() -> String {
    // We return an owned string because it's used by C so we want c_str()
    CLIWRAP_DESTDIR.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Context;

    fn file_contains(
        d: &openat::Dir,
        p: impl AsRef<Path>,
        expected_contents: impl AsRef<str>,
    ) -> Result<bool> {
        let p = p.as_ref();
        let expected_contents = expected_contents.as_ref();
        let found_contents = d
            .read_to_string(p)
            .with_context(|| format!("Reading {:?}", p))?;
        Ok(found_contents.contains(expected_contents))
    }

    #[test]
    fn test_write_wrappers() -> Result<()> {
        let td = tempfile::tempdir()?;
        let td = &openat::Dir::open(td.path())?;
        for &d in &["usr/bin", "usr/libexec"] {
            td.ensure_dir_all(d, 0o755)?;
        }
        td.write_file_contents("usr/bin/rpm", 0o755, "this is rpm")?;
        write_wrappers(td)?;
        assert!(file_contains(
            td,
            "usr/bin/rpm",
            "binary is now located at: usr/libexec/rpm-ostree/wrapped/rpm"
        )?);
        assert!(!td.exists("usr/sbin/grubby")?);
        assert!(file_contains(
            td,
            "usr/bin/yum",
            "to implement this CLI interface"
        )?);
        Ok(())
    }
}
