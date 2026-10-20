#requires -Version 7.0
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Help', 'Check', 'Build', 'Flash', 'Metrics')]
    [string]$Action = 'Help',
    [ValidateSet('battery', 'debug')]
    [string]$Profile = 'battery',
    [string]$Port,
    [string]$PlatformIOPython,
    [string]$EsptoolPython,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildRoot = 'C:/pio-builds/ESP32_EINK'
$environmentName = if ($Profile -eq 'battery') { 'battery' } else { 'esp32-s3-devkitc-1' }
$firmware = Join-Path $buildRoot "$environmentName/firmware.bin"

function Find-Python([string]$Explicit, [string]$Module) {
    # Only known local environments; never run an unrelated python/pio from PATH.
    $candidates = if ($Explicit) { @($Explicit) } else {
        @((Join-Path $HOME '.platformio/penv/Scripts/python.exe'),
          (Join-Path $PSScriptRoot '.venv-benchmark/Scripts/python.exe'))
    }
    $probe = if ($Module -eq 'esptool') {
        "import esptool, importlib.metadata; v=importlib.metadata.version('esptool'); assert v.split('.')[0]=='5', v; print(v); print(esptool.__file__)"
    } else {
        "import platformio; assert platformio.__version__.split('.')[0]=='6'; print(platformio.__version__); print(platformio.__file__)"
    }
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $details = @(& $candidate -I -c $probe 2>$null)
        if ($LASTEXITCODE -eq 0) {
            Write-Host "$Module local: $candidate ($($details -join '; '))"
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $parameter = if ($Module -eq 'esptool') { '-EsptoolPython (esptool v5)' } else { '-PlatformIOPython (Core v6)' }
    throw "$Module local compativel nao encontrado. Informe $parameter com python.exe confiavel ja instalado; nada sera instalado."
}

function Assert-ProjectLayout {
    $config = Get-Content -LiteralPath (Join-Path $root 'platformio.ini') -Raw
    if ($config -notmatch '(?m)^build_dir\s*=\s*C:/pio-builds/ESP32_EINK\s*$') {
        throw 'build_dir divergiu do caminho suportado; revisar antes de compilar/gravar.'
    }
    $partitions = Get-Content -LiteralPath (Join-Path $root 'partitions.csv') -Raw
    if ($partitions -notmatch '(?m)^factory,\s*app,\s*factory,\s*0x10000,\s*3M,\s*$') {
        throw 'Particao app divergente: esperado factory em 0x10000 com 3 MiB. Nao gravar.'
    }
}

function Invoke-SelectedBuild([string]$Python) {
    # setup_opus.py otherwise downloads this source archive in the SCons child.
    $archive = Join-Path $buildRoot 'voice-opus/opus-1.5.2.tar.gz'
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        throw "Cache Opus ausente: $archive. Preparacao de dependencias deve ser feita separadamente; sem download automatico."
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne
        '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1') {
        throw 'Cache Opus invalido; build cancelado sem download.'
    }
    # The installed Core v6 has no offline run flag. Refuse missing packages
    # before its installer runs, and deny network in this Python process.
    $offlineBuild = @'
import sys
from platformio.package.manager._install import PackageManagerInstallMixin
original_install = PackageManagerInstallMixin._install
def installed_only(self, spec, *args, **kwargs):
    spec = self.ensure_spec(spec)
    if kwargs.get('force') or not self.get_package(spec):
        raise RuntimeError('Offline: required package is not installed: ' + str(spec))
    return original_install(self, spec, *args, **kwargs)
PackageManagerInstallMixin._install = installed_only
def offline(event, args):
    if event in ('socket.connect', 'socket.getaddrinfo'):
        raise RuntimeError('Offline: network access is disabled by power_test.ps1')
sys.addaudithook(offline)
from platformio.__main__ import main
sys.argv = ['platformio', 'run', '--project-dir', sys.argv[1],
            '--environment', sys.argv[2], '--disable-auto-clean']
sys.exit(main())
'@
    Write-Host "Build offline selecionado: $environmentName (sem upload; aguarde sucesso)."
    & $Python -I -c $offlineBuild $root $environmentName
    if ($LASTEXITCODE -ne 0) { throw 'Build falhou; firmware anterior nao sera gravado.' }
    if (-not (Test-Path -LiteralPath $firmware -PathType Leaf)) {
        throw "Build terminou sem o artefato esperado: $firmware"
    }
    $size = (Get-Item -LiteralPath $firmware).Length
    if ($size -le 0 -or $size -gt 3MB) { throw 'Firmware vazio ou maior que a particao app de 3 MiB.' }
    Write-Host "Build pronto: $firmware ($size bytes)"
    Write-Host "SHA256: $((Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash)"
}

if ($Action -eq 'Help') {
    @'
Uso: pwsh -NoProfile -File tools/power_test.ps1 [-Action Help|Check|Build|Flash|Metrics]
     [-Profile battery|debug] [-Port COMn] [-PlatformIOPython python.exe]
     [-EsptoolPython python.exe] [-DryRun]

Help (padrao): somente esta ajuda, sem Python, build ou serial.
Check: inventario local de Python/PlatformIO/esptool v5 e binario; sem build/serial.
Build: compila SOMENTE o perfil escolhido, com dependencias ja instaladas.
Flash: exige -Action Flash e -Port; build selecionado, write-flash 0x10000
       e verify-flash 0x10000 via esptool v5. Unica acao que acessa hardware.
Metrics: exige -Profile debug e -Port; IMPRIME comando boot-metrics, nao executa.
DryRun: somente plano textual; nao executa Python, build, flash ou serial.

battery = battery (sem USB CDC); debug = esp32-s3-devkitc-1 (USB diagnostico).
Binarios: C:/pio-builds/ESP32_EINK/{battery,esp32-s3-devkitc-1}/firmware.bin
Nunca grava bootloader, partitions, NVS ou LittleFS; nao faz erase completo.
Nenhuma acao instala Python, PlatformIO, esptool ou dependencias automaticamente.
Nao executar Build/Flash em paralelo com outros builds/runners.
Guia: docs/VALIDACAO_ENERGIA_WAKE.md
'@
    return
}

