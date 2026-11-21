# Writes the CPU reference checkpoints the WebGPU page compares itself with (web/main.js,
# ?task=verify): pairs of states one step apart at several sizes, and the start state the
# benchmark grows from. Output goes to runs\web_verify (not committed).
#   scripts\web_verify_data.ps1 ; scripts\web_serve.ps1 ; open http://localhost:8123/web/?task=verify
param([string]$Bin = "build\release\bin")

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $run = Join-Path $Bin 'cf_run.exe'
    $out = 'runs\web_verify'
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force $out | Out-Null
    $quiet = @('--snapshot-factor', '0', '--checkpoint-factor', '0')

    # Start state of the default run (step 0).
    & $run --new icosphere:2 --seed 1 --steps 0 --out "$out\start_dir" @quiet | Out-Null
    Copy-Item "$out\start_dir\latest.cfs" "$out\start.cfs"

    $pairs = @()
    $cases = @(
        @{ label = 'default parameters, step 0';    args = @('--steps', '0') },
        @{ label = 'default parameters, 1k cells';  args = @('--until-cells', '1000') },
        @{ label = 'default parameters, 20k cells'; args = @('--until-cells', '20000') },
        @{ label = 'fast food, 5k cells';           args = @('--param', 'foodMax=0.02', '--until-cells', '5000') },
        @{ label = 'fast food, 60k cells';          args = @('--param', 'foodMax=0.02', '--until-cells', '60000') },
        @{ label = 'fast food, 250k cells';         args = @('--param', 'foodMax=0.02', '--until-cells', '250000') }
    )
    $i = 0
    foreach ($c in $cases) {
        $a = "$out\a$i"; $b = "$out\b$i"
        $caseArgs = $c.args
        & $run --new icosphere:2 --seed 1 @caseArgs --out $a @quiet | Out-Null
        & $run --load "$a\latest.cfs" --steps 1 --out $b @quiet | Out-Null
        if (-not (Test-Path "$b\latest.cfs")) { throw "cf_run did not write $b\latest.cfs" }
        $pairs += [ordered]@{ label = $c.label; before = "/runs/web_verify/a$i/latest.cfs"; after = "/runs/web_verify/b$i/latest.cfs" }
        $i++
    }
    [ordered]@{ pairs = $pairs } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$out\manifest.json"
    "wrote $($pairs.Count) pairs to $out"
}
finally { Pop-Location }
