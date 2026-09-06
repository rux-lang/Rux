#!/bin/sh

# Run a command inside a prepared FreeBSD guest.
#
# Replaces the Python QEMU driver. The guest images are built and published by
# rux-lang/Toolchain; nothing is ever installed or compiled inside the guest
# here, so a consumer job only boots, syncs, runs, and syncs back.
#
# The VM is killed on INT, TERM, and exit, so cancelling a workflow run stops the
# guest in seconds instead of waiting out the job timeout.
#
# Usage:
#   sh .github/Scripts/FreeBSDVM.sh --arch x86_64 --role build --script FILE
#
# Options:
#   --arch x86_64|aarch64   Guest architecture
#   --role build|runtime|minimal
#   --script FILE           POSIX shell commands to run in the repository root
#   --writable              Persist guest disk changes (default: discard)
#   --push-only PATH        Extra path to push in addition to the repository
#   --pull PATH             Path to copy back afterwards; repeatable

set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -P "$script_directory/../.." && pwd)

arch=
role=
command_script=
writable=false
pull_paths="Bin BuildCache/ccache"

die() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

note() {
    printf '%s\n' "$1" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --arch)
        [ "$#" -ge 2 ] || die "option '--arch' requires a value"
        arch=$2
        shift 2
        ;;
    --role)
        [ "$#" -ge 2 ] || die "option '--role' requires a value"
        role=$2
        shift 2
        ;;
    --script)
        [ "$#" -ge 2 ] || die "option '--script' requires a value"
        command_script=$2
        shift 2
        ;;
    --writable)
        writable=true
        shift
        ;;
    --pull)
        [ "$#" -ge 2 ] || die "option '--pull' requires a value"
        pull_paths="$pull_paths $2"
        shift 2
        ;;
    *) die "unknown option '$1'" ;;
    esac
done

case "$arch" in
x86_64 | aarch64) ;;
*) die "option '--arch' must be x86_64 or aarch64" ;;
esac
case "$role" in
build | runtime | minimal) ;;
*) die "option '--role' must be build, runtime, or minimal" ;;
esac
[ -n "$command_script" ] || die "option '--script' is required"
[ -f "$command_script" ] || die "script '$command_script' was not found"

manifest=$repository_root/.github/Toolchains.env
[ -f "$manifest" ] || die "'$manifest' was not found"
if grep -qvE '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$manifest"; then
    die "'$manifest' contains a line that is not a comment or KEY=VALUE"
fi
. "$manifest"

[ "$TOOLCHAIN_REVISION" != TBD ] || die "TOOLCHAIN_REVISION is still TBD"

checksum_variable="SHA256_IMAGE_$(printf '%s_%s' "$role" "$arch" | tr 'a-z' 'A-Z')"
eval "checksum=\${$checksum_variable:-}"
[ -n "$checksum" ] || die "'$manifest' declares no $checksum_variable"
[ "$checksum" != TBD ] ||
    die "$checksum_variable is still TBD; publish a Toolchain release and record its checksum"

work_root=${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rux-freebsd-$role-$arch
image_root=${RUX_IMAGE_CACHE:-$work_root/image}
mkdir -p "$work_root" "$image_root"

# --- Image ---------------------------------------------------------------

asset_stem=rux-freebsd-$role-$arch-$TOOLCHAIN_REVISION
asset_base=$TOOLCHAIN_BASE_URL/toolchain-$TOOLCHAIN_REVISION

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Images exceed the release asset size limit, so they are published as ordered
# ≤1 GiB parts. The checksum covers the reassembled archive, not the parts.
fetch_image() {
    archive=$work_root/$asset_stem.tar.zst
    rm -f "$archive"
    part=0
    while :; do
        part_name=$(printf '%s.tar.zst.part%02d' "$asset_stem" "$part")
        part_url=$asset_base/$part_name
        if ! curl --fail --silent --show-error --location --retry 3 \
            --output "$work_root/$part_name" "$part_url"; then
            [ "$part" -gt 0 ] || die "could not download '$part_url'"
            break
        fi
        cat "$work_root/$part_name" >>"$archive"
        rm -f "$work_root/$part_name"
        part=$((part + 1))
    done

    actual=$(sha256_of "$archive")
    [ "$actual" = "$checksum" ] ||
        die "checksum mismatch for '$asset_stem': expected $checksum, got $actual"

    rm -rf "$image_root"
    mkdir -p "$image_root"
    tar --use-compress-program=unzstd -xf "$archive" -C "$image_root"
    rm -f "$archive"
    printf '%s' "$TOOLCHAIN_REVISION" >"$image_root/.revision"
}

if [ -f "$image_root/.revision" ] && [ "$(cat "$image_root/.revision")" = "$TOOLCHAIN_REVISION" ]; then
    note "Reusing cached FreeBSD $role $arch image $TOOLCHAIN_REVISION"
else
    note "Downloading FreeBSD $role $arch image $TOOLCHAIN_REVISION"
    fetch_image
fi

disk=$image_root/disk.qcow2
ssh_key=$image_root/ssh_key
[ -f "$disk" ] || die "'$disk' is missing from the image asset"
[ -f "$ssh_key" ] || die "'$ssh_key' is missing from the image asset"
chmod 600 "$ssh_key"

# --- Boot ----------------------------------------------------------------

host_arch=$(uname -m)
accelerated=false
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    case "$arch:$host_arch" in
    x86_64:x86_64 | aarch64:aarch64 | aarch64:arm64) accelerated=true ;;
    esac
