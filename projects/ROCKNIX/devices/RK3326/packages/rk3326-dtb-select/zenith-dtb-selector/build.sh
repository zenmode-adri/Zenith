#!/bin/bash
# Compila Zenith DTB Selector (solo biblioteca estandar de Go >= 1.22, sin dependencias externas).
# Genera: ../prebuilt/ZenithDtbSelector-win64.exe (lo copia la imagen a la raiz de la FAT) y, en ./dist, los binarios para Linux y macOS.
set -e
cd "$(dirname "$0")"
export CGO_ENABLED=0
go vet ./... && go test ./...
LD="-s -w"
mkdir -p dist ../prebuilt
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "$LD" -o ../prebuilt/ZenithDtbSelector-win64.exe .
cp ../prebuilt/ZenithDtbSelector-win64.exe dist/
GOOS=linux  GOARCH=amd64 go build -trimpath -ldflags "$LD" -o dist/zenith-dtb-selector-linux-amd64 .
GOOS=linux  GOARCH=arm64 go build -trimpath -ldflags "$LD" -o dist/zenith-dtb-selector-linux-arm64 .
GOOS=darwin GOARCH=arm64 go build -trimpath -ldflags "$LD" -o dist/zenith-dtb-selector-macos-arm64 .
GOOS=darwin GOARCH=amd64 go build -trimpath -ldflags "$LD" -o dist/zenith-dtb-selector-macos-amd64 .
ls -la dist ../prebuilt
