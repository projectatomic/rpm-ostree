//! Implementation of the client-side of "rpm-ostree module".

// SPDX-License-Identifier: Apache-2.0 OR MIT

use anyhow::{anyhow, bail, Result};
use gio::DBusProxyExt;
use ostree_ext::variant_utils;
use structopt::StructOpt;

#[derive(Debug, StructOpt)]
#[structopt(name = "rpm-ostree module", no_version)]
#[structopt(rename_all = "kebab-case")]
enum Opt {
    /// Enable a module
    Enable(InstallOpts),
    /// Disable a module
    Disable(InstallOpts),
    /// Install a module
    Install(InstallOpts),
    /// Uninstall a module
    Uninstall(InstallOpts),
}

#[derive(Debug, StructOpt)]
struct InstallOpts {
    #[structopt(parse(from_str))]
    modules: Vec<String>,
    #[structopt(long)]
    reboot: bool,
    #[structopt(long)]
    lock_finalization: bool,
    #[structopt(long)]
    dry_run: bool,
}

const OPT_KEY_ENABLE_MODULES: &str = "enable-modules";
const OPT_KEY_DISABLE_MODULES: &str = "disable-modules";
const OPT_KEY_INSTALL_MODULES: &str = "install-modules";
const OPT_KEY_UNINSTALL_MODULES: &str = "uninstall-modules";

pub fn entrypoint(args: &[&str]) -> Result<()> {
    match Opt::from_iter(args.iter().skip(1)) {
        Opt::Enable(ref opts) => enable(opts),
        Opt::Disable(ref opts) => disable(opts),
        Opt::Install(ref opts) => install(opts),
        Opt::Uninstall(ref opts) => uninstall(opts),
    }
}

// XXX: Should split out a lot of the below into a more generic Rust wrapper around
// UpdateDeployment() like we have on the C side.

fn get_modifiers_variant(key: &str, modules: &[String]) -> Result<glib::Variant> {
    let r = glib::VariantDict::new(None);
    r.insert_value(key, &crate::variant_utils::new_variant_strv(modules));
    Ok(r.end())
}

fn get_options_variant(opts: &InstallOpts) -> Result<glib::Variant> {
    let r = glib::VariantDict::new(None);
    r.insert("no-pull-base", &true);
    r.insert("reboot", &opts.reboot);
    r.insert("lock-finalization", &opts.lock_finalization);
    r.insert("dry-run", &opts.dry_run);
    Ok(r.end())
}

fn enable(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_ENABLE_MODULES, opts)
}

fn disable(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_DISABLE_MODULES, opts)
}

fn install(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_INSTALL_MODULES, opts)
}

fn uninstall(opts: &InstallOpts) -> Result<()> {
    modules_impl(OPT_KEY_UNINSTALL_MODULES, opts)
}

fn modules_impl(key: &str, opts: &InstallOpts) -> Result<()> {
    eprintln!("NOTE: Modularity support is experimental and subject to change.");

    if opts.modules.is_empty() {
        bail!("At least one module must be specified");
    }

    let client = &mut crate::client::ClientConnection::new()?;
    let previous_deployment = client
        .get_os_proxy()
        .get_cached_property("DefaultDeployment")
        .ok_or_else(|| anyhow!("Failed to find default-deployment property"))?;
    let modifiers = get_modifiers_variant(key, &opts.modules)?;
    let options = get_options_variant(opts)?;
    let params = variant_utils::new_variant_tuple(&[modifiers, options]);
    let reply = &client.get_os_proxy().call_sync(
        "UpdateDeployment",
        Some(&params),
        gio::DBusCallFlags::NONE,
        -1,
        gio::NONE_CANCELLABLE,
    )?;
    let reply_child = crate::variant_utils::variant_tuple_get(reply, 0)
        .ok_or_else(|| anyhow!("Invalid reply"))?;
    let txn_address = reply_child
        .get_str()
        .ok_or_else(|| anyhow!("Expected string transaction address"))?;
    client.transaction_connect_progress_sync(txn_address)?;
    let new_deployment = client
        .get_os_proxy()
        .get_cached_property("DefaultDeployment")
        .ok_or_else(|| anyhow!("Failed to find default-deployment property"))?;
    if previous_deployment != new_deployment {
        unsafe {
            crate::ffi::print_treepkg_diff_from_sysroot_path(
                "/",
                crate::ffi::RpmOstreeDiffPrintFormat::RPMOSTREE_DIFF_PRINT_FORMAT_FULL_MULTILINE,
                0,
                std::ptr::null_mut(),
            );
        }
    }
    if opts.dry_run {
        println!("Exiting because of '--dry-run' option");
    }
    Ok(())
}
