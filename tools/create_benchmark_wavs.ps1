param(
  [string]$OutputDirectory = "test/results/voice-benchmark/corpus",
  [string]$VoiceName = "Microsoft Maria Desktop"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech
$root = (Resolve-Path ".").Path
$target = Join-Path $root $OutputDirectory
New-Item -ItemType Directory -Path $target -Force | Out-Null
$speaker = [System.Speech.Synthesis.SpeechSynthesizer]::new()
try {
  $voice = $speaker.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Name -eq $VoiceName } | Select-Object -First 1
  if ($null -eq $voice) { throw "Voz nao instalada: $VoiceName" }
  if ($voice.VoiceInfo.Culture.Name -ne "pt-BR") { throw "A voz precisa usar pronuncia pt-BR: $VoiceName" }
  $speaker.SelectVoice($VoiceName)
  $speaker.Rate = 0
  $speaker.Volume = 100
  $samples = @(
    @{ Name = "short.wav"; Text = "Responda apenas: Olá, tudo bem com você?" },
    @{ Name = "medium.wav"; Text = "Esta é uma pergunta de teste. Não use ferramentas, não faça consultas e não acrescente explicações. Responda apenas: Olá, tudo bem com você?" },
    @{ Name = "long.wav"; Text = "Esta é uma pergunta longa de teste, com uma resposta curta e fixa. Escute até o fim antes de responder. Hoje pela manhã, organizei três tarefas: revisar as anotações, confirmar o horário da reunião e separar os documentos. Não execute essas tarefas, não use ferramentas e não faça um resumo. Independentemente dessas informações, responda apenas: Olá, tudo bem com você?" }
  )
  foreach ($sample in $samples) {
    $path = Join-Path $target $sample.Name
    if (Test-Path -LiteralPath $path) { throw "Corpus existente nao sera sobrescrito: $path" }
    $speaker.SetOutputToWaveFile($path)
    $speaker.Speak($sample.Text)
    $speaker.SetOutputToNull()
    Write-Output $path
  }
}
finally {
  $speaker.Dispose()
}
