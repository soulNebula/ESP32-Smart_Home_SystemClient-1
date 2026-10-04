# =============================================================================
#  tools/flash.ps1  --  Reliable flash for this board
# =============================================================================
#  WHY THIS SCRIPT EXISTS (real hardware finding, 2026-09-30)
#  ----------------------------------------------------------
#  Plain `idf.py flash` (and therefore tools/build.ps1 -Task flash) FAILS on
#  this board:  the large application image (1.95 MB) is written but never
#  verifies:
#
#      0x0      bootloader.bin        22368 B  ->  Hash of data verified.   OK
#      0x20000  esp32_smart_home.bin 1954992 B  ->  A fatal error occurred:
#                                                   MD5 of file does not match
#                                                   data in flash!
#
#  What the board does then: the app partition holds wrong bytes, the 2nd stage
#  bootloader reports
#      E (421) esp_image: Checksum failed. Calculated 0x2 read 0x71
#      E (422) boot: OTA app partition slot 0 is not bootable
#      E (440) boot: No bootable app partitions in the partition table
#  and it reboots forever.
#
#  WHAT IS **NOT** THE CAUSE (things that were measured and ruled out)
#    * the image FILE is fine: `esptool image_info` says "Checksum: 71 (valid)"
#      and "Validation Hash: ... (valid)"; the bootloader reads exactly that
#      same 0x71 from the header - only the flash CONTENT disagrees
#    * baud rate: identical failure at 460800, 230400 and 115200
#    * flaky transfer: the wrong "Flash md5" is byte-identical on every retry
#    * compression: `--no-compress` ALONE still fails (measured), so esptool's
#      compressed transfer is NOT sufficient explanation either
#
#  WHAT THE EVIDENCE ACTUALLY SHOWS
#    Every attempt that started with a FULL CHIP ERASE succeeded; every attempt
#    that did not, failed.  6 out of 6 observations:
#        00:17  erase-flash then write        -> OK
#        00:25  write only                    -> FAIL
#        00:27  write only                    -> FAIL
#        00:29  write only                    -> FAIL
#        00:32  erase-flash then write        -> OK
#        00:37  write only                    -> FAIL
#    Conclusion: the per-region sector erase that esptool performs as part of
#    write_flash ("Flash will be erased from 0x00020000 to 0x001fdfff...") is
#    not reliable on this board, so programming over previously-written pages
#    produces corrupt data.  A chip erase first fixes it.  (The exact silicon
#    reason is not proven - what is proven is the procedure.)
#
#  So this script: writes uncompressed, and if verification fails WITHOUT an
#  erase it automatically retries once with a full chip erase.
#
#  USAGE
#  -----
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM31
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Erase     # force chip erase first
#    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Baud 115200
#
#  NOTE: this script is deliberately pure ASCII.  Windows PowerShell 5.1 reads
#        a BOM-less UTF-8 .ps1 as ANSI, which mangles non-ASCII text - keeping
#        the file ASCII removes that entire class of failure.
# =============================================================================

[CmdletBinding()]
param(
    [string] $Port     = 'COM31',
    [int]    $Baud     = 460800,
    [string] $BuildDir = 'C:\esp32_smart_home\build',
    [switch] $Erase
)

$ErrorActionPreference = 'Continue'

function Say([string]$msg, [string]$color = 'Gray') {
    Write-Host $msg -ForegroundColor $color
}
function Strip([string]$s) { return ($s -replace "\x1b\[[0-9;]*m", "") }

Say ""
Say "=== flash.ps1 : uncompressed flash, auto-retry with chip erase ===" Cyan
Say "    port      : $Port"
Say "    baud      : $Baud"
Say "    build dir : $BuildDir"
Say "    force erase: $(if ($Erase) { 'YES' } else { 'no (auto-retry if verify fails)' })"
Say ""

# --- 1. sanity: build artifacts must exist ---------------------------------
$flashArgs = Join-Path $BuildDir 'flash_args'
if (-not (Test-Path $flashArgs)) {
    Say "[FAIL] $flashArgs not found." Red
    Say "       Build first:  powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Task build" Red
    exit 2
}
$argsTxt = [System.IO.File]::ReadAllText($flashArgs, [System.Text.Encoding]::UTF8)
$images  = ([regex]::Matches($argsTxt, '(?m)^\s*(0x[0-9a-fA-F]+)\s+(\S+)\s*$'))
Say "  images to write:"
foreach ($m in $images) {
    $p      = Join-Path $BuildDir $m.Groups[2].Value
    $exists = Test-Path $p
    $len    = if ($exists) { (Get-Item $p).Length } else { 0 }
    Say ("    {0,-10} {1,-34} {2,10} B  {3}" -f `
            $m.Groups[1].Value, $m.Groups[2].Value, $len, $(if ($exists) { 'OK' } else { 'MISSING' })) `
        $(if ($exists) { 'Gray' } else { 'Red' })
    if (-not $exists) { Say "[FAIL] missing image, aborting." Red; exit 2 }
}
Say ""

# --- 2. locate the ESP-IDF python (it has esptool installed) ---------------
$pyCandidates = @(
    'E:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe',
    (Join-Path $env:IDF_TOOLS_PATH 'python_env\idf5.4_py3.11_env\Scripts\python.exe')
)
$py = $null
foreach ($c in $pyCandidates) { if ($c -and (Test-Path $c)) { $py = $c; break } }
if (-not $py) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $py = $cmd.Source }
}
if (-not $py) {
    Say "[FAIL] could not find a python with esptool installed." Red
    exit 2
}
Say "  python    : $py"
Say ""

