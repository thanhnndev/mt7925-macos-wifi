# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
# Fills itl_ssid / itl_psk (and itl_ssid2.. / itl_psk2.. for further networks, in priority order) in a built itlmt7925.kext
# Info.plist from the Wi-Fi profiles saved on this PC. The passphrases end up in plaintext inside the kext (and therefore on the
# EFI stick); they are never printed here and the repo keeps empty values.
# usage: .\inject-creds.ps1 -Kext Y:\EFI\OC\Kexts\itlmt7925.kext [-Ssid 'HomeWifi','Cafe']   (reading the keys needs an elevated shell)
#   without -Ssid the currently connected profile is used; at most 4 networks; keys of unused slots are emptied.
param([Parameter(Mandatory)][string]$Kext, [string[]]$Ssid)
$plist = Join-Path $Kext 'Contents\Info.plist'
if (-not (Test-Path $plist)) { Write-Host "no Info.plist at $plist"; exit 1 }
if (-not $Ssid) {
    $cur = (netsh wlan show interfaces | Select-String '^\s+SSID\s+:\s+(.+)$' | Select-Object -First 1).Matches.Groups[1].Value.Trim()
    if ($cur) { $Ssid = @($cur) }
}
if (-not $Ssid) { Write-Host "no connected Wi-Fi and no -Ssid given"; exit 1 }
if ($Ssid.Count -gt 4) { Write-Host "at most 4 networks"; exit 1 }
$esc = { param($t) [System.Security.SecurityElement]::Escape($t) }
$txt = [IO.File]::ReadAllText($plist)
for ($i = 1; $i -le 4; $i++) {
    $suffix = if ($i -eq 1) { '' } else { "$i" }
    $name = ''
    $psk = ''
    if ($i -le $Ssid.Count) {
        $name = $Ssid[$i - 1]
        $m = netsh wlan show profile name="$name" key=clear | Select-String 'Key Content\s+:\s+(.+)$'
        if (-not $m) { Write-Host "no key content for profile '$name' (not elevated, or not a PSK profile)"; exit 1 }
        $psk = $m.Matches.Groups[1].Value.Trim()
    }
    $nm = $name; $pk = $psk
    $ks = "itl_ssid$suffix"; $kp = "itl_psk$suffix"
    $rxS = "(<key>$ks</key>\s*<string>)[^<]*(</string>)"
    $rxP = "(<key>$kp</key>\s*<string>)[^<]*(</string>)"
    if (-not [regex]::IsMatch($txt, $rxS)) { Write-Host "Info.plist has no $ks key (old build?)"; exit 1 }
    $txt = [regex]::Replace($txt, $rxS, { param($x) $x.Groups[1].Value + (& $esc $nm) + $x.Groups[2].Value })
    $txt = [regex]::Replace($txt, $rxP, { param($x) $x.Groups[1].Value + (& $esc $pk) + $x.Groups[2].Value })
    if ($name) { Write-Host ("net{0}: '{1}' ({2} chars), passphrase {3} chars" -f $i, $name, $name.Length, $psk.Length) }
}
[IO.File]::WriteAllText($plist, $txt, (New-Object Text.UTF8Encoding($false)))
Write-Host "written to $plist"
