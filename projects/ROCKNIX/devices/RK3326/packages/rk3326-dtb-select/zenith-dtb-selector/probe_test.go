package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func fakeCard(t *testing.T, dtbs ...string) string {
	boot := t.TempDir()
	os.MkdirAll(filepath.Join(boot, "extlinux"), 0o755)
	os.WriteFile(filepath.Join(boot, "KERNEL"), nil, 0o644)
	os.WriteFile(confPath(boot), []byte(zenithConf), 0o644)
	os.WriteFile(filepath.Join(boot, "zenith-features.txt"), []byte("dtb-probe=1\n"), 0o644)
	for _, d := range dtbs {
		os.WriteFile(filepath.Join(boot, d), fakeDTB(200, true), 0o644)
	}
	return boot
}

func TestProbeStartStop(t *testing.T) {
	boot := fakeCard(t, "rk3326-gameconsole-r36ultra.dtb", "a.dtb", "b.dtb", "c.dtb")
	orig, _ := os.ReadFile(confPath(boot))
	n, err := probeStart(boot, []string{"a.dtb", "falta.dtb", "../x.dtb", "b.dtb", "a.dtb", "c.dtb"}, false)
	if err != nil || n != 3 {
		t.Fatalf("start: %d %v", n, err)
	}
	st := probeStatus(boot)
	if !st.Active || st.Total != 3 || st.Index != 0 || st.Current != "a.dtb" || strings.Join(st.List, ",") != "a.dtb,b.dtb,c.dtb" {
		t.Fatalf("estado: %+v", st)
	}
	cur, _ := os.ReadFile(confPath(boot))
	if got := readState(string(cur)); got.FDT != "a.dtb" || !got.HideBoot || !got.SSH || got.UART != 2 {
		t.Fatalf("solo debia cambiar el FDT: %+v", got)
	}
	if o, _ := os.ReadFile(filepath.Join(probeDir(boot), "extlinux.orig")); string(o) != string(orig) {
		t.Fatal("extlinux.orig no es el original")
	}
	if _, err := probeStart(boot, []string{"a.dtb", "b.dtb"}, false); err == nil || err.Error() != "probe-active" {
		t.Fatalf("segundo start debia fallar: %v", err)
	}
	if err := probeStop(boot); err != nil {
		t.Fatal(err)
	}
	after, _ := os.ReadFile(confPath(boot))
	if string(after) != strings.Join(normalize(string(orig)), "\n")+"\n" {
		t.Fatalf("no se restauro:\n%s", after)
	}
	if probeStatus(boot).Active {
		t.Fatal("sigue activo")
	}
	if _, err := os.Stat(probeDir(boot)); err == nil {
		t.Fatal("quedo la carpeta")
	}
}

func TestProbeNeedsTwoCandidates(t *testing.T) {
	boot := fakeCard(t, "a.dtb")
	if _, err := probeStart(boot, []string{"a.dtb"}, false); err == nil || err.Error() != "probe-few" {
		t.Fatalf("esperaba probe-few: %v", err)
	}
	if _, err := os.Stat(probeDir(boot)); err == nil {
		t.Fatal("no debia crear nada")
	}
	cur, _ := os.ReadFile(confPath(boot))
	if string(cur) != zenithConf {
		t.Fatal("extlinux cambio sin empezar")
	}
}

func TestProbeLastResult(t *testing.T) {
	boot := fakeCard(t, "a.dtb", "b.dtb")
	os.WriteFile(probeLast(boot), []byte("elegido: b.dtb (candidato 2 de 2, 3 arranques)\n"), 0o644)
	if st := probeStatus(boot); st.Active || !strings.HasPrefix(st.Last, "elegido: b.dtb") {
		t.Fatalf("%+v", st)
	}
}

func TestProbeOrderFile(t *testing.T) {
	loadProbeOrder()
	if len(probeOrder.Candidates) < 10 {
		t.Fatalf("probe-order.json no cargado: %d", len(probeOrder.Candidates))
	}
	seen := map[string]bool{}
	for _, c := range probeOrder.Candidates {
		if !reSafeName.MatchString(c.DTB) || seen[c.DTB] {
			t.Fatalf("candidato invalido o repetido: %q", c.DTB)
		}
		seen[c.DTB] = true
		if findByDTB(c.DTB) == nil {
			t.Fatalf("%s no esta en el catalogo", c.DTB)
		}
	}
}

func findByDTB(dtb string) *Device {
	for i := range catalog.Devices {
		if catalog.Devices[i].DTB == dtb {
			return &catalog.Devices[i]
		}
	}
	return nil
}

func init() {
	// los tests usan el catalogo embebido igual que el programa
	loadCatalog()
}

func TestProbeNeedsCapableImage(t *testing.T) {
	boot := fakeCard(t, "a.dtb", "b.dtb")
	os.Remove(filepath.Join(boot, "zenith-features.txt")) // imagen antigua: sin el script de la consola
	if probeSupported(boot) {
		t.Fatal("no debia estar soportado")
	}
	if _, err := probeStart(boot, []string{"a.dtb", "b.dtb"}, false); err == nil || err.Error() != "probe-unsupported" {
		t.Fatalf("esperaba probe-unsupported: %v", err)
	}
	if cur, _ := os.ReadFile(confPath(boot)); string(cur) != zenithConf {
		t.Fatal("no debia tocar extlinux.conf")
	}
	if _, err := os.Stat(probeDir(boot)); err == nil {
		t.Fatal("no debia crear nada")
	}
	if n, err := probeStart(boot, []string{"a.dtb", "b.dtb"}, true); err != nil || n != 2 {
		t.Fatalf("con force deberia funcionar: %d %v", n, err)
	}
}
