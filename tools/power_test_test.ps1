#requires -Version 7.0
# Offline tests only: no Python, PlatformIO, COM, or firmware access.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'power_test.ps1'
$tokens = $null
$errors = $null
$null = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw ($errors -join [Environment]::NewLine) }
if ([IO.File]::ReadAllBytes($script).Where({ $_ -gt 127 }).Count -ne 0) {
    throw 'power_test.ps1 must contain ASCII only.'
}
$count = 2
function Assert-Output([string]$Text, [string]$Pattern) {
    if ($Text -notmatch $Pattern) { throw "Missing output pattern: $Pattern`n$Text" }
}
function Assert-Rejected([scriptblock]$Call, [string]$Pattern) {
    try { & $Call | Out-Null }
    catch {
        if ($_.Exception.Message -match $Pattern) { return }
        throw
    }
    throw "Expected rejection: $Pattern"
}
$help = (& $script -Action Help | Out-String)
Assert-Output $help 'Help \(padrao\)'
$count++
$default = (& $script | Out-String)
if ($default -ne $help) { throw 'Default must be Help.' }
$count++
foreach ($profile in @('battery', 'debug')) {
    foreach ($action in @('Check', 'Build', 'Flash')) {
        # Deliberately invalid Python paths prove DryRun does not invoke them.
        $text = (& $script -Action $action -Profile $profile -Port COM7 -DryRun `
                 -PlatformIOPython 'Z:/missing/python.exe' -EsptoolPython 'Z:/missing/esptool.exe' | Out-String)
        Assert-Output $text 'Nenhum Python/build/serial/flash executado'
        $expected = if ($profile -eq 'battery') { 'battery' } else { 'esp32-s3-devkitc-1' }
        Assert-Output $text "Environment=$expected"
        if ($action -eq 'Flash') {
            Assert-Output $text 'write-flash 0x10000'
            Assert-Output $text 'verify-flash 0x10000'
            if ($text -match 'erase-flash|erase-region|--erase-all|0x9000') { throw 'Unsafe flash plan.' }
        }
        $count++
    }
}
$metrics = (& $script -Action Metrics -Profile debug -Port COM7 | Out-String)
Assert-Output $metrics 'PLANO APENAS'
Assert-Output $metrics 'boot-metrics --port COM7 --timeout 10'
$count++
Assert-Rejected { & $script -Action Metrics -Profile battery -Port COM7 } 'requer -Profile debug'
$count++
Assert-Rejected { & $script -Action Flash -DryRun } 'Informe -Port'
$count++
Assert-Rejected { & $script -Action Flash -Port 'COM7@args' -DryRun } 'Informe -Port'
$count++
Assert-Rejected { & $script Flash -Port COM7 -DryRun } 'positional|posicional'
$count++
Assert-Rejected { & $script -Action Unknown } 'ValidateSet|validation|validar|validacao'
$count++
$source = Get-Content -LiteralPath $script -Raw
Assert-Output $source 'sys\.exit\(main\(\)\)'
$count++
$doc = Join-Path (Split-Path -Parent $PSScriptRoot) 'docs/VALIDACAO_ENERGIA_WAKE.md'
$markdown = Get-Content -LiteralPath $doc -Raw
$localLinks = [regex]::Matches($markdown, '\]\((?!https?://)([^)]+)\)')
foreach ($link in $localLinks) {
    $target = Join-Path (Split-Path -Parent $doc) $link.Groups[1].Value
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Broken documentation link: $target" }
    $count++
}
Write-Output "PASS: $count offline assertions/groups (parser, ASCII, Help, default, dry runs, Metrics, rejections, exit propagation, doc links)."
