# Builds the game's code library for the MH4U Android app from YOUR Monster Hunter 4 Ultimate USA v1.3.0
# update (.cia), on this PC. The first time it downloads the C compiler (llvm-mingw, about 180 MB); no Android
# tools are needed. Everything it makes from the game stays on this PC and in the folder you choose.
#
# Double-click "Build MH4U library for Android.bat", or:
#   powershell -ExecutionPolicy Bypass -File build-android-library.ps1 [-Cia "path\to\update.cia"]
#       [-Game "path\to\game.3ds"] [-Mods "path\to\mods"]
#
# The game itself (.3ds or .cci) is optional but makes a faster library: its geometry shaders are compiled into it
# (build_game.py --romfs). One beside the .cia is found by itself; otherwise the script asks for it.
#
# The library, libmh4u_code_native.so, is written next to the .cia: put it in the phone's folder with the game and
# the update. A mod that changes the game's code (the single-screen mod does) has to be built in: choose the same
# mods folder you put on the phone (it holds 0004000000126300, or a mod's folder with 0004000000126300 inside), or the
# mod's own folder (the one with exefs in it), as the app's Mods page takes it.
param([string]$Cia = "", [string]$Game = "", [string]$Mods = "", [switch]$NoMods, [switch]$NoGame, [switch]$NoPause)
$ErrorActionPreference = "Stop"
$es = (Get-Culture).TwoLetterISOLanguageName -eq "es"
function T($spanish, $english) { if ($es) { $spanish } else { $english } }
function Finish($code) {
    if (-not $NoPause) { Read-Host (T "Pulsa Enter para cerrar" "Press Enter to close") | Out-Null }
    exit $code
}
# (an error nobody expected: say it and wait, instead of a window that closes before it can be read)
trap {
    Write-Host ""
    Write-Host ((T "Algo salió mal: " "Something went wrong: ") + $_)
    Write-Host (T "Si pides ayuda, copia este texto." "If you ask for help, copy this text.")
    Finish 1
}

$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tool = Join-Path $root "tools\build_game.py"
if (-not (Test-Path $tool)) {
    Write-Host "Can't find $tool"
    Write-Host (T "Descomprime ENTERA la descarga (clic derecho en el ZIP, Extraer todo) y abre esto desde mh4n\tools\android." `
                  "Unzip ALL of the download (right-click the ZIP, Extract All) and run this from mh4n\tools\android.")
    Finish 1
}

# Python: the "py" launcher from python.org, or a "python" that really runs (the Microsoft Store alias without
# Python installed doesn't)
$python = $null
foreach ($candidate in @(@("py", "-3"), @("python"))) {
    if (Get-Command $candidate[0] -ErrorAction SilentlyContinue) {
        $exe = $candidate[0]
        $pre = @($candidate | Select-Object -Skip 1)
        try { & $exe @pre --version *> $null } catch { continue }
        if ($LASTEXITCODE -eq 0) { $python = $candidate; break }
    }
}
if (-not $python) {
    Write-Host (T "Hace falta Python 3.10 o más nuevo: instálalo desde https://www.python.org/downloads/" `
                  "Python 3.10 or newer is needed: install it from https://www.python.org/downloads/")
    Write-Host (T "(marca 'Add python.exe to PATH') y vuelve a abrir esto." "(tick 'Add python.exe to PATH') and run this again.")
    Finish 1
}

Add-Type -AssemblyName System.Windows.Forms
if (-not $Cia) {
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = (T "Tu actualización 1.3.0 de MH4U (USA), descifrada (.cia)" "Your decrypted MH4U USA v1.3.0 update (.cia)")
    $dialog.Filter = "CIA (*.cia)|*.cia|All files (*.*)|*.*"
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Write-Host (T "No elegiste ningún archivo." "No file chosen."); Finish 1 }
    $Cia = $dialog.FileName
}
if (-not (Test-Path $Cia)) { Write-Host "Can't find $Cia"; Finish 1 }

