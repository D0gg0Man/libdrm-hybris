#!/bin/bash
# install-libdrm-hybris.sh -- build and install libdrm-hybris.so, the
# unified LD_PRELOAD shim for running phosh (and gnome-mali) on FuriOS
# Mali/HWC2 devices. Replaces the default phosh session pipeline shims.
#
# Usage: run from the repo root (where src/libdrm-hybris.c lives),
# or pass the source path as $1.
#
# What this does:
#   1. Checks runtime dependencies a fresh device needs
#   2. Builds libdrm-hybris.so from src/libdrm-hybris.c
#   3. Installs it to /usr/lib/aarch64-linux-gnu/ and replaces libseat.so.1
#      (phoc links libseat directly; LD_PRELOAD alone cannot intercept it,
#      and PAM strips LD_PRELOAD for the _greetd user anyway)
#   4. Adds libdrm-hybris.so to /etc/ld.so.preload (survives PAM stripping)
#   5. Adds _greetd to the input group (shim opens /dev/input/* directly)
#   6. Injects LD_PRELOAD into the stock phrog wrapper (kept for non-greetd
#      child processes; launch logic untouched -- phoc -S -E as stock)
#   7. Removes superseded shims (drm_shim.so, libseat_shim.so)
#   8. Patches gnome-mali-session if present (drop drm_shim, fix XDG env)
#   9. Installs the phosh EGL drop-in (EGL_PLATFORM=wayland for the client)
#
# To revert:
#   sudo cp /usr/lib/aarch64-linux-gnu/libseat.so.1.orig \
#           /usr/lib/aarch64-linux-gnu/libseat.so.1
#   sudo cp /usr/libexec/phrog-greetd-session-wrapper.orig \
#           /usr/libexec/phrog-greetd-session-wrapper
#   sudo cp /usr/libexec/gnome-mali-session.orig \
#           /usr/libexec/gnome-mali-session 2>/dev/null
#   sudo sed -i '\|libdrm-hybris|d' /etc/ld.so.preload

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$SCRIPT_DIR/src}"   # directory: all modules are built together
OUT="/usr/lib/aarch64-linux-gnu/libdrm-hybris.so"
LIBSEAT="/usr/lib/aarch64-linux-gnu/libseat.so.1"
PHROG_WRAPPER="/usr/libexec/phrog-greetd-session-wrapper"
GNOME_SESSION="/usr/libexec/gnome-mali-session"
DROPIN_DIR="$HOME/.config/systemd/user/mobi.phosh.Shell.service.d"
DROPIN="$DROPIN_DIR/drmadapter.conf"
PRELOAD_FILE="/etc/ld.so.preload"

# --------------------------------------------------------------------------
# 1. Dependency checks -- fail early on a fresh device missing pieces
# --------------------------------------------------------------------------
echo "=== Checking dependencies ==="
FAIL=0

# Build tools and dev headers -- install whatever is missing via apt
APT_PKGS=""
command -v gcc  >/dev/null || APT_PKGS="$APT_PKGS gcc"
command -v make >/dev/null || APT_PKGS="$APT_PKGS make"
command -v git  >/dev/null || APT_PKGS="$APT_PKGS git"
[ -f /usr/include/libdrm/xf86drm.h ] || [ -f /usr/include/xf86drm.h ] || APT_PKGS="$APT_PKGS libdrm-dev"
[ -f /usr/include/EGL/egl.h ]         || APT_PKGS="$APT_PKGS libegl-dev"
[ -f /usr/include/wayland-server.h ]  || APT_PKGS="$APT_PKGS libwayland-dev"

if [ -n "$APT_PKGS" ]; then
    echo "    installing missing packages:$APT_PKGS"
    sudo apt-get install -y $APT_PKGS
fi

# hybris headers cannot come from generic apt -- must be present already
for hdr in /usr/include/android/android-config.h \
           /usr/include/hybris/gralloc/gralloc.h; do
    [ -f "$hdr" ] || { echo "    FAIL: missing header $hdr (libhybris-dev required)"; FAIL=1; }
done

# Re-verify after install
[ -f /usr/include/libdrm/xf86drm.h ] || [ -f /usr/include/xf86drm.h ] || \
    { echo "    FAIL: still missing xf86drm.h"; FAIL=1; }
for hdr in /usr/include/EGL/egl.h \
           /usr/include/wayland-server.h; do
    [ -f "$hdr" ] || { echo "    FAIL: still missing header $hdr"; FAIL=1; }
done

# drmadapter EGL platform -- phosh client renders through this.
# Built from git below if missing.
DRMADAPTER=$(find /usr/lib/aarch64-linux-gnu/libhybris -name "*drmadapter*" 2>/dev/null | head -1)
[ -n "$DRMADAPTER" ] && echo "    OK: drmadapter platform: $DRMADAPTER"

