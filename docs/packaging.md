# Packaging

Mimir is packaged for Debian trixie, on x86-64 and arm64, as `mimir` (the
`mimir-node` binary, its systemd unit, its settings and its manual page) and
`mimir-dbgsym`. Mimir ships no library: libmimir is built static into
`/usr/bin/mimir-node`. Dannan comes from its own package, `libdannan<major.minor>`,
through `${shlibs:Depends}`; the Lua libraries from Debian's packages.

The package is built with debhelper from `debian/`, driving the CMake build;
the CMake install rules decide where every file goes. With
`-DMIMIR_FETCH_DANNAN=OFF` (the package build) CMake takes the installed
`libdannan-dev` of the pinned minor and never fetches.

## Building

On Debian trixie, natively for the machine's architecture, from a git
checkout. Dannan's packages first, at the pinned tag, by Dannan's own step;
then Mimir's:

```sh
sudo apt install dpkg-dev lintian
tools/ci.sh package
```

`tools/ci.sh package` clones Dannan at the pinned tag and runs its
`tools/ci.sh install-debs <tag>`, installs Mimir's build dependencies, runs
`tools/build-deb.sh` and checks the result with lintian, failing on any error
or warning. Then it installs the package, checks the `mimir` user, the site's
files with their owners and modes, and `mimir-node --version`, and purges it,
checking the purge left nothing: run it in a container or a CI runner, never
on a host that runs mimir. The package is written to `build-deb/`. The tests run inside the
build; `DEB_BUILD_OPTIONS=nocheck` skips them. CI builds the package on x86-64
and arm64.

## Versions

The version comes from `git describe`. The tag `0.1.0` gives `0.1.0`; a build
three commits later gives `0.1.0+git3.gabc1234`, which sorts after `0.1.0` and
before `0.1.1`. Release tags sit on `main`, so a build on `develop`, where none
is reachable, gives the tree's version and the commit, `0.1.0+git.gabc1234`.
Uncommitted changes add `.dirty`. A build of a tag must match
`project(mimir VERSION ...)` in `CMakeLists.txt`, or the script refuses it.
Only builds of a tag are released.

## What the package does

- `/etc/mimir/config.yaml` is a conffile: a cluster of one, reachable from
  the host only. An upgrade keeps local changes and asks before replacing a
  changed file.
- The `mimir` system user is made by systemd-sysusers.
- The postinst makes, only if absent, `/etc/mimir/secrets.yaml` (the users)
  and `/etc/mimir/cluster.key` (32 random bytes), both mode 600 for the
  `mimir` user, and `/var/lib/mimir`, the state directory. They are the
  site's, never the package's.
- `mimir.service` runs `mimir-node --config /etc/mimir/config.yaml` as
  `mimir`, sandboxed. It is installed disabled and not started; an upgrade
  restarts it if it is enabled or running.
- `apt remove` keeps `/etc/mimir` and `/var/lib/mimir`; `apt purge` deletes
  them.

## Releasing

The packages of a release go into the shared tomcully.com apt repository, in
its `incoming/mimir/<version>/`, and are added and signed with its
`publish.sh`; the repository's README says how it is checked and uploaded.
