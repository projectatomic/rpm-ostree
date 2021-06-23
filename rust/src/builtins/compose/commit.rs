//! CLI sub-command `compose commit`.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use crate::cxxrsutil::{CxxResult, FFIGObjectWrapper};
use anyhow::anyhow;
use fn_error_context::context;
use indoc::printdoc;
use std::pin::Pin;

/// Print statistics related to an ostree transaction.
pub fn print_ostree_txn_stats(mut stats: Pin<&mut crate::FFIOstreeRepoTransactionStats>) {
    let stats = &stats.gobj_wrap();
    printdoc!(
        "Metadata Total: {meta_total}
        Metadata Written: {meta_written}
        Content Total: {content_total}
        Content Written: {content_written}
        Content Cache Hits: {cache_hits}
        Content Bytes Written: {content_bytes}
        ",
        meta_total = stats.get_metadata_objects_total(),
        meta_written = stats.get_metadata_objects_written(),
        content_total = stats.get_content_objects_total(),
        content_written = stats.get_content_objects_written(),
        cache_hits = stats.get_devino_cache_hits(),
        content_bytes = stats.get_content_bytes_written()
    );
}

#[context("Writing commit-id to {}", target_path)]
pub fn write_commit_id(target_path: &str, revision: &str) -> CxxResult<()> {
    if target_path.is_empty() {
        return Err(anyhow!("empty target path").into());
    }
    if revision.is_empty() {
        return Err(anyhow!("empty revision content").into());
    }
    std::fs::write(target_path, revision)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_write_commit_id() {
        write_commit_id("", "foo").unwrap_err();
        write_commit_id("/foo", "").unwrap_err();

        let tmpdir = tempfile::tempdir().unwrap();
        let filepath = tmpdir.path().join("commit-id");
        let expected_id = "my-revision-id";
        write_commit_id(&filepath.to_string_lossy(), &expected_id).unwrap();
        let read = std::fs::read_to_string(&filepath).unwrap();
        assert_eq!(read, expected_id);
    }
}
