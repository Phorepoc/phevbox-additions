# Copyright The Phorepoc Authors
# SPDX-License-Identifier: GPL-3.0-only
#
# vbox_drop.ps1 -- HOST side: drop host files on a running VirtualBox VM the way the VM window does when the user drags them over it,
# through the VirtualBox COM API (IGuest.dnDTarget: enter, move, drop, sendData). No drag with the mouse is needed.
#   powershell -File tests\vbox_drop.ps1 <vm> <file-or-folder> [<file-or-folder> ...]
# The VM must run with drag and drop host to guest on (VBoxManage controlvm <vm> draganddrop hosttoguest) and phevboxsvc running in
# the guest. The files then arrive in C:\Data\Received\ and the guest shows a note.
param([string]$Vm, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Paths)
$vb = New-Object -ComObject VirtualBox.VirtualBox
$s = New-Object -ComObject VirtualBox.Session
$m = $vb.FindMachine($Vm)
$m.LockMachine($s, 1)
try {
    $g = $s.Console.Guest.DnDTarget
    $uris = ($Paths | ForEach-Object { "file:///" + ((Resolve-Path $_).Path -replace '\\', '/') }) -join "`r`n"
    $uris += "`r`n"
    $copy = 1                                   # DnDAction_Copy
    $formats = [string[]]@("text/uri-list")
    $allowed = [int[]]@($copy)
    $x = 100; $y = 100
    "enter: " + $g.Enter(0, $x, $y, $copy, $allowed, $formats)
    Start-Sleep -Milliseconds 300
    "move:  " + $g.Move(0, $x + 10, $y + 10, $copy, $allowed, $formats)
    Start-Sleep -Milliseconds 300
    $fmt = ""
    $act = $g.Drop(0, $x + 10, $y + 10, $copy, $allowed, $formats, [ref]$fmt)
    "drop:  action $act, the guest wants '$fmt'"
    if ($act -ne 0) {
        $data = [System.Text.Encoding]::UTF8.GetBytes($uris + [char]0)
        $p = $g.SendData(0, "text/uri-list", $data)
        $p.WaitForCompletion(30000)
        "put data: completed $($p.Completed), result 0x{0:x}" -f $p.ResultCode
    }
}
finally {
    $s.UnlockMachine()
}
