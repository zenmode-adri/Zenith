// Zenith DTB Selector: elige el DTB de una consola RK3326 y reescribe extlinux.conf en la tarjeta SD de Zenith.
package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"time"
)

const version = "1.0.0"

//go:embed web
var webFS embed.FS

type Volume struct {
	Path   string `json:"path"`
	Label  string `json:"label"`
	FS     string `json:"fs"`
	FreeMB int64  `json:"freeMB"`
}

type Device struct {
	ID      string `json:"id"`
	Family  string `json:"family"`
	ZH      string `json:"zh"`
	EN      string `json:"en"`
	ES      string `json:"es"`
	DTB     string `json:"dtb"`
	UART    int    `json:"uart"`
	Overlay string `json:"overlay"`
}

type Catalog struct {
	Families []map[string]string `json:"families"`
	Devices  []Device            `json:"devices"`
}

var (
	catalog  Catalog
	token    string
	lastPing atomic.Int64
	port     int
)

func loadCatalog() {
	b, err := webFS.ReadFile("web/catalog.json")
	if err != nil {
		fatal(err)
	}
	if err := json.Unmarshal(b, &catalog); err != nil {
		fatal(err)
	}
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}

func findDevice(id string) *Device {
	for i := range catalog.Devices {
		if catalog.Devices[i].ID == id {
			return &catalog.Devices[i]
		}
	}
	return nil
}

func main() {
	boot := flag.String("boot", "", "ruta de la particion de arranque (FAT) de la tarjeta; por defecto se autodetecta")
	list := flag.Bool("list", false, "lista los modelos y sale")
	apply := flag.String("apply", "", "aplica el modelo con ese id sin abrir la interfaz (requiere --boot)")
	showBoot := flag.Bool("show-boot", false, "con --apply: mostrar mensajes de arranque en pantalla")
	noBrowser := flag.Bool("no-browser", false, "no abrir el navegador (para pruebas)")
	listen := flag.Int("port", 0, "puerto local (0 = automatico)")
	flag.Parse()
	initPlatform()
	loadCatalog()

	if *list {
		for _, d := range catalog.Devices {
			fmt.Printf("%-44s %-48s uart%d  %s\n", d.ID, d.DTB, d.UART, d.EN)
		}
		return
	}
	if *apply != "" {
		cliApply(*boot, *apply, *showBoot)
		return
	}

	tb := make([]byte, 16)
	rand.Read(tb)
	token = hex.EncodeToString(tb)

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *listen))
	if err != nil {
		fatal(err)
	}
	port = ln.Addr().(*net.TCPAddr).Port
	mux := newMux(*boot)
	go http.Serve(ln, mux)

	url := fmt.Sprintf("http://127.0.0.1:%d/#t=%s", port, token)
	fmt.Printf("Zenith DTB Selector %s\n%s\n", version, url)
	if !*noBrowser {
		openBrowser(url)
	}
	// sale solo cuando la ventana deja de dar señales de vida
	start := time.Now()
	for {
		time.Sleep(2 * time.Second)
		lp := lastPing.Load()
		if lp == 0 {
			if !*noBrowser && time.Since(start) > 3*time.Minute {
				return
			}
			continue
		}
		if time.Since(time.Unix(lp, 0)) > 150*time.Second {
			return
		}
	}
}

func cliApply(boot, id string, showBoot bool) {
	d := findDevice(id)
	if d == nil {
		fatal(fmt.Errorf("modelo desconocido: %s (usa --list)", id))
	}
	if boot == "" {
		vs := detectVolumes()
		if len(vs) != 1 {
			fatal(fmt.Errorf("indica --boot (tarjetas detectadas: %d)", len(vs)))
		}
		boot = vs[0].Path
	}
	if ok, why := validBoot(boot); !ok {
		fatal(fmt.Errorf("%s no parece una tarjeta Zenith (%s)", boot, why))
	}
	cur, err := os.ReadFile(confPath(boot))
	if err != nil {
		fatal(err)
	}
	st := readState(string(cur))
	e := Edit{DTB: d.DTB, UART: d.UART, ApplyUART: true, HideBoot: !showBoot, SSH: st.SSH}
	if !showBoot && !flagWasSet("show-boot") {
		e.HideBoot = st.HideBoot
	}
	nt, err := applyEdit(string(cur), e)
	if err != nil {
		fatal(err)
	}
	if _, err := os.Stat(filepath.Join(boot, d.DTB)); err != nil {
		fatal(fmt.Errorf("el DTB %s no esta en la tarjeta", d.DTB))
	}
	name, err := backupCurrent(boot)
	if err != nil {
		fatal(err)
	}
	if err := writeConf(boot, nt); err != nil {
		fatal(err)
	}
	fmt.Printf("OK: %s -> %s (copia: extlinux/backups/%s)\n", d.EN, d.DTB, name)
}