# The base game (.3ds or .cci): its geometry shaders compiled into the library make it faster. One beside the .cia is
# taken by itself; otherwise the player may choose it, or go on without it (the app then interprets the shaders).
if (-not $Game -and -not $NoGame) {
    $beside = @(Get-ChildItem -Path (Split-Path $Cia) -File | Where-Object { $_.Extension -match '^\.(3ds|cci)$' })
    if ($beside.Count -eq 1) {
        $Game = $beside[0].FullName
    } else {
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = (T "Tu juego MH4U (USA), descifrado (.3ds o .cci): la librería sale más rápida. Cancelar para seguir sin él." `
                           "Your decrypted MH4U USA game (.3ds or .cci): it makes a faster library. Cancel to go on without it.")
        $dialog.Filter = "3DS (*.3ds;*.cci)|*.3ds;*.cci|All files (*.*)|*.*"
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Game = $dialog.FileName }
    }
}
if ($Game -and -not (Test-Path $Game)) { Write-Host "Can't find $Game"; Finish 1 }

if (-not $Mods -and -not $NoMods) {
    $question = T "¿Usas mods que cambian el código del juego, como el de una sola pantalla? Si es así, elige la carpeta mods que vas a poner en el teléfono." `
                  "Do you use mods that change the game's code, such as the single-screen mod? If so, choose the mods folder you'll put on the phone."
    $answer = [System.Windows.Forms.MessageBox]::Show($question, "MH4U", [System.Windows.Forms.MessageBoxButtons]::YesNo)
    if ($answer -eq [System.Windows.Forms.DialogResult]::Yes) {
        $folder = New-Object System.Windows.Forms.FolderBrowserDialog
        $folder.Description = (T "La carpeta mods (con 0004000000126300 adentro), o la carpeta del mod (con exefs adentro)" "The mods folder (with 0004000000126300 inside), or the mod's own folder (with exefs inside)")
        if ($folder.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Mods = $folder.SelectedPath }
    }
}

$exe = $python[0]
$out = Join-Path $root "build\android-game"
$pyArgs = @($python | Select-Object -Skip 1) + @($tool, "--dump", $Cia, "--native", "--android", "--fetch-compiler", "--out", $out)
if ($Game) {
    $pyArgs += @("--romfs", $Game)
    Write-Host ((T "Con los shaders del juego de " "With the game's shaders from ") + $Game)
}
if ($Mods) {
    # build_game.py takes the code mods of that folder as the app copies them to the phone's user folder (every mod in
    # it, in the order the app loads them: GameFolderActivity.syncMods), and says if two of them don't go together
    if (-not (Test-Path $Mods -PathType Container)) { Write-Host (T "No se encuentra $Mods." "Can't find $Mods."); Finish 1 }
    $pyArgs += @("--game-mods", $Mods)
    Write-Host ((T "Con los mods de " "With the mods of ") + $Mods)
} else {
    $pyArgs += @("--no-mods")
}
Write-Host ((T "Creando la librería desde " "Building the library from ") + $Cia + (T " (unos minutos)" " (a few minutes)"))
& $exe @pyArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host ((T "Algo salió mal (código " "Something went wrong (code ") + $LASTEXITCODE + (T "): mira el texto de arriba." "): see the text above."))
    Finish 1
}
$lib = Join-Path $out "recomp\android\libmh4u_code_native.so"
if (-not (Test-Path $lib)) { Write-Host "Can't find $lib"; Finish 1 }
$target = Join-Path (Split-Path $Cia) "libmh4u_code_native.so"
Copy-Item -Force $lib $target
# its id (which version of the tools made it): the app checks it before starting the game
$id = Join-Path $out "recomp\android\libmh4u_code_native.id"
if (Test-Path $id) { Copy-Item -Force $id (Join-Path (Split-Path $Cia) "libmh4u_code_native.id") }
Write-Host ""
Write-Host ((T "Listo: " "Done: ") + $target)
Write-Host (T "Copia esa librería y su .id al teléfono, a la carpeta donde están el juego y la actualización." `
              "Copy that library and its .id to the phone, into the folder with the game and the update.")
Finish 0
