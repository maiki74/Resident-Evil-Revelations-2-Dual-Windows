param([switch]$Restore,[string]$GameDirectory)
$ErrorActionPreference='Stop'
if(Get-Process rerev2 -ErrorAction SilentlyContinue){throw 'Close the game normally before installing or restoring.'}
if(!$GameDirectory){
    $steam=(Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if($steam){
        $libraries=@($steam)
        $vdf=Join-Path $steam 'steamapps\libraryfolders.vdf'
        if(Test-Path -LiteralPath $vdf){
            $libraries+=@([regex]::Matches((Get-Content -LiteralPath $vdf -Raw),'"path"\s+"([^"]+)"') |
                ForEach-Object {$_.Groups[1].Value.Replace('\\','\')})
        }
        $candidates=@($libraries | Where-Object {Test-Path -LiteralPath $_ -PathType Container} | ForEach-Object {
            $manifest=Join-Path $_ 'steamapps\appmanifest_287290.acf'
            if(Test-Path -LiteralPath $manifest){
                $name=[regex]::Match((Get-Content -LiteralPath $manifest -Raw),'"installdir"\s+"([^"]+)"').Groups[1].Value
                if($name){
                    $folder=Join-Path (Join-Path $_ 'steamapps\common') $name
                    if(Test-Path -LiteralPath (Join-Path $folder 'rerev2.exe')){(Resolve-Path -LiteralPath $folder).Path}
                }
            }
        } | Sort-Object -Unique)
        if($candidates.Count -eq 1){$GameDirectory=$candidates[0]}
    }
    if(!$GameDirectory){$GameDirectory=([string](Read-Host 'Paste the game folder containing rerev2.exe')).Trim().Trim('"')}
}
if(!$GameDirectory -or !(Test-Path -LiteralPath (Join-Path $GameDirectory 'rerev2.exe'))){throw 'Select the game folder containing rerev2.exe.'}
$game=(Resolve-Path -LiteralPath $GameDirectory).Path
$scripts=Join-Path $game 'scripts'
$plugin=Join-Path $scripts 'ResidentEvilRevelations2.DualWindows.asi'
$backup=Join-Path $scripts 'DualWindowsV2Backup'
$config=Join-Path $env:LOCALAPPDATA 'CAPCOM\RESIDENT EVIL REVELATIONS2\config.ini'
if(!(Test-Path -LiteralPath $config)){throw 'Start the game once and exit normally to create its settings file.'}
if($Restore){
    if(!(Test-Path -LiteralPath (Join-Path $backup 'config.ini'))){throw 'The DualWindows backup is missing.'}
    $old=Get-Content -LiteralPath (Join-Path $backup 'config.ini') -Raw
    $full=[regex]::Match($old,'(?m)^FullScreen=([^\r\n]*)').Groups[1].Value
    if($full -notin @('ON','OFF')){throw 'The backup does not contain a valid fullscreen setting.'}
    $text=Get-Content -LiteralPath $config -Raw
    $text=[regex]::Replace($text,'(?m)^FullScreen=[^\r\n]*',('FullScreen='+$full))
    if(Test-Path -LiteralPath $plugin){Move-Item -LiteralPath $plugin -Destination ($plugin+'.disabled') -Force}
    [IO.File]::WriteAllText($config,$text,[Text.Encoding]::ASCII)
    Write-Output 'DualWindows disabled; previous fullscreen setting restored. FusionFix and saves were not modified.'
    exit
}
if((Get-FileHash -LiteralPath (Join-Path $game 'rerev2.exe')).Hash -ne '55AC7D2BD80704C6A8A1FA7BD2371726ADBAA01AE0FDA056198FB042404367F9'){throw 'Unsupported game executable.'}
$fusion=Join-Path $scripts 'ResidentEvilRevelations2.FusionFix.asi'
if(!(Test-Path -LiteralPath $fusion) -or !(Test-Path -LiteralPath (Join-Path $game 'dinput8.dll'))){throw 'Install the supplied ResidentEvilRevelations2.FusionFix.zip in the game folder first.'}
if((Get-FileHash -LiteralPath $fusion).Hash -ne 'F100DD3CD36FDA17B28811458B85EB6C10788087FFDDC4B989FA2D98E76C1BDF'){throw 'This build needs the matching FusionFix from ResidentEvilRevelations2.FusionFix.zip.'}
$payload=Join-Path $PSScriptRoot 'ResidentEvilRevelations2.DualWindows.asi'
if(!(Test-Path -LiteralPath $payload)){throw 'Plugin file missing.'}
$text=Get-Content -LiteralPath $config -Raw
$res=[regex]::Match($text,'(?m)^Resolution=(\d+)x(\d+)')
if(!$res.Success -or [int]$res.Groups[1].Value*9 -ne [int]$res.Groups[2].Value*16){throw 'Choose a normal 16:9 game resolution before installing this build.'}
New-Item -ItemType Directory -Force -Path $backup | Out-Null
if(!(Test-Path -LiteralPath (Join-Path $backup 'config.ini'))){Copy-Item -LiteralPath $config -Destination (Join-Path $backup 'config.ini')}
$text=[regex]::Replace($text,'(?m)^FullScreen=[^\r\n]*','FullScreen=OFF')
Copy-Item -LiteralPath $payload -Destination $plugin -Force
$uninstaller=Join-Path $PSScriptRoot 'Uninstall_DualWindows.cmd'
if(Test-Path -LiteralPath $uninstaller){Copy-Item -LiteralPath $uninstaller -Destination (Join-Path $scripts 'Uninstall_DualWindows.cmd') -Force}
[IO.File]::WriteAllText($config,$text,[Text.Encoding]::ASCII)
Write-Output "DualWindows v0.6.2 installed in $game. Launch through Steam."