func flagWasSet(name string) bool {
	set := false
	flag.Visit(func(f *flag.Flag) {
		if f.Name == name {
			set = true
		}
	})
	return set
}

func openBrowser(url string) {
	switch runtime.GOOS {
	case "windows":
		for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles"} {
			base := os.Getenv(env)
			if base == "" {
				continue
			}
			for _, p := range []string{
				filepath.Join(base, "Microsoft", "Edge", "Application", "msedge.exe"),
				filepath.Join(base, "Google", "Chrome", "Application", "chrome.exe"),
			} {
				if _, err := os.Stat(p); err == nil {
					if exec.Command(p, "--app="+url, "--window-size=1120,780").Start() == nil {
						return
					}
				}
			}
		}
		exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	case "darwin":
		exec.Command("open", url).Start()
	default:
		for _, b := range []string{"google-chrome", "chromium", "chromium-browser", "microsoft-edge"} {
			if p, err := exec.LookPath(b); err == nil {
				if exec.Command(p, "--app="+url, "--window-size=1120,780").Start() == nil {
					return
				}
			}
		}
		exec.Command("xdg-open", url).Start()
	}
}

func staticFS() http.Handler {
	sub, _ := fs.Sub(webFS, "web")
	return http.FileServer(http.FS(sub))
}

