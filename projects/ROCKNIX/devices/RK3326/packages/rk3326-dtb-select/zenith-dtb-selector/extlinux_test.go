package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const zenithConf = "LABEL ROCKNIX\n  LINUX /KERNEL\n  ## Default DTB: R36 Ultra.\n  FDT /rk3326-gameconsole-r36ultra.dtb\n  APPEND boot=${partition_boot} disk=${partition_storage} quiet loglevel=1 vt.global_cursor_default=0 console=ttyS2,1500000 console=tty0 systemd.debug_shell=ttyS2 ssh uboot.hwid_adc=${hwid_adc}\n"

func TestChangeDTBOnly(t *testing.T) {
	out, err := applyEdit(zenithConf, Edit{DTB: "rk3326-r36s-type4.dtb", HideBoot: true, SSH: true})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out, "  FDT /rk3326-r36s-type4.dtb\n") {
		t.Fatalf("FDT no cambiado:\n%s", out)
	}
	if !strings.Contains(out, "quiet loglevel=1 vt.global_cursor_default=0 console=ttyS2,1500000") || !strings.Contains(out, " ssh uboot") {
		t.Fatalf("APPEND alterado sin pedirlo:\n%s", out)
	}
}

func TestUARTSwap(t *testing.T) {
	out, _ := applyEdit(zenithConf, Edit{DTB: "rk3326-r36s-type4.dtb", UART: 1, ApplyUART: true, HideBoot: true, SSH: true})
	if !strings.Contains(out, "console=ttyS1,1500000 console=tty0 systemd.debug_shell=ttyS1") {
		t.Fatalf("UART no cambiada:\n%s", out)
	}
}

func TestSSHOffAndShowBoot(t *testing.T) {
	out, _ := applyEdit(zenithConf, Edit{DTB: "rk3326-gameconsole-r36ultra.dtb", HideBoot: false, SSH: false})
	if strings.Contains(out, " ssh ") || strings.Contains(out, "quiet") || strings.Contains(out, "loglevel") {
		t.Fatalf("no se quitaron ssh/quiet/loglevel:\n%s", out)
	}
	back, _ := applyEdit(out, Edit{HideBoot: true, SSH: true})
	if !strings.Contains(back, "quiet loglevel=1") || !strings.Contains(back, " ssh uboot") {
		t.Fatalf("no se restauraron:\n%s", back)
	}
}

func TestCRLFAndTrailingSpaceNormalized(t *testing.T) {
	bad := strings.ReplaceAll(zenithConf, "\n", " \r\n")
	out, err := applyEdit(bad, Edit{DTB: "rk3326-r36s-type4.dtb", HideBoot: true, SSH: true})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(out, "\r") || strings.Contains(out, ".dtb ") {
		t.Fatalf("quedaron CR o espacios tras .dtb:\n%q", out)
	}
}

func TestInsertFDTWhenMissing(t *testing.T) {
	c := "LABEL ROCKNIX\n  LINUX /KERNEL\n  APPEND quiet console=ttyS1,1500000 console=tty0 systemd.debug_shell=ttyS1 uboot.hwid_adc=${hwid_adc}\n"
	out, err := applyEdit(c, Edit{DTB: "rk3326-gamemt-e6.dtb", HideBoot: true})
	if err != nil || !strings.Contains(out, "  LINUX /KERNEL\n  FDT /rk3326-gamemt-e6.dtb\n  APPEND") {
		t.Fatalf("FDT no insertado: %v\n%s", err, out)
	}
}

func TestRejectBadDTB(t *testing.T) {
	for _, d := range []string{"a b.dtb", "../x.dtb", "x.bin", "/abs.dtb"} {
		if _, err := applyEdit(zenithConf, Edit{DTB: d}); err == nil {
			t.Fatalf("acepto un dtb invalido: %q", d)
		}
	}
}

func TestReadState(t *testing.T) {
	st := readState(zenithConf)
	if st.FDT != "rk3326-gameconsole-r36ultra.dtb" || !st.HideBoot || !st.SSH || st.UART != 2 {
		t.Fatalf("%+v", st)
	}
}

func TestDiff(t *testing.T) {
	out, _ := applyEdit(zenithConf, Edit{DTB: "rk3326-r36s-type4.dtb", HideBoot: true, SSH: true})
	d := diffLines(zenithConf, out)
	minus, plus := 0, 0
	for _, l := range d {
		if l.T == "-" {
			minus++
		}
		if l.T == "+" {
			plus++
		}
	}
	if minus != 1 || plus != 1 {
		t.Fatalf("diff inesperado: %+v", d)
	}
}

func TestBackupsDoNotOverwriteWithinSameSecond(t *testing.T) {
	boot := t.TempDir()
	os.MkdirAll(filepath.Join(boot, "extlinux"), 0o755)
	conf := confPath(boot)
	versions := []string{"uno\n", "dos\n", "tres\n"}
	for _, v := range versions {
		os.WriteFile(conf, []byte(v), 0o644)
		if _, err := backupCurrent(boot); err != nil {
			t.Fatal(err)
		}
	}
	bs := listBackups(boot)
	if len(bs) != 3 {
		t.Fatalf("se esperaban 3 copias distintas, hay %d", len(bs))
	}
	seen := map[string]bool{}
	for _, b := range bs {
		d, _ := os.ReadFile(filepath.Join(backupDir(boot), b.Name))
		seen[string(d)] = true
	}
	if len(seen) != 3 {
		t.Fatalf("las copias se pisaron entre si: %v", seen)
	}
}
