//go:build windows

package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

var (
	k32                    = syscall.NewLazyDLL("kernel32.dll")
	pGetLogicalDrives      = k32.NewProc("GetLogicalDrives")
	pGetVolumeInformationW = k32.NewProc("GetVolumeInformationW")
	pGetDiskFreeSpaceExW   = k32.NewProc("GetDiskFreeSpaceExW")
	pSetErrorMode          = k32.NewProc("SetErrorMode")
)

func initPlatform() {
	pSetErrorMode.Call(1) // SEM_FAILCRITICALERRORS: sin dialogos de "inserte un disco"
}

func detectVolumes() []Volume {
	var out []Volume
	mask, _, _ := pGetLogicalDrives.Call()
	for i := 0; i < 26; i++ {
		if mask&(1<<uint(i)) == 0 {
			continue
		}
		root := fmt.Sprintf("%c:\\", 'A'+i)
		if ok, _ := validBoot(root); !ok {
			continue
		}
		v := Volume{Path: root}
		rp, _ := syscall.UTF16PtrFromString(root)
		var label [261]uint16
		var fs [261]uint16
		r, _, _ := pGetVolumeInformationW.Call(uintptr(unsafe.Pointer(rp)), uintptr(unsafe.Pointer(&label[0])), 261, 0, 0, 0, uintptr(unsafe.Pointer(&fs[0])), 261)
		if r != 0 {
			v.Label = syscall.UTF16ToString(label[:])
			v.FS = syscall.UTF16ToString(fs[:])
		}
		var free, total, tfree uint64
		r, _, _ = pGetDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(rp)), uintptr(unsafe.Pointer(&free)), uintptr(unsafe.Pointer(&total)), uintptr(unsafe.Pointer(&tfree)))
		if r != 0 {
			v.FreeMB = int64(free >> 20)
		}
		out = append(out, v)
	}
	return out
}
