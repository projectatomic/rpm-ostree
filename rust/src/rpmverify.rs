//! Use rpm-oxide to pre-validate structure of RPMs.
//!
//! For more information, see https://github.com/QubesOS/qubes-rpm-oxide
//!
//! As of this moment, we do not actually use rpm-oxide to validate
//! GPG signatures.  This only "pre-sanitizes" the RPM to attempt to
//! avoid memory safety issues in librpm.
//!

use anyhow::{anyhow, Context, Result};
use std::fs::File;
use std::io::BufReader;

/// Validate the GPG signature and payload digest of an RPM.
// https://github.com/QubesOS/qubes-rpm-oxide/blob/main/rpm-parser/bin/rpmcheck.rs
pub(crate) fn rpm_gpgcheck_and_payload_validate(path: &str) -> Result<()> {
    let token = rpm_crypto::init();
    let mut r =
        BufReader::new(File::open(path).with_context(|| format!("Failed to open {}", path))?);
    let package = rpm_parser::RPMPackage::read(&mut r, openpgp_parser::AllowWeakHashes::No, token)?;
    let _ = package
        .signature
        .header_signature
        .ok_or_else(|| anyhow!("Package header is not signed"))?;
    let (mut ctx, digest) = package.immutable.payload_digest()?;
    std::io::copy(&mut r, &mut ctx)?;
    if ctx.finalize(true) != digest {
        return Err(anyhow!("Payload digest failed to verify!"));
    }
    Ok(())
}
