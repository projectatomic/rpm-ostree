/*
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

/* Copied and adapted from:
 * https://github.com/cgwalters/coreos-assembler
 * */

use c_utf8::CUtf8Buf;
use failure::{Fallible, bail};
use openat;
use serde_derive::{Deserialize, Serialize};
use serde_json;
use std::collections::{HashMap, BTreeMap};
use std::io::prelude::*;
use std::path::Path;
use std::{collections, fs, io};
use std::os::unix::fs::MetadataExt;
use std::collections::btree_map::Entry;

use crate::utils;

const INCLUDE_MAXDEPTH: u32 = 50;

/// This struct holds file descriptors for any external files/data referenced by
/// a TreeComposeConfig.
struct TreefileExternals {
    postprocess_script: Option<fs::File>,
    add_files: collections::BTreeMap<String, fs::File>,
    passwd: Option<fs::File>,
    group: Option<fs::File>,
}

// This type name is exposed through ffi.
pub struct Treefile {
    // This one isn't used today, but we may do more in the future.
    _workdir: openat::Dir,
    primary_dfd: openat::Dir,
    #[allow(dead_code)] // Not used in tests
    pub(crate) parsed: TreeComposeConfig,
    // This is a copy of rojig.name to avoid needing to convert to CStr when reading
    rojig_name: Option<CUtf8Buf>,
    rojig_spec: Option<CUtf8Buf>,
    serialized: CUtf8Buf,
    externals: TreefileExternals,
    // This is a checksum over *all* the treefile inputs (recursed treefiles + externals).
    checksum: CUtf8Buf,
}

// We only use this while parsing
struct ConfigAndExternals {
    config: TreeComposeConfig,
    externals: TreefileExternals,
}

/// Parse a YAML treefile definition using base architecture `basearch`.
/// This does not open the externals.
fn treefile_parse_stream<R: io::Read>(
    fmt: utils::InputFormat,
    input: &mut R,
    basearch: Option<&str>,
) -> Fallible<TreeComposeConfig> {
    let mut treefile: TreeComposeConfig = utils::parse_stream(&fmt, input)?;

    treefile.basearch = match (treefile.basearch, basearch) {
        (Some(treearch), Some(arch)) => {
            if treearch != arch {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!(
                        "Invalid basearch {} on {}: cross-composes are not supported",
                        treearch, arch
                    ),
                )
                .into());
            } else {
                Some(treearch)
            }
        }
        (None, Some(arch)) => Some(arch.into()),
        // really, only for tests do we not specify a basearch. let's just canonicalize to None
        (_, None) => None,
    };

    // remove from packages-${arch} keys from the extra keys
    let mut archful_pkgs: Option<Vec<String>> = take_archful_pkgs(basearch, &mut treefile)?;

    if fmt == utils::InputFormat::YAML && !treefile.extra.is_empty() {
        let keys: Vec<&str> = treefile.extra.keys().map(|k| k.as_str()).collect();
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Unknown fields: {}", keys.join(", ")),
        )
        .into());
    }

    // Special handling for packages, since we allow whitespace within items.
    // We also canonicalize bootstrap_packages to packages here so it's
    // easier to append the basearch packages after.
    let mut pkgs: Vec<String> = vec![];
    {
        if let Some(base_pkgs) = treefile.packages.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&base_pkgs));
        }
        if let Some(bootstrap_pkgs) = treefile.bootstrap_packages.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&bootstrap_pkgs));
        }
        if let Some(archful_pkgs) = archful_pkgs.take() {
            pkgs.extend_from_slice(&whitespace_split_packages(&archful_pkgs));
        }
    }

    treefile.packages = Some(pkgs);
    treefile = treefile.migrate_legacy_fields()?;
    Ok(treefile)
}

/// Sanity checks that the packages-${basearch} entries are well-formed, and returns the ones
/// matching the current basearch.
fn take_archful_pkgs(
    basearch: Option<&str>,
    treefile: &mut TreeComposeConfig,
) -> Fallible<Option<Vec<String>>> {
    let mut archful_pkgs: Option<Vec<String>> = None;

    for key in treefile.extra.keys().filter(|k| k.starts_with("packages-")) {
        if !treefile.extra[key].is_array()
            || treefile.extra[key]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| !v.is_string())
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Invalid field {}: expected array of strings", key),
            )
            .into());
        }

        if let Some(basearch) = basearch {
            if basearch == &key["packages-".len()..] {
                assert!(archful_pkgs == None);
                archful_pkgs = Some(
                    treefile.extra[key]
                        .as_array()
                        .unwrap()
                        .iter()
                        .map(|v| v.as_str().unwrap().into())
                        .collect(),
                );
            }
        }
    }

    // and drop it from the map
    treefile
        .extra
        .retain(|ref k, _| !k.starts_with("packages-"));

    Ok(archful_pkgs)
}

// If a passwd/group file is provided explicitly, load it as a fd
fn load_passwd_file<P: AsRef<Path>>(
    basedir: P,
    v: &Option<CheckPasswd>,
) -> Fallible<Option<fs::File>> {
    if let &Some(ref v) = v {
        let basedir = basedir.as_ref();
        if let Some(ref path) = v.filename {
            return Ok(Some(utils::open_file(basedir.join(path))?));
        }
    }
    return Ok(None);
}

