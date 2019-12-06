/*
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

use clap::{App, Arg};
use failure::{Fallible, ResultExt, format_err, bail};
use libc;
use std::borrow::Cow;
use std::collections::BTreeMap;
use std::io::prelude::*;
use std::path::Path;
use std::{collections, fs, io, path};

use openat_ext::OpenatDirExt;
use crate::treefile::*;

static SYSUSERS_DIR: &str = "usr/lib/sysusers.d";
static SYSUSERS_AUTO_NAME: &str = "rpmostree-auto.conf";
static SYSUSERS_OVERRIDES_NAME: &str = "rpmostree-overrides.conf";

/// The method to use to derive a user or group ID.
#[derive(PartialEq, Eq, Debug, Hash, Clone)]
enum IdSpecification {
    Unspecified,
    Specified(u32),
    Path(String),
}

impl IdSpecification {
    fn parse(buf: &str) -> Fallible<Self> {
        if buf.starts_with("/") {
            Ok(IdSpecification::Path(buf.to_string()))
        } else if buf == "-" {
            Ok(IdSpecification::Unspecified)
        } else {
            Ok(IdSpecification::Specified(buf.parse::<u32>()?))
        }
    }

    fn format_sysusers<'a>(&'a self) -> Cow<'a, str> {
        match self {
            IdSpecification::Unspecified => Cow::Borrowed("-"),
            IdSpecification::Specified(u) => Cow::Owned(format!("{}", u)),
            IdSpecification::Path(ref s) => Cow::Borrowed(s),
        }
    }
}

/// A generic name ↔ id pair used for both users and groups
#[derive(PartialEq, Eq, Debug, Hash, Clone)]
struct PartialEntryValue {
    raw: String,
    name: String,
    id: IdSpecification,
    rest: String,
}

/// A partial parse of a sysusers.d entry.
////
/// For now, we don't parse all of the entry data; we'd need to handle quoting
/// etc. See extract-word.c in the systemd source. All we need is the
/// user/group → {u,g}id mapping to ensure we're not creating duplicate entries.
/// Group members we just pass through
#[derive(PartialEq, Eq, Debug, Hash, Clone)]
enum PartialSysuserEntry {
    User(PartialEntryValue),
    Group(PartialEntryValue),
    GroupMember(String),
    Range(String),
}

/// Full user information; currently this is what we get from e.g. `useradd`.
#[derive(PartialEq, Eq, Debug, Hash, Clone)]
enum SysuserEntry {
    User {
        name: String,
        uid: IdSpecification,
        gecos: Option<String>,
        homedir: Option<String>,
        shell: Option<String>,
    },
    Group {
        name: String,
        gid: IdSpecification,
    },
    GroupMember {
        uname: String,
        gname: String,
    },
}

impl SysuserEntry {
    fn format_sysusers(&self) -> String {
        match self {
            SysuserEntry::User {
                name,
                uid,
                gecos,
                homedir,
                shell,
            } => {
                fn optional_quoted_string<'a>(s: &'a Option<String>) -> Cow<'a, str> {
                    match s.as_ref() {
                        Some(s) => {
                            let mut elts = s.split_whitespace();
                            let first = elts.next();
                            if first.is_none() {
                                return Cow::Borrowed("-");
                            }
                            match elts.next() {
                                Some(_) => Cow::Owned(format!("\"{}\"", s)),
                                None => Cow::Borrowed(s),
                            }
                        }
                        None => Cow::Borrowed("-"),
                    }
                }
                format!(
                    "u {} {} {} {} {}",
                    name,
                    uid.format_sysusers(),
                    optional_quoted_string(&gecos),
                    optional_quoted_string(&homedir),
                    optional_quoted_string(&shell),
                )
            }
            SysuserEntry::Group { name, gid } => format!("g {} {}", name, gid.format_sysusers()),
            SysuserEntry::GroupMember { uname, gname } => format!("m {} {}", uname, gname),
        }
    }
}

/// (Partially) parse a single line from a sysusers.d file
fn parse_entry(line: &str) -> Fallible<PartialSysuserEntry> {
    let err = || format_err!("Invalid sysusers entry: \"{}\"", line);
    let raw = line.to_string();
    // Values of groupmember and range we don't parse.
    if line.starts_with('m') {
        return Ok(PartialSysuserEntry::GroupMember(raw));
    }
    if line.starts_with('r') {
        return Ok(PartialSysuserEntry::Range(raw));
    }
    // We only parse out the type, name, and id.
    let elts: Vec<&str> = line.split_whitespace().collect();
    if elts.len() < 3 {
        return Err(err());
    }
    let name = elts[1].to_string();
    let id = IdSpecification::parse(elts[2])?;
    let rest = if elts.len() > 3 {
        elts[3..].join(" ")
    } else {
        "".to_string()
    };
    match elts[0] {
        "u" => Ok(PartialSysuserEntry::User(PartialEntryValue {
            raw,
            name,
            id,
            rest,
        })),
        "g" => Ok(PartialSysuserEntry::Group(PartialEntryValue {
            raw,
            name,
            id,
            rest,
        })),
        "m" => {
            // Handled above
            unreachable!();
        }
        _ => Err(err()),
    }
}

/// Parse a sysusers.d file (as a stream)
fn parse_sysusers_stream<I: io::BufRead>(stream: I) -> Fallible<Vec<PartialSysuserEntry>> {
    let mut res = Vec::new();
    for line in stream.lines() {
        let line = line?;
        if line.starts_with("#") || line.is_empty() {
            continue;
        }
        res.push(parse_entry(&line)?);
    }
    Ok(res)
}

/// Implementation of useradd
fn useradd<'a, I>(args: I) -> Fallible<Vec<SysuserEntry>>
where
    I: IntoIterator<Item = &'a str>,
{
    let app = App::new("useradd")
        .version("0.1")
        .about("rpm-ostree useradd wrapper")
        .arg(Arg::with_name("system").short("r"))
        .arg(Arg::with_name("uid").short("u").takes_value(true))
        .arg(Arg::with_name("gid").short("g").takes_value(true))
        .arg(Arg::with_name("no-log-init").short("l"))
        .arg(Arg::with_name("no-create-home").short("M"))
        .arg(Arg::with_name("no-unique").short("o"))
        .arg(Arg::with_name("groups").short("G").takes_value(true))
        .arg(Arg::with_name("home-dir").short("d").takes_value(true))
        .arg(Arg::with_name("comment").short("c").takes_value(true))
        .arg(Arg::with_name("shell").short("s").takes_value(true))
        .arg(Arg::with_name("username").takes_value(true).required(true));
    let matches = app.get_matches_from_safe(args)?;

    if !matches.is_present("system") {
        bail!("--system argument required");
    }

    let mut uid = IdSpecification::Unspecified;
    if let Some(ref uidstr) = matches.value_of("uid") {
        uid = IdSpecification::Specified(uidstr.parse::<u32>()?);
    }
    let name = matches.value_of("username").unwrap().to_string();
    let gecos = matches.value_of("comment").map(|s| s.to_string());
    let homedir = matches.value_of("home-dir").map(|s| s.to_string());
    let shell = matches.value_of("shell").map(|s| s.to_string());

    let mut res = vec![SysuserEntry::User {
        name: name.to_string(),
        uid,
        gecos,
        homedir,
        shell,
    }];
    if let Some(primary_group) = matches.value_of("gid") {
        let is_numeric_gid = primary_group.parse::<u32>().is_ok();
        if !is_numeric_gid && primary_group != name {
            bail!(
                "Unable to represent user with group '{}' != username '{}'",
                primary_group,
                name
            );
        }
    }
    if let Some(gnames) = matches.value_of("groups") {
        for gname in gnames.split(",").filter(|&n| n != name) {
            res.push(SysuserEntry::GroupMember {
                uname: name.to_string(),
                gname: gname.to_string(),
            });
        }
    }
    Ok(res)
}

/// Implementation of groupadd
fn groupadd<'a, I>(args: I) -> Fallible<SysuserEntry>
where
    I: IntoIterator<Item = &'a str>,
{
    let app = App::new("groupadd")
        .version("0.1")
        .about("rpm-ostree groupadd wrapper")
        .arg(Arg::with_name("system").short("r"))
        .arg(Arg::with_name("force").short("f"))
        .arg(Arg::with_name("gid").short("g").takes_value(true))
        .arg(Arg::with_name("groupname").takes_value(true).required(true));
    let matches = app.get_matches_from_safe(args)?;

    if !matches.is_present("system") {
        bail!("--system argument required");
    }

    let mut gid = IdSpecification::Unspecified;
    if let Some(ref gidstr) = matches.value_of("gid") {
        gid = IdSpecification::Specified(gidstr.parse::<u32>()?);
    }
    let name = matches.value_of("groupname").unwrap().to_string();
    Ok(SysuserEntry::Group { name, gid })
}

/// Main entrypoint for useradd
fn useradd_main<W: Write>(sysusers_file: &mut W, args: &Vec<&str>) -> Fallible<()> {
    let r = useradd(args.iter().map(|x| *x))?;
    for elt in r {
        writeln!(sysusers_file, "{}", elt.format_sysusers())?;
    }
    Ok(())
}

/// Main entrypoint for groupadd
fn groupadd_main<W: Write>(sysusers_file: &mut W, args: &Vec<&str>) -> Fallible<()> {
    let r = groupadd(args.iter().map(|x| *x))?;
    writeln!(sysusers_file, "{}", r.format_sysusers())?;
    Ok(())
}

/// Called from rpmostree-scripts.c to implement a special API to extract
/// `useradd/groupadd` invocations from scripts.  Rather than adding to
/// the passwd/group files directly, we generate a sysusers.d entry.
///
/// This function takes as input a file descriptor for an O_TMPFILE fd that has
/// newline-separated arguments.  Note that we can get multiple invocations of
/// useradd/groupadd in a single call.
fn process_useradd_invocation(rootfs: openat::Dir, mut argf: fs::File) -> Fallible<()> {
    argf.seek(io::SeekFrom::Start(0))?;
    let argf = io::BufReader::new(argf);
    let mut lines = argf.lines();
    let sysusers_dir = path::Path::new(SYSUSERS_DIR);
    let autopath = sysusers_dir.join(SYSUSERS_AUTO_NAME);
    let mut sysusers_autof = None;
    loop {
        let mode = match (&mut lines).next() {
            Some(mode) => mode?,
            None => break,
        };
        let mode = mode.as_str();
        if mode == "" {
            break;
        }
        let mut args = vec![mode.to_string()];
        for line in &mut lines {
            let line = line?;
            // Empty arg terminates an invocation
            if line == "" {
                break;
            }
            args.push(line);
        }
        let args: Vec<&str> = args.iter().map(|v| v.as_str()).collect();
        if sysusers_autof.is_none() {
            let f = rootfs.append_file(&autopath, 0644)?;
            sysusers_autof = Some(io::BufWriter::new(f));
        }
        match mode {
            "useradd" => useradd_main(&mut sysusers_autof.as_mut().unwrap(), &args)?,
            "groupadd" => groupadd_main(&mut sysusers_autof.as_mut().unwrap(), &args)?,
            x => bail!("Unknown command: {}", x),
        };
    }
    if let Some(ref mut autof) = sysusers_autof {
        autof.flush()?;
    }
    Ok(())
}

#[derive(Default)]
struct IndexedSysusers {
    users: BTreeMap<String, PartialEntryValue>,
    groups: BTreeMap<String, PartialEntryValue>,
    members: collections::HashSet<String>,
}

impl IndexedSysusers {
    fn new() -> Self {
        Default::default()
    }
}

#[derive(Default)]
struct IdIndex {
    uids: BTreeMap<u32, String>,
    gids: BTreeMap<u32, String>,
}

impl IdIndex {
    fn new(sysusers: &IndexedSysusers) -> Self {
        let mut uids = BTreeMap::new();
        let mut gids = BTreeMap::new();

        for (name, value) in &sysusers.users {
            if let IdSpecification::Specified(uid) = value.id {
                uids.insert(uid, name.clone());
                gids.insert(uid, name.clone());
            }
        }
        for (name, value) in &sysusers.groups {
            if let IdSpecification::Specified(gid) = value.id {
                gids.insert(gid, name.clone());
            }
        }

        Self { uids, gids }
    }
}

/// Insert into `indexed` the values of name -> ID mapping from the sysusers file.
fn parse_sysusers_indexed<I: io::BufRead>(f: I, indexed: &mut IndexedSysusers) -> Fallible<()> {
    let mut entries = parse_sysusers_stream(f)?;
    for entry in entries.drain(..) {
        match entry {
            PartialSysuserEntry::User(u) => {
                indexed.users.insert(u.name.clone(), u.clone());
                indexed.groups.insert(u.name.clone(), u);
            }
            PartialSysuserEntry::Group(g) => {
                indexed.groups.insert(g.name.clone(), g);
            }
            PartialSysuserEntry::GroupMember(raw) => {
                indexed.members.insert(raw);
            }
            PartialSysuserEntry::Range(_) => {}
        }
    }
    Ok(())
}

/// Determine if path `p` is owned by a non-root user or group that isn't in `index`.
fn analyze_non_root_owned_file<P: AsRef<path::Path>>(
    p: P,
    meta: &libc::stat,
    index: &IdIndex,
) -> Option<String> {
    let p = p.as_ref();
    if meta.st_uid != 0 {
        if !index.uids.contains_key(&meta.st_uid) {
            return Some(format!(
                "sysusers: No static entry for owner {} of {:?}",
                meta.st_uid, p
            ));
        }
    }
    if meta.st_gid != 0 {
        if !index.gids.contains_key(&meta.st_gid) {
            return Some(format!(
                "sysusers: No static entry for group {} of {:?}",
                meta.st_gid, p
            ));
        }
    }
    None
}

/// Recurse over directory `dfd`, gathering all files owned by non-static uid/gids.
fn find_nonstatic_ownership_recurse<P: AsRef<path::Path>>(
    dfd: &openat::Dir,
    p: P,
    index: &IdIndex,
    result: &mut Vec<String>,
) -> Fallible<()> {
    let p = p.as_ref();
    for child in dfd.list_dir(p)? {
        let child = child?;
        let childp = p.join(child.file_name());
        let meta = dfd.metadata(&childp)?;
        let stat = meta.stat();
        if stat.st_uid != 0 || stat.st_gid != 0 {
            if let Some(err) = analyze_non_root_owned_file(&childp, stat, index) {
                result.push(err);
            }
        }
        if meta.simple_type() == openat::SimpleType::Dir {
            find_nonstatic_ownership_recurse(dfd, &childp, index, result)?;
        }
    }
    Ok(())
}

/// Given a root directory and a sysusers database, find
/// any files owned by a uid that wasn't allocated statically.
fn find_nonstatic_ownership(root: &openat::Dir, users: &IndexedSysusers) -> Fallible<()> {
    let index = IdIndex::new(users);
    let mut errs = Vec::new();
    find_nonstatic_ownership_recurse(root, ".", &index, &mut errs)?;
    if errs.is_empty() {
        return Ok(());
    }
    for err in errs {
        eprintln!("{}", err);
    }
    bail!("Non-static uid/gid assignments for files found")
}

/// Iterate over any sysusers files from RPMs (not created by us).
fn index_sysusers(sysusers_dir: &openat::Dir, skip_self: bool) -> Fallible<IndexedSysusers> {
    let mut indexed = IndexedSysusers::new();
    // Load and index sysusers.d entries
    for child in sysusers_dir.list_dir(".")? {
        let child = child?;
        let name = child.file_name();
        if sysusers_dir.get_file_type(&child)? != openat::SimpleType::File {
            continue;
        };
        if skip_self && (name == SYSUSERS_AUTO_NAME || name == SYSUSERS_OVERRIDES_NAME) {
            continue;
        }
        let f = sysusers_dir.open_file(name)?;
        let mut f = io::BufReader::new(f);
        parse_sysusers_indexed(&mut f, &mut indexed)?;
    }
    Ok(indexed)
}

/// Replace just the id specification component of a raw sysusers.d string.
/// This is a hack since we don't have a proper parser that handles quoting; if
/// we did we could round trip.
fn replace_id_specification(partial: &PartialSysuserEntry, new_id: u32) -> String {
    match partial {
        PartialSysuserEntry::User(u) => format!("u {} {} {}", u.name, new_id, u.rest),
        PartialSysuserEntry::Group(g) => format!("g {} {} {}", g.name, new_id, g.rest),
        PartialSysuserEntry::GroupMember(raw) => raw.clone(),
        PartialSysuserEntry::Range(raw) => raw.clone(),
    }
}

type SysusersOverrides = BTreeMap<String, u32>;

/// If the given sysusers.d `entry` has an override from the treefile
/// to set a static uid/gid, replace it.
fn replace_id_with_override<W>(
    entry: &PartialSysuserEntry,
    required_overrides: &mut collections::HashSet<&str>,
    overrides: &SysusersOverrides,
    out: &mut W,
) -> Fallible<()>
where
    W: io::Write,
{
    let (name, raw) = match entry {
        PartialSysuserEntry::User(u) => (u.name.as_str(), u.raw.as_str()),
        PartialSysuserEntry::Group(g) => (g.name.as_str(), g.raw.as_str()),
        _ => unreachable!(),
    };
    if required_overrides.remove(name) {
        let id = overrides.get(name).unwrap();
        let rewritten = replace_id_specification(entry, *id);
        out.write(rewritten.as_bytes())?;
    } else {
        out.write(raw.as_bytes())?;
    }
    Ok(())
}

/// Given a sysusers.d file `fname`, use the provided overrides where applicable.
fn rewrite_sysuser_file(
    sysusers_dir: &openat::Dir,
    fname: &str,
    required_user_overrides: &mut collections::HashSet<&str>,
    required_group_overrides: &mut collections::HashSet<&str>,
    user_overrides: &SysusersOverrides,
    group_overrides: &SysusersOverrides,
) -> Fallible<()> {
    let inf = sysusers_dir.open_file(fname)?;
    let inf = io::BufReader::new(inf);
    let tmpf_path = Path::new(fname).with_file_name(format!("{}.tmp", fname));
    let tmpf = sysusers_dir.write_file(&tmpf_path, 0644)?;
    let mut f = io::BufWriter::new(tmpf);

    for line in inf.lines() {
        let line = line?;
        if line.starts_with("#") || line.is_empty() {
            f.write(line.as_bytes())?;
        } else {
            let entry = parse_entry(&line)?;
            match &entry {
                PartialSysuserEntry::User(_) => {
                    replace_id_with_override(
                        &entry,
                        required_user_overrides,
                        user_overrides,
                        &mut f,
                    )?;
                }
                PartialSysuserEntry::Group(_) => {
                    replace_id_with_override(
                        &entry,
                        required_group_overrides,
                        group_overrides,
                        &mut f,
                    )?;
                }
                PartialSysuserEntry::GroupMember(raw) => {
                    f.write(raw.as_bytes())?;
                }
                PartialSysuserEntry::Range(raw) => {
                    f.write(raw.as_bytes())?;
                }
            }
        }
        f.write(b"\n")?;
    }

    f.flush()?;
    sysusers_dir.local_rename(&tmpf_path, fname)?;

    Ok(())
}

/// Loop over the sysusers.d entries, rewriting them to use any fixed ids that were
/// specified in the treefile.
fn rewrite_sysusers_with_overrides(
    sysusers_dir: &openat::Dir,
    user_overrides: &SysusersOverrides,
    group_overrides: &SysusersOverrides,
) -> Fallible<()> {
    fn getkeys(overrides: &SysusersOverrides) -> collections::HashSet<&str> {
        overrides.keys().map(|v| v.as_str()).collect()
    }
    let mut required_user_overrides = getkeys(user_overrides);
    let mut required_group_overrides = getkeys(group_overrides);

    for child in sysusers_dir.list_dir(".")? {
        if let Some(ref name) = child?.file_name().to_str() {
            if !name.ends_with(".conf") {
                continue;
            }

            rewrite_sysuser_file(
                &sysusers_dir,
                name,
                &mut required_user_overrides,
                &mut required_group_overrides,
                user_overrides,
                group_overrides,
            )
            .with_context(|e| format!("Rewriting {}: {}", name, e))?;
        }
    }

    fn check_overrides(isgroup: bool, required: collections::HashSet<&str>) -> Fallible<()> {
        if !required.is_empty() {
            eprintln!(
                "Failed to find sysusers.d entries for {} overrides:",
                if isgroup { "group" } else { "user" }
            );

            for k in required {
                eprintln!("  {}", k);
            }
            bail!("Some sysusers.d entries not found");
        }
        Ok(())
    }
    check_overrides(false, required_user_overrides)?;
    check_overrides(true, required_group_overrides)?;

    Ok(())
}

/// Called after the RPM %pre scripts have run, which are
/// the only ones which should be adding users.
///
/// We then loop over the sysusers.d directory, creating two
/// indexed versions; one of all the sysusers.d entries
/// we didn't create, and one with ours.  We want
/// to remove our duplicates.
pub fn post_useradd(rootfs: &openat::Dir, tf: &Treefile) -> Fallible<()> {
    let sysusers_dirpath = path::Path::new(SYSUSERS_DIR);
    let sysusers_dir = rootfs.sub_dir(sysusers_dirpath)?;

    let mut other_indexed = index_sysusers(&sysusers_dir, true)?;

    // Load our auto-generated sysusers.d entries
    let mut my_entries = if let Some(f) = sysusers_dir.open_file_optional(SYSUSERS_AUTO_NAME)? {
        let mut f = io::BufReader::new(f);
        parse_sysusers_stream(&mut f)?
    } else {
        let mut f = io::BufReader::new("".as_bytes());
        parse_sysusers_stream(&mut f)?
    };

    // Rewrite our sysusers.d file, dropping any entries
    // which duplicate ones defined by the system.
    let f = sysusers_dir.write_file(SYSUSERS_AUTO_NAME, 0644)?;
    let mut f = io::BufWriter::new(f);
    f.write(
        b"# This file was automatically generated by rpm-ostree\n\
              # from intercepted invocations of /usr/sbin/useradd and groupadd.\n",
    )?;
    for entry in my_entries.drain(..) {
        match entry {
            PartialSysuserEntry::User(v) => {
                if !other_indexed.users.contains_key(&v.name) {
                    f.write(v.raw.as_bytes())?;
                    f.write(b"\n")?;
                }
            }
            PartialSysuserEntry::Group(v) => {
                if !other_indexed.groups.contains_key(&v.name) {
                    f.write(v.raw.as_bytes())?;
                    f.write(b"\n")?;
                    other_indexed.groups.insert(v.name.clone(), v);
                }
            }
            PartialSysuserEntry::GroupMember(g) => {
                f.write(g.as_bytes())?;
                f.write(b"\n")?;
                other_indexed.members.insert(g);
            }
            PartialSysuserEntry::Range(r) => {
                f.write(r.as_bytes())?;
                f.write(b"\n")?;
            }
        };
    }
    f.flush()?;

    let empty_overrides = BTreeMap::new();
    let (user_overrides, group_overrides) = if let Some(ref experimental) = tf.parsed.experimental {
        (
            experimental
                .sysusers_users
                .as_ref()
                .unwrap_or(&empty_overrides),
            experimental
                .sysusers_groups
                .as_ref()
                .unwrap_or(&empty_overrides),
        )
    } else {
        (&empty_overrides, &empty_overrides)
    };
    if !user_overrides.is_empty() || !group_overrides.is_empty() {
        rewrite_sysusers_with_overrides(&sysusers_dir, user_overrides, group_overrides)?;
    }
    Ok(())
}

/// Replace file contents (using `openat(2)`).  Not currently atomic.
pub fn replace_file_contents_at<P: AsRef<Path>>(
    dfd: &openat::Dir,
    path: P,
    contents: &[u8],
) -> Fallible<()> {
    let path = path.as_ref();
    dfd.remove_file(path)?;
    let mut f = dfd.new_file(path, 0644)?;
    f.write(contents)?;
    f.flush()?;
    Ok(())
}

/// Clear out /etc/passwd and /etc/group now; they should
/// always be populated by the sysusers entries.
pub fn final_postprocess(rootfs: &openat::Dir) -> Fallible<()> {
    let sysusers_dirpath = path::Path::new(SYSUSERS_DIR);
    let sysusers_dir = rootfs.sub_dir(sysusers_dirpath)?;
    let indexed = index_sysusers(&sysusers_dir, false)?;

    find_nonstatic_ownership(&rootfs, &indexed)?;

    replace_file_contents_at(
        rootfs,
        "usr/etc/passwd",
        b"root:x:0:0:root:/var/roothome:/bin/bash\n",
    )?;
    replace_file_contents_at(rootfs, "usr/etc/shadow", b"root:!locked::0:99999:7:::\n")?;
    replace_file_contents_at(rootfs, "usr/etc/group", b"root:x:0:\n")?;
    replace_file_contents_at(rootfs, "usr/etc/gshadow", b"root:::\n")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // from "man sysusers.d"
    static SYSUSERS1: &str = r###"
u     httpd    404            "HTTP User"
u     authd    /usr/bin/authd "Authorization user"
u     postgres -              "Postgresql Database" /var/lib/pgsql /usr/libexec/postgresdb
g     input    -              -
m     authd    input
u     root     0              "Superuser"           /root          /bin/zsh
"###;

    #[test]
    fn test_parse() {
        let make_buf = || io::BufReader::new(SYSUSERS1.as_bytes());
        let lines: Vec<String> = make_buf()
            .lines()
            .filter_map(|line| {
                let line = line.unwrap();
                if !line.is_empty() {
                    Some(line.to_string())
                } else {
                    None
                }
            })
            .collect();
        let buf = make_buf();
        let r = parse_sysusers_stream(buf).unwrap();
        assert_eq!(
            r[0],
            PartialSysuserEntry::User(PartialEntryValue {
                raw: lines[0].clone(),
                name: "httpd".to_string(),
                id: IdSpecification::Specified(404),
                rest: r#""HTTP User""#.to_string(),
            })
        );
        assert_eq!(
            r[1],
            PartialSysuserEntry::User(PartialEntryValue {
                raw: lines[1].clone(),
                name: "authd".to_string(),
                id: IdSpecification::Path("/usr/bin/authd".to_string()),
                rest: r#""Authorization user""#.to_string(),
            })
        );
        assert_eq!(
            r[2],
            PartialSysuserEntry::User(PartialEntryValue {
                raw: lines[2].clone(),
                name: "postgres".to_string(),
                id: IdSpecification::Unspecified,
                rest: r#""Postgresql Database" /var/lib/pgsql /usr/libexec/postgresdb"#.to_string(),
            })
        );
        assert_eq!(
            r[3],
            PartialSysuserEntry::Group(PartialEntryValue {
                raw: lines[3].clone(),
                name: "input".to_string(),
                id: IdSpecification::Unspecified,
                rest: "-".to_string(),
            })
        );
        assert_eq!(r[4], PartialSysuserEntry::GroupMember(lines[4].clone()));
        assert_eq!(
            r[5],
            PartialSysuserEntry::User(PartialEntryValue {
                raw: lines[5].clone(),
                name: "root".to_string(),
                id: IdSpecification::Specified(0),
                rest: r#""Superuser" /root /bin/zsh"#.to_string(),
            })
        );
    }

    #[test]
    fn test_useradd_wesnoth() {
        let r = useradd(
            vec![
                "useradd",
                "-c",
                "Wesnoth server",
                "-s",
                "/sbin/nologin",
                "-r",
                "-d",
                "/var/run/wesnothd",
                "wesnothd",
            ]
            .iter()
            .map(|v| *v),
        )
        .unwrap();
        assert_eq!(
            r,
            vec![SysuserEntry::User {
                name: "wesnothd".to_string(),
                uid: IdSpecification::Unspecified,
                gecos: Some("Wesnoth server".to_string()),
                shell: Some("/sbin/nologin".to_string()),
                homedir: Some("/var/run/wesnothd".to_string()),
            }]
        );
        assert_eq!(r.len(), 1);
        assert_eq!(
            r[0].format_sysusers(),
            r##"u wesnothd - "Wesnoth server" /var/run/wesnothd /sbin/nologin"##
        );
    }

    #[test]
    fn test_useradd_tss() {
        let r = useradd(
            vec![
                "useradd",
                "-r",
                "-u",
                "59",
                "-g",
                "tss",
                "-d",
                "/dev/null",
                "-s",
                "/sbin/nologin",
                "-c",
                "comment",
                "tss",
            ]
            .iter()
            .map(|v| *v),
        )
        .unwrap();
        assert_eq!(r.len(), 1);
        assert_eq!(
            r[0].format_sysusers(),
            r##"u tss 59 comment /dev/null /sbin/nologin"##
        );
    }

    #[test]
    fn test_groupadd_basics() {
        assert_eq!(
            groupadd(vec!["groupadd", "-r", "wireshark",].iter().map(|v| *v)).unwrap(),
            SysuserEntry::Group {
                name: "wireshark".to_string(),
                gid: IdSpecification::Unspecified,
            },
        );
        assert_eq!(
            groupadd(
                vec!["groupadd", "-r", "-g", "112", "vhostmd",]
                    .iter()
                    .map(|v| *v)
            )
            .unwrap(),
            SysuserEntry::Group {
                name: "vhostmd".to_string(),
                gid: IdSpecification::Specified(112),
            },
        );
    }

    #[test]
    fn test_replace_id_specification() -> Fallible<()> {
        let entry =
            parse_entry(r##"u wesnothd - "Wesnoth server" /var/run/wesnothd /sbin/nologin"##)?;
        let replaced = replace_id_specification(&entry, 42);
        match parse_entry(&replaced)? {
            PartialSysuserEntry::User(PartialEntryValue { name, id, .. }) => {
                assert_eq!(name, "wesnothd");
                assert_eq!(id, IdSpecification::Specified(42));
            }
            _ => unreachable!(),
        };
        Ok(())
    }
}

mod ffi {
    use super::*;
    use crate::ffiutil::*;
    use libc;
    use std::os::unix::io::FromRawFd;

    /// See process_useradd_invocation() above.
    #[no_mangle]
    pub extern "C" fn ror_sysusers_process_useradd_invocations(
        rootfs_dfd: libc::c_int,
        useradd_fd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        // Ownership is transferred
        let useradd_fd = unsafe { fs::File::from_raw_fd(useradd_fd) };
        int_glib_error(process_useradd_invocation(rootfs_dfd, useradd_fd), gerror)
    }

    /// See post_useradd() above.
    #[no_mangle]
    pub extern "C" fn ror_sysusers_post_useradd(
        rootfs_dfd: libc::c_int,
        tf: *mut Treefile,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        assert!(!tf.is_null());
        let tf = unsafe { &mut *tf };
        int_glib_error(
            post_useradd(&rootfs_dfd, tf).with_context(|e| format!("sysusers post-pre: {}", e)),
            gerror,
        )
    }

    /// See final_postprocess() above.
    #[no_mangle]
    pub extern "C" fn ror_sysusers_final_postprocess(
        rootfs_dfd: libc::c_int,
        gerror: *mut *mut glib_sys::GError,
    ) -> libc::c_int {
        let rootfs_dfd = ffi_view_openat_dir(rootfs_dfd);
        int_glib_error(
            final_postprocess(&rootfs_dfd).with_context(|e| format!("sysusers final post: {}", e)),
            gerror,
        )
    }
}
pub use self::ffi::*;