# --- 3. refuse to touch a busy port ---------------------------------------
#     Opening a port just to probe it is what wedges a concurrent esptool run,
#     so we only LIST ports (never open them) and bail out if another python
#     may be flashing right now.
$busy = @(Get-Process -Name python -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
    Say "[WARN] other python processes are running - is something else flashing?" Yellow
    foreach ($b in $busy) { Say ("        PID {0} started {1}" -f $b.Id, $b.StartTime) Yellow }
    Say "        aborting to avoid a corrupted write." Yellow
    exit 3
}
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
Say ("  serial ports: {0}" -f ($ports -join ', '))
if ($ports -notcontains $Port) {
    Say "[FAIL] $Port is not present." Red
    Say "       Unplug/replug the board, then check Device Manager." Red
    exit 4
}
Say ""

# --- 4. write (optionally erase first; auto-retry with erase) -------------
$attempts = if ($Erase) { @($true) } else { @($false, $true) }
$code = 0; $hashN = 0; $fatal = $false
$out = @(); $usedErase = $false; $ok = $false

Push-Location $BuildDir
try {
    for ($i = 0; $i -lt $attempts.Count; $i++) {
        $doErase = $attempts[$i]

        if ($doErase) {
            Say "=== erase_flash (full chip erase) ===" Cyan
            $er = & $py -m esptool --chip esp32s3 -p $Port -b $Baud `
                        --before default_reset --after no_reset erase_flash 2>&1
            $er | ForEach-Object { Write-Host ("  " + (Strip $_)) }
            if ($LASTEXITCODE -ne 0) {
                Say "[FAIL] erase_flash failed." Red
                $code = $LASTEXITCODE
                break
            }
            $usedErase = $true
            Say ""
        }

        Say ("=== write_flash --no-compress @flash_args   (attempt {0}/{1}{2}) ===" -f `
                ($i + 1), $attempts.Count, $(if ($doErase) { ', after chip erase' } else { '' })) Cyan
        $out = & $py -m esptool --chip esp32s3 -p $Port -b $Baud `
                    --before default_reset --after hard_reset `
                    write_flash --no-compress '@flash_args' 2>&1
        $code = $LASTEXITCODE
        $out | ForEach-Object { Write-Host ("  " + (Strip $_)) }

        $text  = ($out | Out-String)
        $hashN = ([regex]::Matches($text, 'Hash of data verified')).Count
        $fatal = ($text -match 'A fatal error occurred') -or ($text -match 'MD5 of file does not match')

        if (($code -eq 0) -and ($hashN -ge 5) -and (-not $fatal)) { $ok = $true; break }

        if (-not $doErase) {
            Say ""
            Say "[WARN] write did NOT verify without a chip erase." Yellow
            Say "[WARN] This is the known failure mode of this board (see header)." Yellow
            Say "[WARN] Retrying once WITH a full chip erase ..." Yellow
            Say ""
        }
    }
}
finally {
    Pop-Location
}

# --- 5. verdict ------------------------------------------------------------
Say ""
if ($ok) {
    Say ("=== FLASH OK : {0}/5 images verified{1} ===" -f `
            $hashN, $(if ($usedErase) { ' (after chip erase)' } else { '' })) Green
    Say "    next:  powershell -ExecutionPolicy Bypass -File tools\monitor.ps1 -Port $Port -Seconds 45 -Reset"
    exit 0
}
Say "[FAIL] flashing did not verify." Red
Say ("       exit code                     = {0}" -f $code) Red
Say ("       'Hash of data verified' lines  = {0}  (expected 5)" -f $hashN) Red
Say ("       chip erase was used           = {0}" -f $usedErase) Red
if ($fatal -and $usedErase) {
    Say "       Still an MD5 mismatch even AFTER a chip erase.  Next things to" Red
    Say "       try, in order:" Red
    Say "         1. a different USB cable / a powered USB hub (power dips)" Red
    Say "         2. -Baud 115200" Red
    Say "         3. a different serial adapter (CH340 vs CP210x vs native USB)" Red
}
exit 6