# Companion libraries -- built from repo src/, with built/ as fallback
build_companion() {
    local name="$1" dest="$2"
    if [ -f "$dest" ]; then
        echo "    OK: $name present"
        return 0
    fi
    if [ -f "$SCRIPT_DIR/src/$name.c" ]; then
        echo "    building $name from src/$name.c"
        local tmp
        tmp="$(mktemp /tmp/$name-XXXXXX.so)"
        if gcc -shared -fPIC -O2 \
                -I/usr/include/libdrm \
                -I/usr/include \
                -I/usr/include/android \
                -o "$tmp" "$SCRIPT_DIR/src/$name.c" \
                -ldl -lEGL -lgralloc -ldrm -lwayland-server; then
            sudo install -D -m 755 "$tmp" "$dest"
            rm -f "$tmp"
            echo "    OK: $name built and installed"
            return 0
        fi
        rm -f "$tmp"
        echo "    WARN: $name build failed, trying prebuilt"
    fi
    if [ -f "$SCRIPT_DIR/built/$name.so" ]; then
        sudo install -D -m 755 "$SCRIPT_DIR/built/$name.so" "$dest"
        echo "    OK: $name installed from built/"
        return 0
    fi
    return 1
}

# wlegl_server.so -- android_wlegl protocol for client buffer sharing
if build_companion "wlegl_server" "/usr/local/lib/wlegl_server.so"; then
    WLEGL="/usr/local/lib/wlegl_server.so"
else
    echo "    WARN: phosh buffer sharing will fail without wlegl_server.so"
    WLEGL=""
fi

# eglplatform_drmadapter.so -- hybris EGL platform for the phosh client
if [ -z "$DRMADAPTER" ]; then
    if build_companion "eglplatform_drmadapter" \
            "/usr/lib/aarch64-linux-gnu/libhybris/eglplatform_drmadapter.so"; then
        DRMADAPTER="/usr/lib/aarch64-linux-gnu/libhybris/eglplatform_drmadapter.so"
    fi
fi

# vulkan_x11_stub.so -- optional, silences Vulkan loader
if build_companion "vulkan_x11_stub" "/usr/local/lib/vulkan_x11_stub.so"; then
    VULKAN="/usr/local/lib/vulkan_x11_stub.so"
else
    echo "    WARN: vulkan_x11_stub.so missing -- optional, Vulkan apps may log errors"
    VULKAN=""
fi

if [ -z "$DRMADAPTER" ]; then
    echo "    FAIL: eglplatform_drmadapter.so unavailable -- phosh cannot render without it"
    FAIL=1
fi

[ "$FAIL" -eq 1 ] && { echo ""; echo "Fix the FAIL items above and re-run."; exit 1; }

[ -f "$SRC" ] || { echo "ERROR: $SRC not found"; exit 1; }

# --------------------------------------------------------------------------
# 2. Build
# --------------------------------------------------------------------------
echo ""
echo "=== Building libdrm-hybris.so ==="
TMP="$(mktemp /tmp/libdrm-hybris-XXXXXX.so)"
gcc -shared -fPIC -O2 \
    -I/usr/include/libdrm \
    -I/usr/include \
    -I/usr/include/android \
    -o "$TMP" "$SRC" \
    -ldl -lEGL -lgralloc -ldrm -lwayland-server
echo "    build OK"

# --------------------------------------------------------------------------
# 3. Install -- both names
# --------------------------------------------------------------------------
echo ""
echo "=== Installing ==="
sudo install -m 755 "$TMP" "$OUT"
echo "    installed: $OUT"

if [ ! -f "${LIBSEAT}.orig" ]; then
    sudo cp "$LIBSEAT" "${LIBSEAT}.orig"
    echo "    backed up: ${LIBSEAT}.orig"
fi
sudo install -m 755 "$TMP" "$LIBSEAT"
echo "    replaced: $LIBSEAT"
rm -f "$TMP"

# --------------------------------------------------------------------------
# 4. ld.so.preload -- survives PAM environment stripping for _greetd
# --------------------------------------------------------------------------
echo ""
echo "=== Updating $PRELOAD_FILE ==="
sudo touch "$PRELOAD_FILE"
grep -qxF "$OUT" "$PRELOAD_FILE" || echo "$OUT" | sudo tee -a "$PRELOAD_FILE" > /dev/null
[ -n "$VULKAN" ] && { grep -qxF "$VULKAN" "$PRELOAD_FILE" || echo "$VULKAN" | sudo tee -a "$PRELOAD_FILE" > /dev/null; }
# Deduplicate
sudo bash -c "awk '!seen[\$0]++' $PRELOAD_FILE > /tmp/preload.tmp && mv /tmp/preload.tmp $PRELOAD_FILE"
echo "    contents:"
sed 's/^/      /' "$PRELOAD_FILE"