type IncludeMap = collections::BTreeMap<(u64, u64), String>;

/// Given a treefile filename and an architecture, parse it and also
/// open its external files.
fn treefile_parse<P: AsRef<Path>>(
    filename: P,
    basearch: Option<&str>,
    seen_includes: &mut IncludeMap,
) -> Fallible<ConfigAndExternals> {
    let filename = filename.as_ref();
    let f = utils::open_file(filename)?;
    let meta = f.metadata()?;
    let devino = (meta.dev(), meta.ino());
    match seen_includes.entry(devino) {
        Entry::Occupied(_) => {
            bail!("Include loop detected; {} was already included", filename.to_str().unwrap())
        },
        Entry::Vacant(e) => {
            e.insert(filename.to_str().unwrap().to_string());
        }
    };
    let mut f = io::BufReader::new(f);
    let fmt = utils::InputFormat::detect_from_filename(filename)?;
    let tf = treefile_parse_stream(fmt, &mut f, basearch).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Parsing {}: {}", filename.to_string_lossy(), e.to_string()),
        )
    })?;
    let postprocess_script = if let Some(ref postprocess) = tf.postprocess_script.as_ref() {
        Some(utils::open_file(filename.with_file_name(postprocess))?)
    } else {
        None
    };
    let mut add_files: BTreeMap<String, fs::File> = BTreeMap::new();
    if let Some(ref add_file_names) = tf.add_files.as_ref() {
        for (name, _) in add_file_names.iter() {
            add_files.insert(name.clone(), utils::open_file(filename.with_file_name(name))?);
        }
    }
    let parent = filename.parent().unwrap();
    let passwd = load_passwd_file(&parent, &tf.check_passwd)?;
    let group = load_passwd_file(&parent, &tf.check_groups)?;
    Ok(ConfigAndExternals {
        config: tf,
        externals: TreefileExternals {
            postprocess_script,
            add_files,
            passwd,
            group,
        },
    })
}

/// Merge a "basic" or non-array field. The semantics originally defined for
/// these are that first-one wins.
fn merge_basic_field<T>(dest: &mut Option<T>, src: &mut Option<T>) {
    if dest.is_some() {
        return;
    }
    *dest = src.take()
}

/// Merge a vector field by appending. This semantic was originally designed for
/// the `packages` key.
fn merge_vec_field<T>(dest: &mut Option<Vec<T>>, src: &mut Option<Vec<T>>) {
    if let Some(mut srcv) = src.take() {
        if let Some(ref mut destv) = dest {
            srcv.append(destv);
        }
        *dest = Some(srcv);
    }
}

/// Merge a map field similarly to Python's `dict.update()`. In case of
/// duplicate keys, `dest` wins (`src` is the "included" config).
fn merge_map_field<T>(
    dest: &mut Option<BTreeMap<String, T>>,
    src: &mut Option<BTreeMap<String, T>>)
{
    if let Some(mut srcv) = src.take() {
        if let Some(mut destv) = dest.take() {
            srcv.append(&mut destv);
        }
        *dest = Some(srcv);
    }
}

/// Given two configs, merge them.
fn treefile_merge(dest: &mut TreeComposeConfig, src: &mut TreeComposeConfig) {
    macro_rules! merge_basics {
        ( $($field:ident),* ) => {{
            $( merge_basic_field(&mut dest.$field, &mut src.$field); )*
        }};
    };
    macro_rules! merge_vecs {
        ( $($field:ident),* ) => {{
            $( merge_vec_field(&mut dest.$field, &mut src.$field); )*
        }};
    };
    macro_rules! merge_maps {
        ( $($field:ident),* ) => {{
            $( merge_map_field(&mut dest.$field, &mut src.$field); )*
        }};
    };

    merge_basics!(
        treeref,
        basearch,
        rojig,
        selinux,
        gpg_key,
        include,
        container,
        recommends,
        documentation,
        boot_location,
        tmp_is_dir,
        default_target,
        machineid_compat,
        releasever,
        automatic_version_prefix,
        mutate_os_release,
        preserve_passwd,
        check_passwd,
        check_groups,
        postprocess_script
    );
    merge_vecs!(
        repos,
        packages,
        bootstrap_packages,
        ostree_layers,
        ostree_override_layers,
        install_langs,
        initramfs_args,
        units,
        etc_group_members,
        ignore_removed_users,
        ignore_removed_groups,
        postprocess,
        add_files,
        remove_files,
        remove_from_packages
    );
    merge_maps!(
        add_commit_metadata
    );

    if let Some(mut src_experimental) = src.experimental.take() {
        if let Some(ref mut dest_experimental) = dest.experimental {
            merge_basic_field(
                &mut dest_experimental.sysusers,
                &mut src_experimental.sysusers,
            );
            merge_map_field(
                &mut dest_experimental.sysusers_users,
                &mut src_experimental.sysusers_users,
            );
            merge_map_field(
                &mut dest_experimental.sysusers_groups,
                &mut src_experimental.sysusers_groups,
            );
        } else {
            dest.experimental = Some(src_experimental)
        }
    }
}

