# shoko-termux

[Shoko Server](https://shokoanime.com) on an Android phone, natively in
Termux. No proot, no Debian, no root. The stock `linux-arm64` glibc build of
Shoko runs on Termux's glibc repo (`glibc-runner`), plus one 60-line shim for
what Android's SELinux denies to apps. Runs on a Poco F5 Pro (Snapdragon 8+
Gen 1, 12 GB) with a USB SSD as the library.

## Why not proot

That was the previous setup and it worked, with a tax: every syscall through
ptrace (hashing a 900 GB library feels it), a 2 GB Debian rootfs to keep
updated, `sv restart` never reaching the .NET process, and every
`proot-distro login` being its own world (SSD bound in one, missing in the
next). Native means Shoko sees `/storage/XXXX-XXXX` like any Termux program,
services stop on TERM, one directory to back up.

## Why not bionic

.NET ships a `linux-bionic-arm64` runtime, but Shoko drags in
`Magick.Native` (ImageMagick + every codec statically linked, built on glibc
only). Rebuilding that chain with the NDK is days of work and repeats on every
Magick.NET bump. glibc-runner sidesteps it: same binaries, patched ELF
interpreter, no ptrace.

```txt
 anywhere                                      poco f5 pro (termux, home LAN)
┌──────────────────┐                          ┌───────────────────────────────────────┐
│ browser / mpv    │ https :8443              │ haproxy ─► anubis :8924 ─► Shoko :8111│
│ shoko-companion  ├─────────────────────────►│   │ Host label = backend    │         │
│                  │ http :8111 (LAN/tailnet) │   └─► anubis :8923 ─► mealprep :8090   │
│                  ├─────────────────────────►│                          │            │
└──────────────────┘                          │ ~/shoko/app (glibc .NET) ─┘            │
                                              │ ~/.shoko/Shoko.CLI  data, SQLite      │
                                              │ /storage/XXXX-XXXX  USB SSD (FUSE)    │
                                              └───────────────────────────────────────┘
```

## Layout

```txt
deploy/sv-shoko.run       runit service, sets LD_PRELOAD + DOTNET_ROOT, execs Shoko.CLI
deploy/sv-anubis.run      runit service, anubis instance in front of Shoko
deploy/sv-nfs.run         runit service, SSD exported over NFSv3 to the LAN (rclone, userspace)
deploy/anubis.env         anubis :8924 -> :8111
deploy/haproxy.cfg        "backend shoko", symlinked into ~/haproxy.d/
deploy/haproxy-base.cfg   reference copy of the box-level ~/haproxy.d/00-base.cfg
shim/ifaddrs_shim.c       getifaddrs/if_nametoindex over ioctl, LD_PRELOADed into Shoko
```

On the Poco, outside the repo: `~/shoko/app` (Shoko publish output),
`~/shoko/dotnet` (ASP.NET runtime), `~/shoko/libifaddrs_shim.so`,
`~/.shoko/Shoko.CLI` (settings, SQLite, images, logs).

## Install

All on the phone in Termux unless said otherwise. `$PREFIX` is Termux's.

### 1. glibc runtime

```sh
pkg install glibc-repo
pkg install glibc-runner libicu-glibc tzdata gcc-glibc binutils-glibc   # gcc only for the shim
```

`glibc-runner` installs a full glibc under `$PREFIX/glibc`. `grun -c <binary>`
rewrites a binary's ELF interpreter and rpath to it, after which the binary
runs as itself (`/proc/self/exe` correct, which .NET's apphost needs).

Never run glibc binaries with Termux's `LD_PRELOAD` set (it points at a bionic
`.so`): `env -u LD_PRELOAD ...`. Never export a glibc `.so` in `LD_PRELOAD`
in a normal shell either: every bionic program after it fails to link.

### 2. Shoko binaries

Any Linux machine with the .NET 8 SDK (or a
`mcr.microsoft.com/dotnet/sdk:8.0` container), cross-publishes fine from
x86_64:

```sh
git clone https://github.com/ShokoAnime/ShokoServer && cd ShokoServer
dotnet publish Shoko.CLI -c Release -r linux-arm64 --self-contained false -o out
scp -r out poco:shoko/app
```

Runtime on the phone (no proot needed, plain tarball):

```sh
mkdir -p ~/shoko && cd ~/shoko
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --runtime aspnetcore --channel 8.0 --arch arm64 --install-dir ~/shoko/dotnet
grun -c ~/shoko/app/Shoko.CLI
grun -c ~/shoko/dotnet/dotnet
```

Self-contained publish (`--self-contained true`) also works and skips the
runtime dir; then only `grun -c ~/shoko/app/Shoko.CLI`.

### 3. Shim

Android denies apps `bind()` on netlink sockets and `SIOCGIFINDEX` on
AF_UNIX sockets, so glibc's `getifaddrs()` fails with EACCES and
`if_nametoindex()` returns 0. .NET's `NetworkInterface.GetAllNetworkInterfaces`
then throws, or worse merges every interface into index 0 and Shoko decides
it has no network and stops talking to AniDB. The same ioctls on an AF_INET
socket are allowed, so the shim does that. IPv4 only, no gateways
(`/proc/net/route` is denied too; Shoko treats "no gateway" as fine and
checks WAN over HTTP).

