Treefile
--------

A "treefile" is a made up term for a JSON-formatted specification used
as input to `rpm-ostree compose tree` to bind "set of RPMs with
configuration" to "OSTree commit".

It's recommended to keep them in git, and set up a CI system like
Jenkins to operate on them as it changes.

It supports the following parameters:

 * `ref`: string, mandatory: Holds a string which will be the name of
   the branch for the content.

 * `gpg-key` (or `gpg_key`): string, optional: Key ID for GPG signing; the
   secret key must be in the home directory of the building user.  Defaults to
   none.

 * `repos` array of strings, mandatory: Names of yum repositories to
   use, from any files that end in `.repo`, in the same directory as
   the treefile.  `rpm-ostree compose tree` does not use the system
   `/etc/yum.repos.d`, because it's common to want to compose a target
   system distinct from the one the host sytem is running.

 * `selinux`: boolean, optional: Defaults to `true`.  If `false`, then
   no SELinux labeling will be performed on the server side.

 * `boot-location` (or `boot_location`): string, optional:
    There are 2 possible values:
    * "new": A misnomer, this value is no longer "new".  Kernel data
      goes in `/usr/lib/ostree-boot` in addition to `/usr/lib/modules`.
      This is the default; use it if you have a need to care about
      upgrading from very old versions of libostree.
    * "modules": Kernel data goes just in `/usr/lib/modules`.  Use
      this for new systems, and systems that don't need to be upgraded
      from very old libostree versions.

 * `etc-group-members`: Array of strings, optional: Unix groups in this
   list will be stored in `/etc/group` instead of `/usr/lib/group`.  Use
   this option for groups for which humans should be a member.

 * `install-langs`: Array of strings, optional.  This sets the RPM
   _install_langs macro.  Set this to e.g. `["en_US", "fr_FR"]`.

 * `mutate-os-release`: String, optional.  This causes rpm-ostree to
    change the `VERSION` and `PRETTY_NAME` fields to include the ostree
    version, and adds a specific `OSTREE_VERSION` key that can be easier
    for processes to query than looking via ostree. The actual value of
    this key represents the baked string that gets substituted out for
    the final OSTree version.

 * `documentation`: boolean, optional. If this is set to false it sets the RPM
   transaction flag "nodocs" which makes yum/rpm not install files marked as
   documentation. The default is true.

 * `packages`: Array of strings, mandatory: Set of installed packages.
   comps groups are currently not supported due to walters having issues with libcomp:
   https://github.com/cgwalters/fedora-atomic-work/commit/36d18b490529fec91b74ca9b464adb73ef0ab462

 * `packages-$basearch`: Array of strings, optional: Set of installed packages, used
    only if $basearch matches the target architecture name.

 * `ostree-layers`: Array of strings, optional: After all packages are unpacked,
    check out these OSTree refs, which must already be in the destination repository.
    Any conflicts with packages will be an error.

 * `ostree-override-layers`: Array of strings, optional: Like above, but any
    files present in packages and prior layers will be silently overriden.
    This is useful for development builds to replace parts of the base tree.

 * `bootstrap_packages`: Array of strings, optional: Deprecated; you should
    now just include this set in the main `packages` array.

 * `recommends`: boolean, optional: Install `Recommends`, defaults to `true`.

 * `units`: Array of strings, optional: Systemd units to enable by default

 * `default-target` (or `default_target`): String, optional: Set the default
    systemd target.

 * `initramfs-args`: Array of strings, optional.  Passed to the
    initramfs generation program (presently `dracut`).  An example use
    case for this with Dracut is `--filesystems xfs,ext4` to ensure
    specific filesystem drivers are included.  If not specified,
    `--no-hostonly` will be used.

 * `remove-files`: Array of files to delete from the generated tree.

 * `remove-from-packages`: Array, optional: Delete from specified packages
   files which match the provided array of regular expressions.
   This is safer than `remove-files` as it allows finer grained control
   with less risk of too-wide regular expressions.

   Each array element is an array, whose first member is a package name,
   and subsequent members are regular expressions (compatible with JavaScript).

   Example: `remove-from-packages: [["cpio", "/usr/share/.*"], ["dhclient", "/usr/lib/.*", "/usr/share/.*"]]`

   Note this does not alter the RPM database, so `rpm -V` will complain.

 * `preserve-passwd`: boolean, optional: Defaults to `true`.  If enabled,
   and `check-passwd` has a type other than file, copy the `/etc/passwd` (and
   `/usr/lib/passwd`) files from the previous commit if they exist. If
   check-passwd has the file type, then the data is preserved from that file to
   `/usr/lib/passwd`.
   This helps ensure consistent uid/gid allocations across builds.  However, it
   does mean that removed users will exist in the `passwd` database forever.

 * `check-passwd`: Object, optional: Checks to run against the new passwd file
   before accepting the tree. All the entries specified should exist (unless
   ignored) and have the same values or the compose will fail. There are four
   types: none (for no checking), previous (to check against the passwd file in
   the previous commit), file (to check against another passwd file), and data
   to specify the relevant passwd data in the json itself.
   Note that if you choose file, and preserve-passwd is true then the data will
   be copied from the referenced file and not the previous commit.

   Example: `check-passwd: { "type": "none" }`
   Example: `check-passwd: { "type": "previous" }`
   Example: `check-passwd: { "type": "file", "filename": "local-passwd" }`
   Example: `check-passwd: { "type": "data", "entries": { "bin": 1, "adm": [3, 4] } }`
   See also: `ignore-remove-users`

 * `check-groups`: Object, optional: Checks to run against the new group file
   before accepting the tree. All the entries specified should exist (unless
   ignored) and have the same values or the compose will fail. There are four
   types: none (for no checking), previous (to check against the group file in
   the previous commit), file (to check against another group file), and data
   to specify the relevant group data in the json itself.
   Note that if you choose file, and preserve-passwd is true then the data will
   be copied from the referenced file and not the previous commit.

   Example: `check-groups: { "type": "none" }`
   Example: `check-groups: { "type": "previous" }`
   Example: `check-groups: { "type": "file", "filename": "local-group" }`
   Example: `check-groups: { "type": "data", "entries": { "bin": 1, "adm": 4 } }`
   See also: `ignore-remove-groups`

 * `ignore-removed-users`: Array, optional: Users to ignore if they are missing
   in the new passwd file. If an entry of `*` is specified then any user can be
   removed without failing the compose.

   Example: `ignore-removed-users: ["avahi-autoipd", "tss"]`

 * `ignore-removed-groups`: Array, optional: Groups to ignore if they are missing
   in the new group file. If an entry of `*` is specified then any group can be
   removed without failing the compose.

   Example: `ignore-removed-groups: ["avahi"]`

 * `releasever`: String, optional: Used to set the librepo `$releasever` variable,
   commonly used in yum repo files.

   Example: `releasever: "26"`

 * `automatic-version-prefix` (or `automatic_version_prefix`): String, optional:
   Set the prefix for versions on the commits. The idea is that if the previous
   commit on the branch to the doesn't match the prefix, or doesn't have a
   version, then the new commit will have the version as specified. If the
   prefix matches exactly, then we append ".1". Otherwise we parse the number
   after the prefix and increment it by one and then append that to the prefix.

   A current date/time may also be passed through `automatic-version-prefix`,
   by including a date tag in the prefix as such: `<date:format>`, where
   `format` is a string with date formats such as `%Y` (year), `%m` (month), etc.
   The full list of supported formats is [found in the GLib API](https://developer.gnome.org/glib/stable/glib-GDateTime.html#g-date-time-format).
   Including a date/time format will automatically append a `.0` to
   the version, if not present in the prefix, which resets to `.0` if
   the date (or prefix) changes.

   This means that on an empty branch with an `automatic-version-prefix`
   of `"22"` the first three commits would get the versions: "22", "22.1",
   "22.2". Some example progressions are shown:

   | `automatic-version-prefix` | version progression                        |
   | -------------------------- | ------------------------------------------ |
   | `22`                       | 22, 22.1, 22.2, ...                        |
   | `22.1`                     | 22.1.1, 22.1.2, 22.1.3, ...                |
   | `22.<date:%Y>`             | 22.2019.0, 22.2019.1, 22.2020.0, ...       |
   | `22.<date:%Y>.1`           | 22.2019.1.0, 22.2019.1.1, 22.2020.1.0, ... |

   Example: `automatic-version-prefix: "22.0"`

 * `add-commit-metadata`: Map<String, Object>, optional: Metadata to inject as
   part of composed commits. Keys inserted here can still be overridden at the
   command line with `--add-metadata-string` or `--add-metadata-from-json`.

 * `postprocess-script`: String, optional: Full filesystem path to a script
   that will be executed in the context of the target tree.  The script
   will be copied into the target into `/tmp`, and run as a container
   (a restricted chroot, with no network access).  After execution is
   complete, it will be deleted.

   It is *strongly recommended* to avoid using this except as a last resort.
   Having the system generated through RPMs allows administrators to understand
   the inputs to the system.  Any new files created through this mechanism will
   not have the versioning inherent in RPM.

   Only the script file will be copied in; thus if it has any dependencies,
   on data beyond what is in the target tree, you must embed them in the binary
   itself.

   An example use for this is working around bugs in the input RPMs that are
   hard to fix in stable releases.

   Note this does not alter the RPM database, so `rpm -V` will complain.

   If you want to depend on network access, or tools not in the target host,
   you can use the split-up `rpm-ostree compose install`
   and `rpm-ostree compose postprocess/commit` commands.

 * `postprocess`: array of string, optional: This is an *inline* script
   variant of `postprocess-script` that is also an array, so it works
   correctly with inheritance.  If both `postprocess-script` and `postprocess`
   are provided, then `postprocess-script` will be executed after all
   other `postprocess`.

 * `include`: string or array of string, optional: Path(s) to treefiles which will be
   used as an inheritance base.  The semantics for inheritance are:
   Non-array values in child values override parent values.  Array
   values are concatenated.  Filenames will be resolved relative to
   the including treefile.  Since rpm-ostree 2019.5, this value may
   also be an array of strings.  Including the same file multiple times
   is an error.

 * `arch-include`: object (`Map<String,include>`), optional: Each member of this
   object should be the name of a base architecture (`$basearch`), and the `include` value
   functions the same as the `include` key above - it can be either
   a single string, or an array of strings - and it has the same semantics.
   Entries which match `arch-include` are processed after `include`.

   Example (in YAML):

   ```yaml
   arch-include:
     x86_64: bootloader-x86_64.yaml
     s390x:
       - bootloader-s390x.yaml
       - tweaks-s390x.yaml
    ```

 * `container`: boolean, optional: Defaults to `false`.  If `true`, then
   rpm-ostree will not do any special handling of kernel, initrd or the
   /boot directory. This is useful if the target for the tree is some kind
   of container which does not have its own kernel.

 * `add-files`: Array, optional: Copy external files to the rootfs.

   Each array element is an array, whose first member is the source
   file name, and the second element is the destination name.  The
   source file must be in the same directory as the treefile.

   Example: `"add-files": [["bar", "/usr/share/bar"], ["foo", "/lib/foo"]]`

   Note that in the OSTree model, not all directories are managed by OSTree. In
   short, only files in `/usr` (or UsrMove symlinks into `/usr`) and `/etc` are
   supported. For more details, see the OSTree manual:
   https://ostree.readthedocs.io/en/latest/manual/deployment/

 * `tmp-is-dir`: boolean, optional: Defaults to `false`.  By default,
   rpm-ostree creates symlink `/tmp` → `sysroot/tmp`.  When set to `true`,
   `/tmp` will be a regular directory, which allows the `systemd` unit
   `tmp.mount` to mount it as `tmpfs`. It's more flexible to leave it
   as a directory, and further, we don't want to encourage `/sysroot`
   to be writable. For host system composes, we recommend turning this on;
   it's left off by default to ease the transition.

 * `machineid-compat`: boolean, optional: Defaults to `true`.  By default,
   rpm-ostree creates `/usr/etc/machine-id` as an empty file for historical
   reasons.  Set this to `false` to ensure it's not present at all.  This
   will cause systemd to execute `ConditionFirstBoot=`, which implies
   running `systemctl preset-all` for example.  This requires booting the system
   with `rw` so that systemd can properly populate `/etc/machine-id` and execute
   the presets at switchroot.  When this is enabled, the `units`
   directive will no longer function.  Instead, create a
   `/usr/lib/systemd/system-presets/XX-example.preset` file as part of a package
   or in the postprocess script.

Experimental options
--------

All options listed here are subject to change or removal in a future
version of `rpm-ostree`.

 * `rojig`: Object, optional.  Sub-keys are `name`, `summary`, `license`,
   and `description`.  Of those, `name` and `license` are mandatory.

 * `sysusers`: boolean, optional: Defaults to `false`. Enable generation of
   systemd sysusers.d entries based on `useradd`/`gruopadd` invocations.  If enabled,
   this overrides `preserve-passwd`.  It also obsoletes `check-passwd`.
 
 * `sysusers-users`: List of `string` -> `number` mappings.  When sysusers is
   enabled, it is required that any non-root-owned files that are in `/usr`
   (and hence shipped with the ostree commit) have static mappings.  Otherwise,
   the values could change across builds.

 * `sysusers-groups`: Same as `sysusers-users` but for groups.