/// Merge the treefile externals. There are currently only two keys that
/// reference external files.
fn treefile_merge_externals(dest: &mut TreefileExternals, src: &mut TreefileExternals) {
    // This one, being a basic-valued field, has first-wins semantics.
    if dest.postprocess_script.is_none() {
        dest.postprocess_script = src.postprocess_script.take();
    }

    // add-files is an array and hence has append semantics.
    dest.add_files.append(&mut src.add_files);

    // passwd/group are basic values
    if dest.passwd.is_none() {
        dest.passwd = src.passwd.take();
    }
    if dest.group.is_none() {
        dest.group = src.group.take();
    }
}

/// Recursively parse a treefile, merging along the way.
fn treefile_parse_recurse<P: AsRef<Path>>(
    filename: P,
    basearch: Option<&str>,
    depth: u32,
    seen_includes: &mut IncludeMap,
) -> Fallible<ConfigAndExternals> {
    let filename = filename.as_ref();
    let mut parsed = treefile_parse(filename, basearch, seen_includes)?;
    let include = parsed.config.include.take().unwrap_or_else(|| Include::Multiple(Vec::new()));
    let mut includes = match include {
        Include::Single(v) => vec![v],
        Include::Multiple(v) => v,
    };
    if let Some(mut arch_includes) = parsed.config.arch_include.take() {
        if let Some(basearch) = basearch {
            if let Some(arch_include_value) = arch_includes.remove(basearch) {
                match arch_include_value {
                    Include::Single(v) => includes.push(v),
                    Include::Multiple(v) => includes.extend(v),
                }
            }
        }
    }
    for include_path in includes.iter() {
        if depth == INCLUDE_MAXDEPTH {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("Reached maximum include depth {}", INCLUDE_MAXDEPTH),
            )
            .into());
        }
        let parent = filename.parent().unwrap();
        let include_path = parent.join(include_path);
        let mut included = treefile_parse_recurse(include_path, basearch, depth + 1, seen_includes)?;
        treefile_merge(&mut parsed.config, &mut included.config);
        treefile_merge_externals(&mut parsed.externals, &mut included.externals);
    }
    Ok(parsed)
}

// Similar to the importer check but just checks for prefixes since
// they're files, and also allows /etc since it's before conversion
fn add_files_path_is_valid(path: &str) -> bool {
    let path = path.trim_start_matches("/");
    (path.starts_with("usr/") && !path.starts_with("usr/local/"))
        || path.starts_with("etc/")
        || path.starts_with("bin/")
        || path.starts_with("sbin/")
        || path.starts_with("lib/")
        || path.starts_with("lib64/")
}

impl Treefile {
    /// The main treefile creation entrypoint.
    fn new_boxed(
        filename: &Path,
        basearch: Option<&str>,
        workdir: openat::Dir,
    ) -> Fallible<Box<Treefile>> {
        let mut seen_includes = collections::BTreeMap::new();
        let mut parsed = treefile_parse_recurse(filename, basearch, 0, &mut seen_includes)?;
        parsed.config = parsed.config.substitute_vars()?;
        parsed.config.canonicalize();
        Treefile::validate_config(&parsed.config)?;
        let dfd = openat::Dir::open(filename.parent().unwrap())?;
        let (rojig_name, rojig_spec) = if let Some(rojig) = parsed.config.rojig.as_ref() {
            (
                Some(CUtf8Buf::from_string(rojig.name.clone())),
                Some(Treefile::write_rojig_spec(&workdir, rojig)?),
            )
        } else {
            (None, None)
        };
        let serialized = Treefile::serialize_json_string(&parsed.config)?;
        // Notice we hash the *reserialization* of the final flattened treefile only so that e.g.
        // comments/whitespace/hash table key reorderings don't trigger a respin. We could take
        // this further by using a custom `serialize_with` for Vecs where ordering doesn't matter
        // (or just sort the Vecs).
        let mut hasher = glib::Checksum::new(glib::ChecksumType::Sha256);
        parsed.config.hasher_update(&mut hasher)?;
        parsed.externals.hasher_update(&mut hasher)?;
        Ok(Box::new(Treefile {
            primary_dfd: dfd,
            parsed: parsed.config,
            _workdir: workdir,
            rojig_name: rojig_name,
            rojig_spec: rojig_spec,
            serialized: serialized,
            externals: parsed.externals,
            checksum: CUtf8Buf::from_string(hasher.get_string().unwrap()),
        }))
    }

