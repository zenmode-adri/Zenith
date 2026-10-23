package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func fakeDTB(size int, withSoC bool) []byte {
	b := make([]byte, size)
	binary.BigEndian.PutUint32(b[0:4], 0xd00dfeed)
	binary.BigEndian.PutUint32(b[4:8], uint32(size))
	if withSoC && size > 130 {
		copy(b[100:], []byte("rockchip,rk3326"))
	}
	return b
}

func TestSafeDTBName(t *testing.T) {
	ok := map[string]string{"mi-consola.dtb": "mi-consola.dtb", "C:\\Users\\x\\mi.dtb": "mi.dtb", "a%20b.dtb": ""}
	for in, want := range ok {
		got, err := safeDTBName(in)
		if want == "" {
			if err == nil {
				t.Fatalf("acepto %q como %q", in, got)
			}
			continue
		}
		if err != nil || got != want {
			t.Fatalf("%q -> %q, %v (esperaba %q)", in, got, err, want)
		}
	}
	for _, bad := range []string{"..\\x.dtb", "x.bin", ".dtb", "-x.dtb", "a b.dtb", "KERNEL", "x.dtb ", "con;punto.dtb"} {
		if n, err := safeDTBName(bad); err == nil && bad != "..\\x.dtb" {
			t.Fatalf("acepto %q -> %q", bad, n)
		}
	}
	// "..\x.dtb" se reduce a su nombre base: nunca sale de la raiz
	if n, err := safeDTBName("..\\x.dtb"); err != nil || n != "x.dtb" {
		t.Fatalf("..\\x.dtb -> %q %v", n, err)
	}
}

func TestCheckDTB(t *testing.T) {
	if soc, err := checkDTB(fakeDTB(200, true)); err != nil || !soc {
		t.Fatalf("dtb valido rechazado: %v %v", soc, err)
	}
	if soc, err := checkDTB(fakeDTB(200, false)); err != nil || soc {
		t.Fatalf("debe aceptar pero avisar de SoC: %v %v", soc, err)
	}
	bad := fakeDTB(200, true)
	bad[0] = 0
	for _, b := range [][]byte{nil, []byte("hola"), bad, fakeDTB(30, true), make([]byte, maxDTBSize+1)} {
		if _, err := checkDTB(b); err == nil {
			t.Fatalf("acepto un DTB invalido de %d bytes", len(b))
		}
	}
	trunc := fakeDTB(200, true)
	binary.BigEndian.PutUint32(trunc[4:8], 5000) // totalsize mayor que el fichero
	if _, err := checkDTB(trunc); err == nil {
		t.Fatal("acepto un DTB truncado")
	}
}

func TestImportDTB(t *testing.T) {
	boot := t.TempDir()
	os.MkdirAll(filepath.Join(boot, "extlinux"), 0o755)
	os.WriteFile(filepath.Join(boot, "KERNEL"), nil, 0o644)
	os.WriteFile(confPath(boot), []byte("LABEL X\n  LINUX /KERNEL\n  FDT /a.dtb\n  APPEND quiet\n"), 0o644)
	d := fakeDTB(300, true)
	n, soc, ex, err := importDTB(boot, "mi.dtb", d, false)
	if err != nil || n != "mi.dtb" || !soc || ex {
		t.Fatalf("import: %v %v %v %v", n, soc, ex, err)
	}
	if _, _, ex, err := importDTB(boot, "mi.dtb", d, false); err == nil || !ex {
		t.Fatalf("debia negarse a pisar: %v %v", ex, err)
	}
	if _, _, ex, err := importDTB(boot, "mi.dtb", d, true); err != nil || !ex {
		t.Fatalf("overwrite explicito: %v %v", ex, err)
	}
	if _, _, _, err := importDTB(boot, "../fuera.dtb", d, false); err != nil {
		t.Fatalf("el nombre base deberia aceptarse: %v", err)
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(boot), "fuera.dtb")); err == nil {
		t.Fatal("escribio fuera de la tarjeta")
	}
	if _, _, _, err := importDTB(boot, "malo.dtb", []byte("no soy un dtb"), false); err == nil {
		t.Fatal("acepto basura")
	}
	if ents, _ := os.ReadDir(boot); func() bool {
		for _, e := range ents {
			if filepath.Ext(e.Name()) == ".tmp-zenith" {
				return true
			}
		}
		return false
	}() {
		t.Fatal("quedo un temporal")
	}
}
