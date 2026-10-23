package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Modo de prueba de DTB: ver firstboot/dtb-probe.sh (consola). Aqui solo se prepara y se cancela desde el PC.
// Estado en la FAT: dtbprobe/{state,list,index,extlinux.orig,attempts,log} y, al terminar, dtbprobe-last.txt.

type ProbeCand struct {
	DTB   string   `json:"dtb"`
	Group []string `json:"group"`
	ICC   *int     `json:"icc"`
	Names []struct {
		EN string `json:"en"`
		ES string `json:"es"`
		ZH string `json:"zh"`
	} `json:"names"`
}

type ProbeOrder struct {
	Candidates []ProbeCand `json:"candidates"`
}

type ProbeStatus struct {
	Active  bool     `json:"active"`
	Index   int      `json:"index"`
	Total   int      `json:"total"`
	Current string   `json:"current"`
	List    []string `json:"list"`
	Last    string   `json:"last"`
}

var probeOrder ProbeOrder

func loadProbeOrder() {
	b, err := webFS.ReadFile("web/probe-order.json")
	if err != nil {
		return
	}
	json.Unmarshal(b, &probeOrder)
}

func probeDir(boot string) string  { return filepath.Join(boot, "dtbprobe") }
func probeLast(boot string) string { return filepath.Join(boot, "dtbprobe-last.txt") }

func readLines(path string) []string {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var out []string
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			out = append(out, l)
		}
	}
	return out
}

// probeSupported: la imagen de la tarjeta lleva el script de la consola (mkimage escribe zenith-features.txt en la FAT).
// Sin esa marca, activar el modo dejaria la consola probando siempre el primer candidato (nada lo avanza ni lo confirma).
func probeSupported(boot string) bool {
	for _, l := range readLines(filepath.Join(boot, "zenith-features.txt")) {
		if l == "dtb-probe=1" {
			return true
		}
	}
	return false
}

func probeStatus(boot string) ProbeStatus {
	st := ProbeStatus{List: []string{}}
	if _, err := os.Stat(filepath.Join(probeDir(boot), "state")); err == nil {
		st.Active = true
		st.List = readLines(filepath.Join(probeDir(boot), "list"))
		st.Total = len(st.List)
		if ix := readLines(filepath.Join(probeDir(boot), "index")); len(ix) > 0 {
			fmt.Sscanf(ix[0], "%d", &st.Index)
		}
		if st.Index >= 0 && st.Index < st.Total {
			st.Current = st.List[st.Index]
		}
	}
	if l := readLines(probeLast(boot)); len(l) > 0 {
		st.Last = l[0]
	}
	return st
}

// probeCandidates devuelve los DTB del orden por defecto que existen en la tarjeta.
func probeCandidates(boot string) []string {
	have := map[string]bool{}
	for _, d := range dtbsOnCard(boot) {
		have[d] = true
	}
	var out []string
	for _, c := range probeOrder.Candidates {
		if have[c.DTB] {
			out = append(out, c.DTB)
		}
	}
	return out
}

// probeStart prepara el modo de prueba: copia de seguridad, marca, lista, extlinux.orig y FDT = primer candidato.
// Solo se cambia la linea FDT (las demas opciones de extlinux.conf se respetan).
func probeStart(boot string, list []string, force bool) (int, error) {
	if ok, why := validBoot(boot); !ok {
		return 0, fmt.Errorf("%s", why)
	}
	if !force && !probeSupported(boot) {
		return 0, fmt.Errorf("probe-unsupported")
	}
	if st := probeStatus(boot); st.Active {
		return 0, fmt.Errorf("probe-active")
	}
	have := map[string]bool{}
	for _, d := range dtbsOnCard(boot) {
		have[d] = true
	}
	var use []string
	seen := map[string]bool{}
	for _, d := range list {
		if have[d] && !seen[d] && reSafeName.MatchString(d) {
			use = append(use, d)
			seen[d] = true
		}
	}
	if len(use) < 2 {
		return 0, fmt.Errorf("probe-few")
	}
	cur, err := os.ReadFile(confPath(boot))
	if err != nil {
		return 0, err
	}
	st := readState(string(cur))
	nt, err := applyEdit(string(cur), Edit{DTB: use[0], HideBoot: st.HideBoot, SSH: st.SSH})
	if err != nil {
		return 0, err
	}
	if _, err := backupCurrent(boot); err != nil {
		return 0, err
	}
	pd := probeDir(boot)
	if err := os.MkdirAll(pd, 0o755); err != nil {
		return 0, err
	}
	files := map[string]string{
		"extlinux.orig": string(cur),
		"list":          strings.Join(use, "\n") + "\n",
		"index":         "0\n",
	}
	for n, c := range files {
		if err := os.WriteFile(filepath.Join(pd, n), []byte(c), 0o644); err != nil {
			os.RemoveAll(pd)
			return 0, err
		}
	}
	os.Remove(probeLast(boot))
	if err := writeConf(boot, nt); err != nil {
		os.RemoveAll(pd)
		return 0, err
	}
	// la marca va la ULTIMA: sin ella la consola ignora todo lo anterior
	if err := os.WriteFile(filepath.Join(pd, "state"), []byte("active\n"), 0o644); err != nil {
		os.RemoveAll(pd)
		os.WriteFile(confPath(boot), cur, 0o644)
		return 0, err
	}
	return len(use), nil
}

// probeStop cancela: restaura el extlinux.conf original y borra el modo de prueba.
func probeStop(boot string) error {
	if ok, why := validBoot(boot); !ok {
		return fmt.Errorf("%s", why)
	}
	pd := probeDir(boot)
	if _, err := os.Stat(pd); err != nil {
		return nil
	}
	orig, err := os.ReadFile(filepath.Join(pd, "extlinux.orig"))
	if err == nil && len(orig) > 0 {
		if _, err := backupCurrent(boot); err != nil {
			return err
		}
		if err := writeConf(boot, strings.Join(normalize(string(orig)), "\n")+"\n"); err != nil {
			return err
		}
	}
	return os.RemoveAll(pd)
}
