//! The main CLI logic.
// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::Result;
use nix::sys::signal;

fn inner_main(args: &[&str]) -> Result<()> {
    match args.get(1).copied() {
        // Add custom Rust commands here, and also in `libmain.cxx` if user-visible.
        Some("countme") => rpmostree_rust::countme::entrypoint(args),
        Some("ex-container") => rpmostree_rust::container::entrypoint(args),
        Some("module") => rpmostree_rust::modularity::entrypoint(args),
        _ => {
            // Otherwise fall through to C++ main().
            Ok(rpmostree_rust::ffi::rpmostree_main(&args)?)
        }
    }
}

// It is only recently that our main() function is in Rust, calling
// into C++ as a library.  As of right now, the only Rust commands
// are hidden, i.e. should not appear in --help.  So we just recognize
// those, and if there's something we don't know about, invoke the C++
// main().
fn main() {
    if std::env::var("RPMOSTREE_GDB_HOOK").is_ok() {
        println!("RPMOSTREE_GDB_HOOK detected; stopping...");
        println!("Attach via gdb using `gdb -p {}`.", std::process::id());
        signal::raise(signal::Signal::SIGSTOP).expect("signal(SIGSTOP)");
    }
    // Call this early on; it invokes e.g. setenv() so must be done
    // before we create threads.
    rpmostree_rust::ffi::early_main();
    // We need to write to stderr, because some of our commands write to stdout
    // like `rpm-ostree compose tree --print-json`.
    tracing_subscriber::fmt::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
        .with_writer(std::io::stderr)
        .init();
    tracing::trace!("starting");
    // Gather our arguments.
    let args: Vec<String> = std::env::args().collect();
    let args: Vec<&str> = args.iter().map(|s| s.as_str()).collect();
    // Process arguments, capturing any error.  Note that in some
    // cases the C++ code may call exit(<code>) directly.
    let r = inner_main(&args);
    // Print the error
    if let Err(e) = r {
        // See discussion in CxxResult for why we use this format
        let msg = format!("{:#}", e);
        rpmostree_rust::ffi::main_print_error(msg.as_str());
        std::process::exit(1)
    }
}