```sh
git clone https://github.com/sandravwc/shoko-termux ~/shoko/repo
cd ~/shoko/repo/shim
env -u LD_PRELOAD PATH=$PREFIX/glibc/bin:$PATH gcc -O2 -shared -fPIC -o ~/shoko/libifaddrs_shim.so ifaddrs_shim.c
env -u LD_PRELOAD PATH=$PREFIX/glibc/bin:$PATH gcc -O2 -DTEST -o /tmp/t ifaddrs_shim.c && env -u LD_PRELOAD /tmp/t   # prints interfaces, OK
```

### 4. Service

```sh
pkg install termux-services
mkdir -p $PREFIX/var/service/shoko
cp ~/shoko/repo/deploy/sv-shoko.run $PREFIX/var/service/shoko/run
export SVDIR=$PREFIX/var/service
sv up shoko
sv status shoko; tail $PREFIX/var/log/sv/shoko/current
```

Web UI on `http://<poco>:8111`. First run creates `~/.shoko/Shoko.CLI`.
Coming from proot: copy the old `~/.shoko` over and fix paths in
`settings-server.json` and the `ImportFolder` table
(`pkg install sqlite; sqlite3 ~/.shoko/Shoko.CLI/SQLite/JMMServer.db3 "update ImportFolder set ImportFolderLocation='...'"`).

### 5. USB SSD

Android mounts it at `/storage/XXXX-XXXX` (FUSE, `rw` for Termux after
`termux-setup-storage`). Add it as an import directory in the Shoko UI by that
path. No symlinks, no ownership, no permissions on that filesystem; Shoko
does not need them.

### 6. Export the SSD over NFS (optional)

No kernel nfsd for apps, so userspace: `rclone serve nfs` (NFSv3, Go). No
auth, so LAN bind only. Handles cached on disk so file handles survive a
service restart without stale mounts on the clients.

```sh
pkg install rclone
mkdir -p $PREFIX/var/service/nfs
cp ~/shoko/repo/deploy/sv-nfs.run $PREFIX/var/service/nfs/run   # edit the /storage path and LAN IP
sv up nfs
```

Client (Linux), port must be given since it is not 2049 on portmapper:

```sh
sudo mkdir -p /mnt/shoko_ds1
echo '192.168.1.106:/ /mnt/shoko_ds1 nfs port=2049,mountport=2049,tcp,nolock,vers=3,nofail,_netdev,x-systemd.automount 0 0' | sudo tee -a /etc/fstab
sudo systemctl daemon-reload && sudo mount /mnt/shoko_ds1
```

Faster than sshfs (no crypto on the phone), slower than a real NAS: FUSE on
Android, then rclone, then the NFS stack. Fine for playback and copies.

### 7. Public: haproxy + anubis (optional)

One haproxy for the box, one anubis per app. haproxy loads a directory:

```sh
mkdir -p ~/haproxy.d
cp ~/shoko/repo/deploy/haproxy-base.cfg ~/haproxy.d/00-base.cfg    # once per box, edit cert path
ln -s ~/shoko/repo/deploy/haproxy.cfg ~/haproxy.d/20-shoko.cfg
# haproxy service run line: exec haproxy -W -db -f $HOME/haproxy.d
haproxy -c -f ~/haproxy.d && sv restart haproxy

mkdir -p $PREFIX/var/service/anubis-shoko
cp ~/shoko/repo/deploy/sv-anubis.run $PREFIX/var/service/anubis-shoko/run
sv up anubis-shoko
```

DNS: `shoko.poco.<zone>` as CNAME to `poco.<zone>` so the existing dyndns
keeps one A record current. Cert must cover it: reissue with a wildcard,
`acme.sh --issue --server letsencrypt --dns dns_autodns -d poco.<zone> -d '*.poco.<zone>'`.

## Operate

- Logs: `$PREFIX/var/log/sv/shoko/current`, `~/.shoko/Shoko.CLI/logs/`
- Restart: `sv restart shoko` (works now, TERM reaches the process)
- NFS export: `sv status nfs`, log `$PREFIX/var/log/sv/nfs/current`
- Update Shoko: publish again, `scp` over `~/shoko/app`, `grun -c ~/shoko/app/Shoko.CLI`, restart
- Update runtime: rerun `dotnet-install.sh`, `grun -c ~/shoko/dotnet/dotnet`
- Update shim: `git pull`, rebuild (step 3), restart
- `pkill -f Shoko.CLI` from an ssh session kills the session too (matches its own command line); use `pkill -x Shoko.CLI`

## Gotchas found on the way

- `proot-distro login --mount` is not a flag; it is `--bind`. The stale
  `rootfs/mnt/<name>` proot leaves behind is mode 000, so a container started
  without the bind shows "Permission denied" instead of "not found".
- `dotnet` SDK under glibc-runner: `dotnet build` dies on a named mutex,
  `/tmp/.dotnet/shm` is hardcoded and Termux has no `/tmp`. Runtime is fine,
  build elsewhere.
- Shoko logs `Checking LAN Connectivity…` and nothing after = zero usable
  interfaces (the `NoInterfaces` branch has no log line). That is the shim
  missing, or netlink being denied.
- `SIOCGIFHWADDR` is denied too, so MAC addresses come back empty. Nothing
  in Shoko cares.
- `TimeZoneNotFoundException: 'Tokyo Standard Time'` on AniDB jobs: .NET reads
  `/usr/share/zoneinfo`, Termux has `$PREFIX/share/zoneinfo` (`pkg install tzdata`).
  `TZDIR` in the run script fixes it.
