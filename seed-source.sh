#!/bin/bash
# seed-source.sh — descarga fuentes de paquetes LibreELEC/ROCKNIX con espejos de respaldo
# y deja la caché completa (tarball + .sha256 + .url sidecars) para que scripts/get la acepte.
# Uso (DENTRO de WSL, desde la raíz del repo buildroot):  bash seed-source.sh <pkg> [pkg...]
set -u
cd "$(dirname "$0")"

find_pkg_mk() {
  local pkg="$1"
  find packages projects -name package.mk -not -path "*/build.*/*" 2>/dev/null | \
    xargs grep -l "^PKG_NAME=\"${pkg}\"\$" 2>/dev/null | head -1
}

mirror_for() {
  local url="$1"
  case "$url" in
    *ftpmirror.gnu.org*|*ftp.gnu.org*)
      echo "${url/ftpmirror.gnu.org\//mirrors.kernel.org/gnu/}"
      echo "${url/ftp.gnu.org\//mirrors.kernel.org/gnu/}"
      echo "${url/ftpmirror.gnu.org\//ftp.gnu.org/gnu/}"
      echo "${url/ftpmirror.gnu.org\//ftp.funet.fi/pub/gnu/}"
      echo "$url" ;;
    *sourceware.org*)
      echo "${url//sourceware.org/mirrors.kernel.org/sourceware}"
      echo "$url" ;;
    *) echo "$url" ;;
  esac
}

for PKG in "$@"; do
  MK="$(find_pkg_mk "$PKG")"
  if [ -z "$MK" ]; then echo "[$PKG] package.mk no encontrado, se omite"; continue; fi
  PKG_URL=$(grep -m1 '^PKG_URL=' "$MK" | cut -d'"' -f2)
  PKG_SHA256=$(grep -m1 '^PKG_SHA256=' "$MK" | cut -d'"' -f2)
  PKG_VER=$(grep -m1 '^PKG_VERSION=' "$MK" | cut -d'"' -f2)
  PKG_URL=$(echo "$PKG_URL" | sed -e "s|\\\${PKG_NAME}|$PKG|g" -e "s|\\\${PKG_VERSION}|$PKG_VER|g")
  if [ -z "$PKG_URL" ]; then echo "[$PKG] PKG_URL no estático en $MK, se omite (hacer a mano)"; continue; fi
  SRC_NAME="${PKG_URL##*/}"
  DEST="sources/${PKG}/${SRC_NAME}"
  mkdir -p "sources/${PKG}"
  if [ -f "${DEST}.sha256" ] && [ "$(cat "${DEST}.sha256" 2>/dev/null)" = "${PKG_SHA256}" ] && [ -s "$DEST" ]; then
    echo "[$PKG] caché ya válida"; continue
  fi
  OK=0
  while read -r TRY; do
    [ -z "$TRY" ] && continue
    echo "[$PKG] intentando: $TRY"
    if curl -fsSL --connect-timeout 15 --max-time 300 -o "$DEST" "$TRY" && [ -s "$DEST" ]; then
      GOT=$(sha256sum "$DEST" | cut -d" " -f1)
      if [ -n "$PKG_SHA256" ] && [ "$GOT" != "$PKG_SHA256" ]; then
        echo "[$PKG] checksum NO coincide, otra URL"; rm -f "$DEST"; continue
      fi
      { [ -n "$PKG_SHA256" ] && echo "$PKG_SHA256" || echo "$GOT"; } > "${DEST}.sha256"
      printf '%s\n' "$TRY" > "${DEST}.url"
      echo "[$PKG] OK -> $DEST"; OK=1; break
    fi
    rm -f "$DEST"
  done < <(mirror_for "$PKG_URL")
  [ "$OK" = 0 ] && echo "[$PKG] FALLO en todas las URLs"
done