func hostOK(r *http.Request) bool {
	h := r.Host
	return h == fmt.Sprintf("127.0.0.1:%d", port) || h == fmt.Sprintf("localhost:%d", port)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func jerr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func newMux(forced string) http.Handler {
	mux := http.NewServeMux()
	static := staticFS()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if !hostOK(r) {
			http.Error(w, "forbidden", 403)
			return
		}
		static.ServeHTTP(w, r)
	})
	api := func(h func(http.ResponseWriter, *http.Request)) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			if !hostOK(r) || r.Header.Get("X-Zenith-Token") != token {
				jerr(w, 403, "forbidden")
				return
			}
			lastPing.Store(time.Now().Unix())
			h(w, r)
		}
	}
	mux.HandleFunc("/api/ping", api(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/info", api(func(w http.ResponseWriter, r *http.Request) {
		vols := detectVolumes()
		if forced != "" {
			if ok, _ := validBoot(forced); ok {
				vols = []Volume{{Path: forced, Label: filepath.Base(forced), FS: "FAT", FreeMB: 0}}
			}
		}
		if vols == nil {
			vols = []Volume{}
		}
		writeJSON(w, 200, map[string]any{"version": version, "os": runtime.GOOS, "volumes": vols})
	}))
	mux.HandleFunc("/api/volume", api(func(w http.ResponseWriter, r *http.Request) {
		boot := r.URL.Query().Get("path")
		if ok, why := validBoot(boot); !ok {
			jerr(w, 400, why)
			return
		}
		b, err := os.ReadFile(confPath(boot))
		if err != nil {
			jerr(w, 500, err.Error())
			return
		}
		st := readState(string(b))
		match := []string{}
		for _, d := range catalog.Devices {
			if d.DTB == st.FDT {
				match = append(match, d.ID)
			}
		}
		writeJSON(w, 200, map[string]any{
			"state": st, "match": match, "text": string(b),
			"dtbs": dtbsOnCard(boot), "backups": listBackups(boot),
		})
	}))
	type req struct {
		Path   string `json:"path"`
		Device string `json:"device"`
		DTB    string `json:"dtb"`
		Opts   Edit   `json:"opts"`
		Backup string `json:"backup"`
	}
	build := func(rq req) (string, string, []string, error) {
		if ok, why := validBoot(rq.Path); !ok {
			return "", "", nil, fmt.Errorf("%s", why)
		}
		cur, err := os.ReadFile(confPath(rq.Path))
		if err != nil {
			return "", "", nil, err
		}
		e := rq.Opts
		var warns []string
		if rq.Device != "" {
			d := findDevice(rq.Device)
			if d == nil {
				return "", "", nil, fmt.Errorf("modelo desconocido")
			}
			e.DTB = d.DTB
			e.UART = d.UART
		} else if rq.DTB != "" {
			e.DTB = rq.DTB
		}
		if e.DTB != "" {
			if _, err := os.Stat(filepath.Join(rq.Path, e.DTB)); err != nil {
				warns = append(warns, "dtb-missing")
			}
		}
		nt, err := applyEdit(string(cur), e)
		return string(cur), nt, warns, err
	}
	mux.HandleFunc("/api/preview", api(func(w http.ResponseWriter, r *http.Request) {
		var rq req
		json.NewDecoder(r.Body).Decode(&rq)
		old, nt, warns, err := build(rq)
		if err != nil {
			jerr(w, 400, err.Error())
			return
		}
		writeJSON(w, 200, map[string]any{"diff": diffLines(old, nt), "changed": old != nt, "warnings": warns})
	}))
	mux.HandleFunc("/api/apply", api(func(w http.ResponseWriter, r *http.Request) {
		var rq req
		json.NewDecoder(r.Body).Decode(&rq)
		old, nt, warns, err := build(rq)
		if err != nil {
			jerr(w, 400, err.Error())
			return
		}
		for _, x := range warns {
			if x == "dtb-missing" {
				jerr(w, 400, "dtb-missing")
				return
			}
		}
		if old == nt {
			writeJSON(w, 200, map[string]any{"ok": true, "changed": false})
			return
		}
		name, err := backupCurrent(rq.Path)
		if err != nil {
			jerr(w, 500, "backup: "+err.Error())
			return
		}
		if err := writeConf(rq.Path, nt); err != nil {
			jerr(w, 500, err.Error())
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true, "changed": true, "backup": name})
	}))
	mux.HandleFunc("/api/restore", api(func(w http.ResponseWriter, r *http.Request) {
		var rq req
		json.NewDecoder(r.Body).Decode(&rq)
		if ok, why := validBoot(rq.Path); !ok {
			jerr(w, 400, why)
			return
		}
		if strings.ContainsAny(rq.Backup, `/\`) || !strings.HasPrefix(rq.Backup, "extlinux.conf.") {
			jerr(w, 400, "bad backup name")
			return
		}
		b, err := os.ReadFile(filepath.Join(backupDir(rq.Path), rq.Backup))
		if err != nil {
			jerr(w, 404, err.Error())
			return
		}
		if _, err := backupCurrent(rq.Path); err != nil {
			jerr(w, 500, err.Error())
			return
		}
		if err := writeConf(rq.Path, strings.Join(normalize(string(b)), "\n")+"\n"); err != nil {
			jerr(w, 500, err.Error())
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/import", api(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jerr(w, 405, "method")
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, maxDTBSize+1)
		data, err := io.ReadAll(r.Body)
		if err != nil {
			jerr(w, 400, "bad-dtb")
			return
		}
		name, soc, existed, err := importDTB(r.URL.Query().Get("path"), r.Header.Get("X-Filename"), data, r.Header.Get("X-Overwrite") == "1")
		if err != nil {
			code := 400
			if err.Error() == "dtb-exists" {
				code = 409
			}
			jerr(w, code, err.Error())
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true, "name": name, "soc": soc, "overwritten": existed})
	}))
	mux.HandleFunc("/api/quit", api(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"ok": true})
		go func() { time.Sleep(300 * time.Millisecond); os.Exit(0) }()
	}))
	return mux
}
