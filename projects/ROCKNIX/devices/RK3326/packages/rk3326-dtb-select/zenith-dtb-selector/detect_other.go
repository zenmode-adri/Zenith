//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
)

func initPlatform() {}

func freeMB(path string) int64 {
	var s syscall.Statfs_t
	if err := syscall.Statfs(path, &s); err != nil {
		return 0
	}
	return int64(uint64(s.Bavail) * uint64(s.Bsize) >> 20)
}

func detectVolumes() []Volume {
	seen := map[string]bool{}
	var cands []string
	// Linux: puntos de montaje vfat/exfat
	if b, err := os.ReadFile("/proc/mounts"); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			f := strings.Fields(l)
			if len(f) >= 3 && (f[2] == "vfat" || f[2] == "msdos" || f[2] == "exfat" || f[2] == "fuseblk") {
				cands = append(cands, strings.ReplaceAll(f[1], "\\040", " "))
			}
		}
	}
	// macOS y rutas habituales de automontaje
	for _, g := range []string{"/Volumes/*", "/media/*/*", "/run/media/*/*", "/mnt/*", "/media/*"} {
		m, _ := filepath.Glob(g)
		cands = append(cands, m...)
	}
	var out []Volume
	for _, c := range cands {
		if seen[c] {
			continue
		}
		seen[c] = true
		if ok, _ := validBoot(c); !ok {
			continue
		}
		out = append(out, Volume{Path: c, Label: filepath.Base(c), FS: "FAT", FreeMB: freeMB(c)})
	}
	return out
}
