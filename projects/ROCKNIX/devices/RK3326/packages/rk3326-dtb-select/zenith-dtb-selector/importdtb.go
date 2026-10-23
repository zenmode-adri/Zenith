package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const maxDTBSize = 2 << 20 // un DTB real pesa ~60-100 KB

var reSafeName = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,62}\.dtb$`)

// safeDTBName devuelve solo el nombre base y comprueba que es seguro para la raiz de la FAT (y para el parser de u-boot).
func safeDTBName(raw string) (string, error) {
	n, err := url.PathUnescape(raw)
	if err != nil {
		return "", fmt.Errorf("bad-name")
	}
	n = filepath.Base(strings.ReplaceAll(n, "\\", "/"))
	if !reSafeName.MatchString(n) {
		return "", fmt.Errorf("bad-name")
	}
	return n, nil
}

// checkDTB valida la cabecera FDT (magic 0xd00dfeed, tamaño coherente) y avisa si no parece de un RK3326.
func checkDTB(b []byte) (isRK3326 bool, err error) {
	if len(b) < 40 || len(b) > maxDTBSize {
		return false, fmt.Errorf("bad-dtb")
	}
	if binary.BigEndian.Uint32(b[0:4]) != 0xd00dfeed {
		return false, fmt.Errorf("bad-dtb")
	}
	total := binary.BigEndian.Uint32(b[4:8])
	if total < 40 || int(total) > len(b) {
		return false, fmt.Errorf("bad-dtb")
	}
	return bytes.Contains(b[:total], []byte("rk3326")), nil
}

// importDTB copia un DTB propio a la raiz de la tarjeta (escritura atomica). No pisa uno existente salvo overwrite.
func importDTB(boot, rawName string, data []byte, overwrite bool) (name string, isRK3326, existed bool, err error) {
	if ok, why := validBoot(boot); !ok {
		return "", false, false, fmt.Errorf("%s", why)
	}
	name, err = safeDTBName(rawName)
	if err != nil {
		return "", false, false, err
	}
	isRK3326, err = checkDTB(data)
	if err != nil {
		return "", false, false, err
	}
	dst := filepath.Join(boot, name)
	if _, e := os.Stat(dst); e == nil {
		existed = true
		if !overwrite {
			return name, isRK3326, true, fmt.Errorf("dtb-exists")
		}
	}
	tmp := dst + ".tmp-zenith"
	if err = os.WriteFile(tmp, data, 0o644); err != nil {
		return name, isRK3326, existed, err
	}
	if err = os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return name, isRK3326, existed, err
	}
	got, err := os.ReadFile(dst)
	if err != nil || !bytes.Equal(got, data) {
		return name, isRK3326, existed, fmt.Errorf("la verificacion posterior a la escritura ha fallado")
	}
	return name, isRK3326, existed, nil
}