    /// Do some upfront semantic checks we can do beyond just the type safety serde provides.
    fn validate_config(config: &TreeComposeConfig) -> Fallible<()> {
        // check add-files
        if let Some(files) = &config.add_files {
            for (_, dest) in files.iter() {
                if !add_files_path_is_valid(&dest) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!("Unsupported path in add-files: {}", dest),
                    )
                    .into());
                }
            }
        }
        Ok(())
    }

    fn serialize_json_string(config: &TreeComposeConfig) -> Fallible<CUtf8Buf> {
        let mut output = vec![];
        serde_json::to_writer_pretty(&mut output, config)?;
        Ok(CUtf8Buf::from_string(
            String::from_utf8(output).expect("utf-8 json"),
        ))
    }

    /// Generate a rojig spec file.
    fn write_rojig_spec<'a, 'b>(workdir: &'a openat::Dir, r: &'b Rojig) -> Fallible<CUtf8Buf> {
        let description = r
            .description
            .as_ref()
            .and_then(|v| if v.len() > 0 { Some(v.as_str()) } else { None })
            .unwrap_or(r.summary.as_str());
        let name: String = format!("{}.spec", r.name);
        {
            let mut f = workdir.write_file(name.as_str(), 0o644)?;
            write!(
                f,
                r###"
# The canonical version of this is maintained by rpm-ostree.
# Suppress most build root processing we are just carrying
# binary data.
%global __os_install_post /usr/lib/rpm/brp-compress %{{nil}}
Name: {rpmostree_rojig_name}
Version:	%{{ostree_version}}
Release:	1%{{?dist}}
Summary:	{rpmostree_rojig_summary}
License:	{rpmostree_rojig_license}
#@@@rpmostree_rojig_meta@@@

%description
{rpmostree_rojig_description}

%prep

%build

%install
mkdir -p %{{buildroot}}%{{_prefix}}/lib/ostree-jigdo/%{{name}}
for x in *; do mv ${{x}} %{{buildroot}}%{{_prefix}}/lib/ostree-jigdo/%{{name}}; done

%files
%{{_prefix}}/lib/ostree-jigdo/%{{name}}
"###,
                rpmostree_rojig_name = r.name,
                rpmostree_rojig_summary = r.summary,
                rpmostree_rojig_license = r.license,
                rpmostree_rojig_description = description,
            )?;
        }
        Ok(CUtf8Buf::from_string(name))
    }
}

fn hash_file(hasher: &mut glib::Checksum, mut f: &fs::File) -> Fallible<()> {
    let mut reader = io::BufReader::with_capacity(128 * 1024, f);
    loop {
        // have to scope fill_buf() so we can consume() below
        let n = {
            let buf = reader.fill_buf()?;
            hasher.update(buf);
            buf.len()
        };
        if n == 0 {
            break;
        }
        reader.consume(n);
    }
    f.seek(io::SeekFrom::Start(0))?;
    Ok(())
}

impl TreefileExternals {
    fn hasher_update(&self, hasher: &mut glib::Checksum) -> Fallible<()> {
        if let Some(ref f) = self.postprocess_script {
            hash_file(hasher, f)?;
        }
        if let Some(ref f) = self.passwd {
            hash_file(hasher, f)?;
        }
        if let Some(ref f) = self.group {
            hash_file(hasher, f)?;
        }
        for f in self.add_files.values() {
            hash_file(hasher, f)?;
        }
        Ok(())
    }
}

/// For increased readability in YAML/JSON, we support whitespace in individual
/// array elements.
fn whitespace_split_packages(pkgs: &[String]) -> Vec<String> {
    pkgs.iter()
        .flat_map(|pkg| pkg.split_whitespace().map(String::from))
        .collect()
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
enum BootLocation {
    #[serde(rename = "new")]
    New,
    #[serde(rename = "modules")]
    Modules,
}

impl Default for BootLocation {
    fn default() -> Self {
        BootLocation::New
    }
}

#[derive(Serialize, Deserialize, Debug, Clone)]
enum CheckPasswdType {
    #[serde(rename = "none")]
    None,
    #[serde(rename = "previous")]
    Previous,
    #[serde(rename = "file")]
    File,
    #[serde(rename = "data")]
    Data,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
struct CheckPasswd {
    #[serde(rename = "type")]
    variant: CheckPasswdType,
    filename: Option<String>,
    // Skip this for now, a separate file is easier
    // and anyways we want to switch to sysusers
    // entries: Option<Map<>String>,
}

#[derive(Serialize, Deserialize, Debug)]
struct Rojig {
    name: String,
    summary: String,
    license: String,
    description: Option<String>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(untagged)]
enum Include {
    Single(String),
    Multiple(Vec<String>)
}

// Because of how we handle includes, *everything* here has to be
// Option<T>.  The defaults live in the code (e.g. machineid-compat defaults
// to `true`).
#[derive(Serialize, Deserialize, Debug)]
pub(crate) struct TreeComposeConfig {
    // Compose controls
    #[serde(rename = "ref")]
    #[serde(skip_serializing_if = "Option::is_none")]
    treeref: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    basearch: Option<String>,
    // Optional rojig data
    #[serde(skip_serializing_if = "Option::is_none")]
    rojig: Option<Rojig>,
    #[serde(skip_serializing_if = "Option::is_none")]
    repos: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    selinux: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "gpg-key")]
    gpg_key: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    include: Option<Include>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "arch-include")]
    arch_include: Option<BTreeMap<String, Include>>,

    // Core content
    #[serde(skip_serializing_if = "Option::is_none")]
    packages: Option<Vec<String>>,
    // Deprecated option
    #[serde(skip_serializing_if = "Option::is_none")]
    bootstrap_packages: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ostree-layers")]
    ostree_layers: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ostree-override-layers")]
    ostree_override_layers: Option<Vec<String>>,

    // Content installation opts
    #[serde(skip_serializing_if = "Option::is_none")]
    container: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    recommends: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    documentation: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "install-langs")]
    install_langs: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "initramfs-args")]
    initramfs_args: Option<Vec<String>>,

    // Tree layout options
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "boot-location")]
    boot_location: Option<BootLocation>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "tmp-is-dir")]
    tmp_is_dir: Option<bool>,

    // systemd
    #[serde(skip_serializing_if = "Option::is_none")]
    units: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "default-target")]
    default_target: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "machineid-compat")]
    // Defaults to `true`
    machineid_compat: Option<bool>,

    // versioning
    #[serde(skip_serializing_if = "Option::is_none")]
    releasever: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "automatic-version-prefix")]
    automatic_version_prefix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "mutate-os-release")]
    mutate_os_release: Option<String>,

    // passwd-related bits
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "etc-group-members")]
    etc_group_members: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "preserve-passwd")]
    preserve_passwd: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-passwd")]
    check_passwd: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "check-groups")]
    check_groups: Option<CheckPasswd>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-users")]
    ignore_removed_users: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "ignore-removed-groups")]
    ignore_removed_groups: Option<Vec<String>>,

    // Content manipulation
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "postprocess-script")]
    // This one references an external filename
    postprocess_script: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    // This one is inline, and supports multiple (hence is useful for inheritance)
    postprocess: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-files")]
    add_files: Option<Vec<(String, String)>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-files")]
    remove_files: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "remove-from-packages")]
    remove_from_packages: Option<Vec<Vec<String>>>,
    // The BTreeMap here is on purpose; it ensures we always re-serialize in sorted order so that
    // checksumming is deterministic across runs. (And serde itself uses BTreeMap for child objects
    // as well).
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "add-commit-metadata")]
    add_commit_metadata: Option<BTreeMap<String, serde_json::Value>>,

    #[serde(flatten)]
    legacy_fields: LegacyTreeComposeConfigFields,

    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "experimental")]
    pub(crate) experimental: Option<ExperimentalTreeComposeConfigFields>,

    #[serde(flatten)]
    extra: HashMap<String, serde_json::Value>,
}