# --------------------------------------------------------------------------
# 5. _greetd input group
# --------------------------------------------------------------------------
echo ""
echo "=== Adding _greetd to input group ==="
if id _greetd &>/dev/null; then
    sudo usermod -aG input _greetd
    echo "    done"
else
    echo "    SKIP: _greetd user not found"
fi

# --------------------------------------------------------------------------
# 6. phrog wrapper LD_PRELOAD injection (for child processes of the session)
# --------------------------------------------------------------------------
echo ""
echo "=== Patching phrog session wrapper ==="
if [ ! -f "$PHROG_WRAPPER" ]; then
    echo "    SKIP: $PHROG_WRAPPER not found"
else
    [ -f "${PHROG_WRAPPER}.orig" ] || sudo cp "$PHROG_WRAPPER" "${PHROG_WRAPPER}.orig"
    if grep -q "libdrm-hybris" "$PHROG_WRAPPER"; then
        echo "    already patched"
    else
        PRELOAD_LINE="$OUT"
        [ -n "$WLEGL" ]  && PRELOAD_LINE="${PRELOAD_LINE}:${WLEGL}"
        [ -n "$VULKAN" ] && PRELOAD_LINE="${PRELOAD_LINE}:${VULKAN}"
        sudo sed -i "/^exec /i # Injected by install-libdrm-hybris.sh\nexport LD_PRELOAD=\"${PRELOAD_LINE}\${LD_PRELOAD:+:\$LD_PRELOAD}\"\n" "$PHROG_WRAPPER"
        echo "    injected LD_PRELOAD"
    fi
fi

# --------------------------------------------------------------------------
# 7. Remove superseded shims
# --------------------------------------------------------------------------
echo ""
echo "=== Removing superseded shims ==="
for shim in /usr/local/lib/drm_shim.so /usr/local/lib/libseat_shim.so; do
    [ -f "$shim" ] && { sudo rm -f "$shim"; echo "    removed: $shim"; } || true
done

# --------------------------------------------------------------------------
# 8. gnome-mali-session patch (if present)
# --------------------------------------------------------------------------
echo ""
echo "=== Patching gnome-mali-session (if present) ==="
if [ ! -f "$GNOME_SESSION" ]; then
    echo "    SKIP: not installed"
else
    [ -f "${GNOME_SESSION}.orig" ] || sudo cp "$GNOME_SESSION" "${GNOME_SESSION}.orig"
    if ! grep -q "libdrm-hybris" "$GNOME_SESSION"; then
        sudo sed -i \
            's|/usr/local/lib/drm_shim.so:||g;s|:/usr/local/lib/drm_shim.so||g;s|/usr/local/lib/drm_shim.so||g' \
            "$GNOME_SESSION"
        sudo sed -i \
            "s|MALI_PRELOAD=\"/usr/local/lib/wlegl_server.so|MALI_PRELOAD=\"${OUT}:/usr/local/lib/wlegl_server.so|g" \
            "$GNOME_SESSION"
        echo "    shim path updated"
    fi
    if ! grep -q "unset-environment XDG_SESSION_DESKTOP" "$GNOME_SESSION"; then
        sudo sed -i \
            '/systemctl --user import-environment/i systemctl --user unset-environment XDG_SESSION_DESKTOP XDG_CURRENT_DESKTOP' \
            "$GNOME_SESSION"
        echo "    XDG environment fix applied"
    fi
fi

# --------------------------------------------------------------------------
# 9. phosh EGL drop-in
# --------------------------------------------------------------------------
echo ""
echo "=== Installing phosh EGL drop-in ==="
mkdir -p "$DROPIN_DIR"
cat > "$DROPIN" << 'EOF'
# mobi.phosh.Shell.service.d/drmadapter.conf
#
# phosh is a Wayland client -- must use EGL_PLATFORM=wayland and
# HYBRIS_EGLPLATFORM=drmadapter. Using hwcomposer would open a second
# HWC2 connection -> "failed to create composer client" -> SIGABRT.

[Service]
Environment=EGL_PLATFORM=wayland
Environment=HYBRIS_EGLPLATFORM=drmadapter
UnsetEnvironment=WLR_BACKENDS
UnsetEnvironment=WLR_HWC_SKIP_VERSION_CHECK
EOF
systemctl --user daemon-reload 2>/dev/null || true
echo "    installed: $DROPIN"

# --------------------------------------------------------------------------
echo ""
echo "=== Done ==="
echo ""
echo "Reboot or: sudo systemctl restart greetd"
echo ""
echo "Verify after login:"
echo "    nm -D $LIBSEAT | grep libdrm_hybris_shim_marker   # shim is libseat"
echo "    PHOSH_PID=\$(pgrep -x phosh)"
echo "    cat /proc/\$PHOSH_PID/environ | tr '\\0' '\\n' | grep -E 'EGL|HYBRIS'"
echo "    # expect: EGL_PLATFORM=wayland  HYBRIS_EGLPLATFORM=drmadapter"
