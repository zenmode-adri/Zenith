package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Estado leido de un extlinux.conf.
type State struct {
	FDT      string `json:"fdt"`      // nombre del dtb sin la barra inicial
	HideBoot bool   `json:"hideBoot"` // "quiet" presente
	SSH      bool   `json:"ssh"`
	UART     int    `json:"uart"` // 0 si no hay console=ttyS
}

// Cambios pedidos por la interfaz.
type Edit struct {
	DTB        string `json:"dtb"`
	UART       int    `json:"uart"`
	ApplyUART  bool   `json:"applyUart"`
	HideBoot   bool   `json:"hideBoot"`
	SSH        bool   `json:"ssh"`
	KeepBoot   bool   `json:"-"`
}

var (
	reFDT     = regexp.MustCompile(`(?i)^(\s*)FDT\s+(\S+)\s*$`)
	reAPPEND  = regexp.MustCompile(`^(\s*APPEND\s+)(.*)$`)
	reConsole = regexp.MustCompile(`^console=(?:/dev/)?ttyS(\d+)(,\d+)?$`)
	reDebugSh = regexp.MustCompile(`^systemd\.debug_shell=(?:/dev/)?ttyS(\d+)$`)
)

// normalize deja el texto con LF, sin espacios ni CR al final de linea y con un unico salto final.
// u-boot tiene un parser muy pobre: un espacio o un CR tras el .dtb impide arrancar.
func normalize(text string) []string {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	lines := strings.Split(text, "\n")
	for i := range lines {
		lines[i] = strings.TrimRight(lines[i], " \t")
	}
	for len(lines) > 0 && lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	return lines
}

func readState(text string) State {
	var st State
	for _, l := range normalize(text) {
		if m := reFDT.FindStringSubmatch(l); m != nil && st.FDT == "" {
			st.FDT = strings.TrimPrefix(m[2], "/")
		}
		if m := reAPPEND.FindStringSubmatch(l); m != nil {
			for _, tok := range strings.Fields(m[2]) {
				if tok == "quiet" {
					st.HideBoot = true
				}
				if tok == "ssh" {
					st.SSH = true
				}
				if c := reConsole.FindStringSubmatch(tok); c != nil && st.UART == 0 {
					fmt.Sscanf(c[1], "%d", &st.UART)
				}
			}
		}
	}
	return st
}

// applyEdit devuelve el nuevo texto. Solo toca lo que cambia respecto al estado actual.
func applyEdit(text string, e Edit) (string, error) {
	lines := normalize(text)
	if len(lines) == 0 {
		return "", fmt.Errorf("extlinux.conf vacio")
	}
	cur := readState(text)

	// --- FDT ---
	if e.DTB != "" {
		if strings.ContainsAny(e.DTB, " \t/\\") || !strings.HasSuffix(e.DTB, ".dtb") {
			return "", fmt.Errorf("nombre de DTB no valido: %q", e.DTB)
		}
		done := false
		for i, l := range lines {
			if m := reFDT.FindStringSubmatch(l); m != nil {
				lines[i] = m[1] + "FDT /" + e.DTB
				done = true
				break
			}
		}
		if !done {
			ins := -1
			for i, l := range lines {
				if strings.HasPrefix(strings.TrimSpace(l), "LINUX") {
					ins = i + 1
					break
				}
			}
			if ins < 0 {
				return "", fmt.Errorf("no encuentro la linea LINUX en extlinux.conf")
			}
			lines = append(lines[:ins], append([]string{"  FDT /" + e.DTB}, lines[ins:]...)...)
		}
	}

	// --- APPEND ---
	ai := -1
	for i, l := range lines {
		if reAPPEND.MatchString(l) {
			ai = i
			break
		}
	}
	if ai < 0 {
		return "", fmt.Errorf("no encuentro la linea APPEND en extlinux.conf")
	}
	m := reAPPEND.FindStringSubmatch(lines[ai])
	toks := strings.Fields(m[2])

	has := func(t string) bool {
		for _, x := range toks {
			if x == t {
				return true
			}
		}
		return false
	}
	remove := func(pred func(string) bool) {
		out := toks[:0]
		for _, x := range toks {
			if !pred(x) {
				out = append(out, x)
			}
		}
		toks = out
	}
	insertBefore := func(newTok string, beforePrefixes ...string) {
		for i, x := range toks {
			for _, p := range beforePrefixes {
				if strings.HasPrefix(x, p) {
					toks = append(toks[:i], append([]string{newTok}, toks[i:]...)...)
					return
				}
			}
		}
		toks = append(toks, newTok)
	}

	// mensajes de arranque
	if e.HideBoot != cur.HideBoot {
		if e.HideBoot {
			if !has("quiet") {
				insertBefore("quiet", "console=", "uboot.")
			}
			if !strings.Contains(" "+strings.Join(toks, " "), " loglevel=") {
				// justo detras de quiet
				for i, x := range toks {
					if x == "quiet" {
						toks = append(toks[:i+1], append([]string{"loglevel=1"}, toks[i+1:]...)...)
						break
					}
				}
			}
		} else {
			remove(func(x string) bool { return x == "quiet" || strings.HasPrefix(x, "loglevel=") })
		}
	}
	// SSH
	if e.SSH != cur.SSH {
		if e.SSH {
			insertBefore("ssh", "uboot.")
		} else {
			remove(func(x string) bool { return x == "ssh" })
		}
	}
	// UART de depuracion
	if e.ApplyUART && (e.UART == 1 || e.UART == 2) && cur.UART != e.UART {
		first := true
		for i, x := range toks {
			if c := reConsole.FindStringSubmatch(x); c != nil && first {
				toks[i] = fmt.Sprintf("console=ttyS%d%s", e.UART, c[2])
				first = false
			} else if reDebugSh.MatchString(x) {
				toks[i] = fmt.Sprintf("systemd.debug_shell=ttyS%d", e.UART)
			}
		}
	}
	lines[ai] = m[1] + strings.Join(toks, " ")
	return strings.Join(lines, "\n") + "\n", nil
}