#[derive(Serialize, Deserialize, Debug)]
struct LegacyTreeComposeConfigFields {
    #[serde(skip_serializing)]
    gpg_key: Option<String>,
    #[serde(skip_serializing)]
    boot_location: Option<BootLocation>,
    #[serde(skip_serializing)]
    default_target: Option<String>,
    #[serde(skip_serializing)]
    automatic_version_prefix: Option<String>,
}

#[derive(Serialize, Deserialize, Debug)]
pub(crate) struct ExperimentalTreeComposeConfigFields {
    #[serde(skip_serializing_if = "Option::is_none")]
    sysusers: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "sysusers-users")]
    pub(crate) sysusers_users: Option<BTreeMap<String, u32>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[serde(rename = "sysusers-groups")]
    pub(crate) sysusers_groups: Option<BTreeMap<String, u32>>,
}

impl TreeComposeConfig {
    /// Look for use of legacy/renamed fields and migrate them to the new field.
    fn migrate_legacy_fields(mut self) -> Fallible<Self> {
        macro_rules! migrate_field {
            ( $field:ident ) => {{
                if self.legacy_fields.$field.is_some() && self.$field.is_some() {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!("Cannot use new and legacy forms of {}", stringify!($field)),
                    )
                    .into());
                }
                self.$field = self.$field.or(self.legacy_fields.$field.take());
            }};
        };

        migrate_field!(gpg_key);
        migrate_field!(boot_location);
        migrate_field!(default_target);
        migrate_field!(automatic_version_prefix);

        Ok(self)
    }

    /// Look for use of ${variable} and replace it by its proper value
    fn substitute_vars(mut self) -> Fallible<Self> {
        let mut substvars: collections::HashMap<String, String> = collections::HashMap::new();
        // Substitute ${basearch} and ${releasever}
        if let Some(arch) = &self.basearch {
            substvars.insert("basearch".to_string(), arch.clone());
        }
        if let Some(releasever) = &self.releasever {
            substvars.insert("releasever".to_string(), releasever.clone());
        }
        envsubst::validate_vars(&substvars)?;

        macro_rules! substitute_field {
            ( $field:ident ) => {{
                if let Some(value) = self.$field.take() {
                    self.$field = if envsubst::is_templated(&value) {
                        match envsubst::substitute(value, &substvars) {
                            Ok(s) => Some(s),
                            Err(e) => return Err(e),
                        }
                    } else {
                        Some(value)
                    }
                }
            }};
        };
        substitute_field!(treeref);
        substitute_field!(automatic_version_prefix);
        substitute_field!(mutate_os_release);

        Ok(self)
    }

    fn hasher_update(&self, hasher: &mut glib::Checksum) -> Fallible<()> {
        // don't use pretty mode to increase the chances of a stable serialization
        // https://github.com/projectatomic/rpm-ostree/pull/1865
        hasher.update(serde_json::to_vec(self)?.as_slice());
        Ok(())
    }

    /// Some options override others; perform those overrides.
    fn canonicalize(&mut self) {
        // If sysusers is enabled, disable all the other passwd/group related bits.
        if let Some(ref experimental) = self.experimental {
            if let Some(true) = experimental.sysusers {
                self.preserve_passwd = Some(false);
                self.check_passwd = Some(CheckPasswd {
                    variant: CheckPasswdType::None,
                    filename: None,
                });
                self.check_groups = self.check_passwd.clone();
                self.ignore_removed_users = None;
                self.ignore_removed_groups = None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile;

    static ARCH_X86_64: &str = "x86_64";

    static VALID_PRELUDE: &str = r###"
ref: "exampleos/x86_64/blah"
packages:
 - foo bar
 - baz
packages-x86_64:
 - grub2 grub2-tools
packages-s390x:
 - zipl
"###;

    // This one has "comments" (hence unknown keys)
    static VALID_PRELUDE_JS: &str = r###"
{
 "ref": "exampleos/${basearch}/blah",
 "comment-packages": "We want baz to enable frobnication",
 "packages": ["foo", "bar", "baz"],
 "packages-x86_64": ["grub2", "grub2-tools"],
 "comment-packages-s390x": "Note that s390x uses its own bootloader",
 "packages-s390x": ["zipl"]
}
"###;

    #[test]
    fn basic_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    #[test]
    fn basic_valid_add_remove_files() {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(
            r###"
add-files:
  - - foo
    - /usr/bin/foo
  - - baz
    - /usr/bin/blah
remove-files:
 - foo
 - bar
"###,
        );
        let buf = buf.as_bytes();
        let mut input = io::BufReader::new(buf);
        let treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        assert!(treefile.add_files.unwrap().len() == 2);
        assert!(treefile.remove_files.unwrap().len() == 2);
    }

    #[test]
    fn basic_valid_sysusers() {
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(
            r###"
experimental:
  sysusers: true
  sysusers-users:
    foo: 42
    bar: 43
"###,
        );
        let buf = buf.as_bytes();
        let mut input = io::BufReader::new(buf);
        let treefile =
            treefile_parse_stream(InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        let overrides = treefile.experimental.unwrap().sysusers_users.unwrap();
        assert_eq!(overrides.len(), 2);
        assert_eq!(*overrides.get("foo").unwrap(), 42);
        assert_eq!(*overrides.get("bar").unwrap(), 43);
    }

    #[test]
    fn basic_js_valid() {
        let mut input = io::BufReader::new(VALID_PRELUDE_JS.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::JSON, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 5);
    }

    #[test]
    fn basic_valid_noarch() {
        let mut input = io::BufReader::new(VALID_PRELUDE.as_bytes());
        let mut treefile = treefile_parse_stream(utils::InputFormat::YAML, &mut input, None).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/blah");
        assert!(treefile.packages.unwrap().len() == 3);
    }

    fn append_and_parse(append: &'static str) -> TreeComposeConfig {
        let buf = VALID_PRELUDE.to_string() + append;
        let mut input = io::BufReader::new(buf.as_bytes());
        let treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile.substitute_vars().unwrap()
    }

    fn test_invalid(data: &'static str) {
        let buf = VALID_PRELUDE.to_string() + data;
        let mut input = io::BufReader::new(buf.as_bytes());
        match treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)) {
            Err(ref e) => match e.downcast_ref::<io::Error>() {
                Some(ref ioe) if ioe.kind() == io::ErrorKind::InvalidInput => {}
                _ => panic!("Expected invalid treefile, not {}", e.to_string()),
            },
            Ok(_) => panic!("Expected invalid treefile"),
        }
    }

    #[test]
    fn basic_valid_releasever() {
        let buf = r###"
ref: "exampleos/${basearch}/${releasever}"
releasever: 30
automatic-version-prefix: ${releasever}
mutate-os-release: ${releasever}
"###;
        let mut input = io::BufReader::new(buf.as_bytes());
        let mut treefile =
            treefile_parse_stream(utils::InputFormat::YAML, &mut input, Some(ARCH_X86_64)).unwrap();
        treefile = treefile.substitute_vars().unwrap();
        assert!(treefile.treeref.unwrap() == "exampleos/x86_64/30");
        assert!(treefile.releasever.unwrap() == "30");
        assert!(treefile.automatic_version_prefix.unwrap() == "30");
        assert!(treefile.mutate_os_release.unwrap() == "30");
    }

    #[test]
    fn test_valid_no_releasever() {
        let treefile = append_and_parse("automatic_version_prefix: ${releasever}");
        assert!(treefile.releasever == None);
        assert!(treefile.automatic_version_prefix.unwrap() == "${releasever}");
    }

    #[test]
    fn basic_valid_legacy() {
        let treefile = append_and_parse(
            "
gpg_key: foo
boot_location: new
default_target: bar
automatic_version_prefix: baz
        ",
        );
        assert!(treefile.gpg_key.unwrap() == "foo");
        assert!(treefile.boot_location.unwrap() == BootLocation::New);
        assert!(treefile.default_target.unwrap() == "bar");
        assert!(treefile.automatic_version_prefix.unwrap() == "baz");
    }

    #[test]
    fn basic_valid_legacy_new() {
        let treefile = append_and_parse(
            "
gpg-key: foo
boot-location: new
default-target: bar
automatic-version-prefix: baz
        ",
        );
        assert!(treefile.gpg_key.unwrap() == "foo");
        assert!(treefile.boot_location.unwrap() == BootLocation::New);
        assert!(treefile.default_target.unwrap() == "bar");
        assert!(treefile.automatic_version_prefix.unwrap() == "baz");
    }

    #[test]
    fn basic_invalid_legacy_both() {
        test_invalid(
            "
gpg-key: foo
gpg_key: bar
        ",
        );
        test_invalid(
            "
boot-location: new
boot_location: both
        ",
        );
        test_invalid(
            "
default-target: foo
default_target: bar
        ",
        );
        test_invalid(
            "
automatic-version-prefix: foo
automatic_version_prefix: bar
        ",
        );
    }

    #[test]
    fn test_invalid_install_langs() {
        test_invalid(
            r###"install_langs:
  - "klingon"
  - "esperanto"
"###,
        );
    }

    #[test]
    fn test_invalid_arch_packages_type() {
        test_invalid(
            r###"packages-hal9000: true
"###,
        );
    }

    #[test]
    fn test_invalid_arch_packages_array_type() {
        test_invalid(
            r###"packages-hal9000:
  - 12
  - 34
"###,
        );
    }

    fn new_test_treefile<'a, 'b>(workdir: &std::path::Path, contents: &'a str, basearch: Option<&'b str>) -> Fallible<Box<Treefile>> {
        let tf_path = workdir.join("treefile.yaml");
        utils::write_file(&tf_path, |b| { b.write_all(contents.as_bytes())?; Ok(()) })?;
        Ok(Treefile::new_boxed(
            tf_path.as_path(),
            basearch,
            openat::Dir::open(workdir)?,
        )?)
    }

    #[test]
    fn test_treefile_new() {
        let workdir = tempfile::tempdir().unwrap();
        let tf = new_test_treefile(workdir.path(), VALID_PRELUDE, None).unwrap();
        assert!(tf.parsed.rojig.is_none());
        assert!(tf.rojig_spec.is_none());
        assert!(tf.parsed.machineid_compat.is_none());
    }

    const ROJIG_YAML: &'static str = r###"
rojig:
  name: "exampleos"
  license: "MIT"
  summary: "ExampleOS rojig base image"
"###;

    #[test]
    fn test_treefile_new_rojig() {
        let workdir = tempfile::tempdir().unwrap();
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(ROJIG_YAML);
        let tf = new_test_treefile(workdir.path(), buf.as_str(), None).unwrap();
        let rojig = tf.parsed.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
        let rojig_spec_str = tf.rojig_spec.as_ref().unwrap().as_str();
        let rojig_spec = Path::new(rojig_spec_str);
        assert!(rojig_spec.file_name().unwrap() == "exampleos.spec");
    }

    #[test]
    fn test_treefile_includes() -> Fallible<()> {
        let workdir = tempfile::tempdir()?;
        utils::write_file(workdir.path().join("foo.yaml"), |b| {
            let foo = r#"
packages:
  - fooinclude
"#;
            b.write_all(foo.as_bytes())?; Ok(()) })?;
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(r#"
include: foo.yaml
"#);
        let tf = new_test_treefile(workdir.path(), buf.as_str(), None)?;
        assert!(tf.parsed.packages.unwrap().len() == 4);
        Ok(())
    }

    #[test]
    fn test_treefile_arch_includes() -> Fallible<()> {
        let workdir = tempfile::tempdir()?;
        utils::write_file(workdir.path().join("foo-x86_64.yaml"), |b| {
            let foo = r#"
packages:
  - foo-x86_64-include
"#;
            b.write_all(foo.as_bytes())?; Ok(()) })?;
        let mut buf = VALID_PRELUDE.to_string();
        buf.push_str(r#"
arch-include:
    x86_64: foo-x86_64.yaml
    s390x: foo-s390x.yaml
"#);
        // Note foo-s390x.yaml doesn't exist
        let tf = new_test_treefile(workdir.path(), buf.as_str(), Some(ARCH_X86_64))?;
        assert!(tf.parsed.packages.unwrap().iter().find(|&p| p == "foo-x86_64-include").is_some());
        Ok(())
    }


    #[test]
    fn test_treefile_merge() {
        let basearch = Some(ARCH_X86_64);
        let mut base = append_and_parse(
            r###"
add-commit-metadata:
  my-first-key: "please don't override me"
  my-second-key: "override me"
etc-group-members:
  - sudo
        "###,
        );
        let mut mid_input = io::BufReader::new(
            r###"
packages:
  - some layered packages
add-commit-metadata:
  my-second-key: "something better"
  my-third-key: 1000
  my-fourth-key:
    nested: table
etc-group-members:
  - docker
"###
            .as_bytes(),
        );
        let mut mid = treefile_parse_stream(utils::InputFormat::YAML, &mut mid_input, basearch).unwrap();
        let mut top_input = io::BufReader::new(ROJIG_YAML.as_bytes());
        let mut top = treefile_parse_stream(utils::InputFormat::YAML, &mut top_input, basearch).unwrap();
        assert!(top.add_commit_metadata.is_none());
        treefile_merge(&mut mid, &mut base);
        treefile_merge(&mut top, &mut mid);
        let tf = &top;
        assert!(tf.packages.as_ref().unwrap().len() == 8);
        assert!(tf.etc_group_members.as_ref().unwrap().len() == 2);
        let rojig = tf.rojig.as_ref().unwrap();
        assert!(rojig.name == "exampleos");
        let data = tf.add_commit_metadata.as_ref().unwrap();
        assert!(data.get("my-first-key").unwrap().as_str().unwrap() == "please don't override me");
        assert!(data.get("my-second-key").unwrap().as_str().unwrap() == "something better");
        assert!(data.get("my-third-key").unwrap().as_i64().unwrap() == 1000);
        assert!(data.get("my-fourth-key").unwrap().as_object().unwrap().get("nested").unwrap().as_str().unwrap() == "table");
    }
}

mod ffi {
    use super::*;
    use glib_sys;
    use glib::translate::*;
    use libc;
    use std::io::Seek;
    use std::os::unix::io::{AsRawFd, RawFd};
    use std::{fs, io, ptr};

    use crate::ffiutil::*;

    // Some of our file descriptors may be read multiple times.
    // We try to consistently seek to the start to make that
    // convenient from the C side.  Note that this function
    // will abort if seek() fails (it really shouldn't).
    fn raw_seeked_fd(fd: &mut fs::File) -> RawFd {
        fd.seek(io::SeekFrom::Start(0)).expect("seek");
        fd.as_raw_fd()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_new(
        filename: *const libc::c_char,
        basearch: *const libc::c_char,
        workdir_dfd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> *mut Treefile {
        // Convert arguments
        let filename = ffi_view_os_str(filename);
        let basearch = ffi_view_nullable_str(basearch);
        let workdir = ffi_view_openat_dir(workdir_dfd);
        // Run code, map error if any, otherwise extract raw pointer, passing
        // ownership back to C.
        ptr_glib_error(
            Treefile::new_boxed(filename.as_ref(), basearch, workdir),
            gerror,
        )
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_dfd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf).primary_dfd.as_raw_fd()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_postprocess_script_fd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf)
            .externals
            .postprocess_script
            .as_mut()
            .map_or(-1, raw_seeked_fd)
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_add_file_fd(
        tf: *mut Treefile,
        filename: *const libc::c_char,
    ) -> libc::c_int {
        let tf = ref_from_raw_ptr(tf);
        let filename = ffi_view_os_str(filename);
        let filename = filename.to_string_lossy().into_owned();
        raw_seeked_fd(tf.externals.add_files.get_mut(&filename).expect("add-file"))
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_passwd_fd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf)
            .externals
            .passwd
            .as_mut()
            .map_or(-1, raw_seeked_fd)
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_group_fd(tf: *mut Treefile) -> libc::c_int {
        ref_from_raw_ptr(tf)
            .externals
            .group
            .as_mut()
            .map_or(-1, raw_seeked_fd)
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_json_string(tf: *mut Treefile) -> *const libc::c_char {
        ref_from_raw_ptr(tf).serialized.as_ptr()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_ostree_layers(tf: *mut Treefile) -> *mut *mut libc::c_char  {
        let tf = ref_from_raw_ptr(tf);
        if let Some(ref layers) = tf.parsed.ostree_layers {
            layers.to_glib_full()
        } else {
            ptr::null_mut()
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_ostree_override_layers(tf: *mut Treefile) -> *mut *mut libc::c_char  {
        let tf = ref_from_raw_ptr(tf);
        if let Some(ref layers) = tf.parsed.ostree_override_layers {
            layers.to_glib_full()
        } else {
            ptr::null_mut()
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_all_ostree_layers(tf: *mut Treefile) -> *mut *mut libc::c_char  {
        let tf = ref_from_raw_ptr(tf);
        let mut ret : Vec<String> = Vec::new();
        if let Some(ref layers) = tf.parsed.ostree_layers {
            ret.extend(layers.iter().cloned())
        }
        if let Some(ref layers) = tf.parsed.ostree_override_layers {
            ret.extend(layers.iter().cloned())
        }
        ret.to_glib_full()
    }


    #[no_mangle]
    pub extern "C" fn ror_treefile_get_rojig_spec_path(tf: *mut Treefile) -> *const libc::c_char {
        let tf = ref_from_raw_ptr(tf);
        if let &Some(ref rojig) = &tf.rojig_spec {
            rojig.as_ptr()
        } else {
            ptr::null_mut()
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_sysusers(tf: *mut Treefile) -> bool {
        assert!(!tf.is_null());
        let tf = unsafe { &mut *tf };
        if let Some(ref experimental) = tf.parsed.experimental {
            experimental.sysusers.unwrap_or(false)
        } else {
            false
        }
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_rojig_name(tf: *mut Treefile) -> *const libc::c_char {
        assert!(!tf.is_null());
        let tf = unsafe { &mut *tf };
        tf.rojig_name
            .as_ref()
            .map(|v| v.as_ptr())
            .unwrap_or(ptr::null_mut())
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_get_checksum(tf: *mut Treefile) -> *const libc::c_char {
        let tf = ref_from_raw_ptr(tf);
        tf.checksum.as_ptr()
    }

    #[no_mangle]
    pub extern "C" fn ror_treefile_free(tf: *mut Treefile) {
        if tf.is_null() {
            return;
        }
        unsafe {
            Box::from_raw(tf);
        }
    }

}
pub use self::ffi::*;