if ($Action -in @('Flash', 'Metrics')) {
    if ($Port -notmatch '\ACOM[1-9][0-9]*\z') { throw 'Informe -Port COMn explicitamente (sem autodeteccao de portas).' }
}
if ($Action -eq 'Flash' -and -not $PSBoundParameters.ContainsKey('Action')) {
    throw 'Flash somente com -Action Flash explicito.'
}
if ($Action -eq 'Metrics') {
    if ($Profile -ne 'debug') { throw 'boot-metrics requer -Profile debug com USB CDC; nao existe no perfil battery.' }
    # Intentionally print only. Serial access remains a separate operator action.
    Write-Output 'PLANO APENAS: acorde o aparelho e execute separadamente, com USB de diagnostico:'
    $pythonCommand = if ($PlatformIOPython) { $PlatformIOPython } else { Join-Path $HOME '.platformio/penv/Scripts/python.exe' }
    Write-Output ('& "{0}" "{1}" boot-metrics --port {2} --timeout 10' -f
                  $pythonCommand.Replace('"', '`"'), (Join-Path $PSScriptRoot 'device_console.py'), $Port)
    Write-Output 'Este script nao abriu COM. valid_audio_us continua unmeasured; corrente exige bancada sem USB.'
    return
}
if ($DryRun) {
    Write-Output "DRY RUN: Action=$Action Profile=$Profile Environment=$environmentName"
    Write-Output "Artefato esperado (existencia nao validada neste modo): $firmware"
    if ($Action -in @('Build', 'Flash')) {
        Write-Output "Planejado: PlatformIO Core v6 offline run --environment $environmentName --disable-auto-clean"
        Write-Output 'Exigir build bem-sucedido nesta execucao; validar arquivo e tamanho <= 3 MiB.'
    }
    if ($Action -eq 'Flash') {
        Write-Output "Planejado esptool v5: --chip esp32s3 --port $Port --before usb-reset --after no-reset write-flash 0x10000 $firmware"
        Write-Output "Planejado esptool v5: --chip esp32s3 --port $Port --before no-reset --after hard-reset verify-flash 0x10000 $firmware"
    }
    Write-Output 'Nenhum Python/build/serial/flash executado.'
    return
}

Assert-ProjectLayout
$pythonPlatformIO = Find-Python $PlatformIOPython 'platformio'
if ($Action -eq 'Check') {
    Write-Host "Perfil: $Profile -> $environmentName"
    Write-Host "Artefato esperado: $firmware"
    if (Test-Path -LiteralPath $firmware -PathType Leaf) {
        Write-Host "Binario presente; NAO comprova build atualizado. SHA256: $((Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash)"
    } else { Write-Host 'Binario ausente; Build explicito sera necessario.' }
    try { $null = Find-Python $EsptoolPython 'esptool' }
    catch { Write-Warning $_.Exception.Message }
    Write-Host 'Check concluido sem build ou acesso serial. Ausencia de esptool v5 impede Flash, nao o inventario.'
    return
}

$pythonEsptool = if ($Action -eq 'Flash') { Find-Python $EsptoolPython 'esptool' } else { $null }
if (-not (Test-Path -LiteralPath $buildRoot -PathType Container)) {
    throw "Diretorio de build ausente: $buildRoot. Preparar ambiente separadamente."
}
$lock = $null
$image = $null
try {
    # Coordinates only invocations of this script, not other PlatformIO runners.
    $lock = [IO.File]::Open((Join-Path $buildRoot '.power-test.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    Invoke-SelectedBuild $pythonPlatformIO
    if ($Action -eq 'Flash') {
        # Windows: retain a read-only shared handle through write AND verify,
        # so an overlapping builder cannot replace the selected binary.
        $image = [IO.File]::Open($firmware, 'Open', 'Read', 'Read')
        if ($image.Length -le 0 -or $image.Length -gt 3MB) { throw 'Tamanho da imagem bloqueada invalido; gravacao cancelada.' }
        if ($image.ReadByte() -ne 0xE9) { throw 'Arquivo nao tem magic de imagem ESP; gravacao cancelada.' }
        Write-Host "SHA256 da imagem bloqueada: $((Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash)"
        $common = @('-I', '-m', 'esptool', '--chip', 'esp32s3', '--port', $Port, '--baud', '460800')
        & $pythonEsptool @common --before usb-reset --after no-reset write-flash 0x10000 $firmware
        if ($LASTEXITCODE -ne 0) { throw 'write-flash falhou; nao declarar instalacao concluida.' }
        & $pythonEsptool @common --before no-reset --after hard-reset verify-flash 0x10000 $firmware
        if ($LASTEXITCODE -ne 0) { throw 'verify-flash falhou; candidato NAO validado. Rever conexao e rollback app-only.' }
        Write-Host 'Aplicacao gravada e verificada em 0x10000. Isto nao valida energia nem captura fisica.'
    }
} finally {
    if ($null -ne $image) { $image.Dispose() }
    if ($null -ne $lock) { $lock.Dispose() }
}