// ---------- diff de lineas (LCS) ----------

type DiffLine struct {
	T string `json:"t"` // " ", "-", "+"
	S string `json:"s"`
}

func diffLines(oldT, newT string) []DiffLine {
	a, b := normalize(oldT), normalize(newT)
	n, m := len(a), len(b)
	dp := make([][]int, n+1)
	for i := range dp {
		dp[i] = make([]int, m+1)
	}
	for i := n - 1; i >= 0; i-- {
		for j := m - 1; j >= 0; j-- {
			if a[i] == b[j] {
				dp[i][j] = dp[i+1][j+1] + 1
			} else if dp[i+1][j] >= dp[i][j+1] {
				dp[i][j] = dp[i+1][j]
			} else {
				dp[i][j] = dp[i][j+1]
			}
		}
	}
	var out []DiffLine
	i, j := 0, 0
	for i < n && j < m {
		switch {
		case a[i] == b[j]:
			out = append(out, DiffLine{" ", a[i]})
			i++
			j++
		case dp[i+1][j] >= dp[i][j+1]:
			out = append(out, DiffLine{"-", a[i]})
			i++
		default:
			out = append(out, DiffLine{"+", b[j]})
			j++
		}
	}
	for ; i < n; i++ {
		out = append(out, DiffLine{"-", a[i]})
	}
	for ; j < m; j++ {
		out = append(out, DiffLine{"+", b[j]})
	}
	return out
}

// ---------- ficheros de la tarjeta ----------

func confPath(boot string) string { return filepath.Join(boot, "extlinux", "extlinux.conf") }
func backupDir(boot string) string {
	return filepath.Join(boot, "extlinux", "backups")
}

type Backup struct {
	Name string `json:"name"`
	Time string `json:"time"`
	FDT  string `json:"fdt"`
}

func listBackups(boot string) []Backup {
	ents, err := os.ReadDir(backupDir(boot))
	if err != nil {
		return []Backup{}
	}
	out := []Backup{}
	for _, e := range ents {
		if e.IsDir() || !strings.HasPrefix(e.Name(), "extlinux.conf.") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(backupDir(boot), e.Name()))
		if err != nil {
			continue
		}
		ts := strings.TrimPrefix(e.Name(), "extlinux.conf.")
		if len(ts) > 15 {
			ts = ts[:15]
		}
		t, _ := time.ParseInLocation("20060102-150405", ts, time.Local)
		out = append(out, Backup{Name: e.Name(), Time: t.Format("2006-01-02 15:04:05"), FDT: readState(string(b)).FDT})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name > out[j].Name })
	return out
}

// backupCurrent copia el extlinux.conf actual a backups/ y conserva las 30 mas recientes.
func backupCurrent(boot string) (string, error) {
	cur, err := os.ReadFile(confPath(boot))
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(backupDir(boot), 0o755); err != nil {
		return "", err
	}
	name := "extlinux.conf." + time.Now().Format("20060102-150405")
	base := name
	for n := 2; ; n++ { // dos cambios en el mismo segundo no deben pisarse la copia
		if _, err := os.Stat(filepath.Join(backupDir(boot), name)); os.IsNotExist(err) {
			break
		}
		name = fmt.Sprintf("%s-%d", base, n)
	}
	if err := os.WriteFile(filepath.Join(backupDir(boot), name), cur, 0o644); err != nil {
		return "", err
	}
	bs := listBackups(boot)
	for i := 30; i < len(bs); i++ {
		os.Remove(filepath.Join(backupDir(boot), bs[i].Name))
	}
	return name, nil
}

// writeConf escribe de forma atomica (fichero temporal + renombrado) y verifica releyendo.
func writeConf(boot, text string) error {
	dst := confPath(boot)
	tmp := dst + ".tmp-zenith"
	if err := os.WriteFile(tmp, []byte(text), 0o644); err != nil {
		return err
	}
	f, err := os.OpenFile(tmp, os.O_RDWR, 0)
	if err == nil {
		f.Sync()
		f.Close()
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return err
	}
	got, err := os.ReadFile(dst)
	if err != nil || string(got) != text {
		return fmt.Errorf("la verificacion posterior a la escritura ha fallado")
	}
	return nil
}

// dtbsOnCard devuelve los .dtb del directorio raiz de la tarjeta.
func dtbsOnCard(boot string) []string {
	ents, err := os.ReadDir(boot)
	if err != nil {
		return []string{}
	}
	out := []string{}
	for _, e := range ents {
		if !e.IsDir() && strings.HasSuffix(strings.ToLower(e.Name()), ".dtb") {
			out = append(out, e.Name())
		}
	}
	sort.Strings(out)
	return out
}

func validBoot(boot string) (bool, string) {
	if st, err := os.Stat(confPath(boot)); err != nil || st.IsDir() {
		return false, "missing-extlinux"
	}
	if _, err := os.Stat(filepath.Join(boot, "KERNEL")); err != nil {
		return false, "missing-kernel"
	}
	return true, ""
}