fi

if [ "$arch" = x86_64 ] && [ "$accelerated" != true ]; then
    die "the FreeBSD x86-64 guest requires an accessible /dev/kvm on an x86-64 host"
fi
if [ "$accelerated" = true ]; then
    note "Booting the $arch guest with KVM acceleration"
else
    note "Booting the $arch guest under TCG emulation; expect it to be slow"
fi

# Bind the forwarded port to loopback only, and retry because the check and the
# bind are not atomic.
pick_port() {
    attempt=0
    while [ "$attempt" -lt 32 ]; do
        candidate=$((20000 + $(od -An -N2 -tu2 </dev/urandom | tr -d ' ') % 30000))
        if ! ss -ltnH "sport = :$candidate" 2>/dev/null | grep -q .; then
            printf '%s' "$candidate"
            return 0
        fi
        attempt=$((attempt + 1))
    done
    die "could not find a free loopback port"
}

ssh_port=$(pick_port)
serial_log=$work_root/vm/serial.log
mkdir -p "$work_root/vm"

qemu_pid=
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ -n "$qemu_pid" ] && kill -0 "$qemu_pid" 2>/dev/null; then
        note "Stopping the FreeBSD guest"
        kill -TERM "$qemu_pid" 2>/dev/null || true
        waited=0
        while kill -0 "$qemu_pid" 2>/dev/null && [ "$waited" -lt 10 ]; do
            sleep 1
            waited=$((waited + 1))
        done
        kill -KILL "$qemu_pid" 2>/dev/null || true
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

set -- \
    -m 8192 \
    -smp "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" \
    -nographic -serial "file:$serial_log" -monitor none \
    -drive "file=$disk,if=virtio,format=qcow2" \
    -device virtio-rng-pci \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$ssh_port-:22" \
    -device virtio-net-pci,netdev=net0

[ "$writable" = true ] || set -- "$@" -snapshot

if [ "$accelerated" = true ]; then
    set -- "$@" -enable-kvm -cpu host
fi

case "$arch" in
x86_64)
    qemu=qemu-system-x86_64
    ;;
aarch64)
    qemu=qemu-system-aarch64
    firmware=/usr/share/AAVMF/AAVMF_CODE.fd
    [ -f "$firmware" ] || die "'$firmware' was not found; install qemu-efi-aarch64"
    set -- "$@" -machine virt -drive "if=pflash,format=raw,readonly=on,file=$firmware"
    [ "$accelerated" = true ] || set -- "$@" -cpu cortex-a72
    ;;
esac

"$qemu" "$@" &
qemu_pid=$!

ssh_options="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
-o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes"

guest_ssh() {
    # shellcheck disable=SC2086 # ssh_options is a deliberate word list.
    ssh $ssh_options -i "$ssh_key" -p "$ssh_port" root@127.0.0.1 "$@"
}

boot_deadline=$(($(date +%s) + 900))
until guest_ssh true 2>/dev/null; do
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        tail -40 "$serial_log" >&2 || true
        die "the FreeBSD guest exited before SSH became available"
    fi
    if [ "$(date +%s)" -ge "$boot_deadline" ]; then
        tail -40 "$serial_log" >&2 || true
        die "the FreeBSD guest did not become reachable within 15 minutes"
    fi
    sleep 5
done
note "Guest is up on port $ssh_port"

guest_revision=$(guest_ssh cat /etc/rux-ci-revision 2>/dev/null || true)
[ "$guest_revision" = "$TOOLCHAIN_REVISION" ] ||
    die "guest reports revision '$guest_revision' but '$TOOLCHAIN_REVISION' was expected"

# --- Sync, run, sync back -------------------------------------------------

guest_root=/root/rux
# shellcheck disable=SC2086
rsync -a --delete --exclude=Build/ --exclude=Build-Debug/ --exclude=.git/ \
    -e "ssh $ssh_options -i $ssh_key -p $ssh_port" \
    "$repository_root/" "root@127.0.0.1:$guest_root/"

# shellcheck disable=SC2086
scp $ssh_options -i "$ssh_key" -P "$ssh_port" \
    "$command_script" "root@127.0.0.1:$guest_root/.ci-command.sh" >/dev/null

run_status=0
guest_ssh "cd $guest_root && PATH=/opt/rux-tools/bin:\$PATH sh .ci-command.sh" || run_status=$?

# Artifacts are pulled even when the command failed, so a failing job still
# uploads its logs and a warmed compilation cache.
for path in $pull_paths; do
    if guest_ssh "test -e $guest_root/$path" 2>/dev/null; then
        mkdir -p "$(dirname "$repository_root/$path")"
        # shellcheck disable=SC2086
        rsync -a -e "ssh $ssh_options -i $ssh_key -p $ssh_port" \
            "root@127.0.0.1:$guest_root/$path" "$(dirname "$repository_root/$path")/" || true
    fi
done

exit "$run_status"
