# Writes the reference checkpoints the WebGPU page compares itself with (web/tasks.js). Output
# goes to runs\web_verify (not committed).
#
#   scripts\web_verify_data.ps1                     the data of ?task=verify (manifest.json)
#   scripts\web_verify_data.ps1 -Gallery 599,505    also the data of ?task=gallery (gallery.json)
#   scripts\web_verify_data.ps1 -Gallery 599 -GalleryOnly     only that (and not the data of ?task=verify)
#   scripts\web_verify_data.ps1 -Relay -GalleryOnly           a run whose parameters change on the way (relay.json)
#   then: python scripts\web_serve.py ; open http://localhost:8123/web/?task=verify
#
# ?task=verify takes one step from a state and compares the result with the CPU reference's.
#   - Pairs of states one step apart: the base model at several sizes; every mechanism beyond it
#     alone; combinations of mechanisms with the parameter sets of forms the search found
#     (web/app/gallery/gallery.json); each at a small and a larger size.
#     The "before" state of a mechanism pair is grown by the CUDA backend (fast), the step to the
#     "after" state is always taken by the CPU reference (cf_run).
#   - Pairs with `set`: the CPU resumed with changed parameters (morphogens switched on in a
#     running form, and switched off again); the page applies them with setParams.
#   - Start states (step 0) of several shapes, with and without morphogens, for makeStart.
# ?task=gallery grows forms of the gallery in the page to the step the search stopped at; the
#   reference is the CUDA backend's checkpoint of that step (about 2.7 MB per form; all 120
#   forms of the gallery are 320 MB and take under two minutes to write).
#   With &data=/runs/web_verify/relay.json it runs the -Relay case: parameters changed twice
#   during the run, by setParams in the page and by resuming the CUDA backend here.
param([string]$Bin = "build\release\bin", [int[]]$Gallery = @(), [switch]$GalleryOnly, [switch]$Relay)

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $run = Join-Path $Bin 'cf_run.exe'
    $gpu = Join-Path $Bin 'cf_gpu.exe'
    $out = 'runs\web_verify'
    New-Item -ItemType Directory -Force $out | Out-Null
    $quiet = @('--snapshot-factor', '0', '--checkpoint-factor', '0')
    $inv = [cultureinfo]::InvariantCulture

    # --param k=v for every entry of an ordered table of strings
    function ParamArgs($table) {
        $a = @()
        foreach ($k in $table.Keys) { $a += '--param'; $a += ('{0}={1}' -f $k, $table[$k]) }
        return $a
    }
    # the same table with numbers, for a manifest
    function Numbers($table) {
        $o = [ordered]@{}
        foreach ($k in $table.Keys) { $o[$k] = [double]::Parse([string]$table[$k], $inv) }
        return $o
    }
    function Fresh([string]$dir) {
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    }
    function Need([string]$file) {
        if (-not (Test-Path $file)) { throw "expected $file was not written" }
    }
    # a parameter value of the gallery as text (its numbers were written with 9 significant digits)
    function Text($value) { return ([double]$value).ToString('R', $inv) }

    $galleryData = Get-Content -Raw 'web\app\gallery\gallery.json' | ConvertFrom-Json
    $startFiles = @{}
    foreach ($s in (Get-Content -Raw 'web\app\starts\starts.json' | ConvertFrom-Json)) { $startFiles[$s.shape] = $s.file }
    function GalleryParams($item) {
        $t = [ordered]@{}
        foreach ($prop in $item.params.PSObject.Properties) { $t[$prop.Name] = Text $prop.Value }
        return $t
    }

    if (-not $GalleryOnly) {
        # Start state of the default run (step 0).
        Fresh "$out\start_dir"
        & $run --new icosphere:2 --seed 1 --steps 0 --out "$out\start_dir" @quiet | Out-Null
        Copy-Item "$out\start_dir\latest.cfs" "$out\start.cfs" -Force

        $pairs = @()

        # ---- the base model (CPU all the way)
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
            Fresh $a; Fresh $b
            $caseArgs = $c.args
            & $run --new icosphere:2 --seed 1 @caseArgs --out $a @quiet | Out-Null
            & $run --load "$a\latest.cfs" --steps 1 --out $b @quiet | Out-Null
            Need "$b\latest.cfs"
            $pairs += [ordered]@{ label = $c.label; before = "/runs/web_verify/a$i/latest.cfs"; after = "/runs/web_verify/b$i/latest.cfs" }
            $i++
        }

        # ---- every mechanism alone, and combinations as the search makes them
        $fast = [ordered]@{ foodMax = '0.01' }
        $sizes = @(@{ name = '3k cells'; stop = @('--until-cells', '3000') }, @{ name = '40k cells'; stop = @('--until-cells', '40000') })
        $smaller = @(@{ name = '3k cells'; stop = @('--until-cells', '3000') }, @{ name = '25k cells'; stop = @('--until-cells', '25000') })
        # Pulses: period 137 steps, on for the first 51. The step taken from step 2900 is in the
        # "on" part (2900 mod 137 = 23), the one from step 4370 in the "off" part (123).
        $pulseStops = @(@{ name = 'on, step 2900'; stop = @('--until-step', '2900') }, @{ name = 'off, step 4370'; stop = @('--until-step', '4370') })
        $mechanisms = @(
            @{ label = 'field';             base = $fast; p = [ordered]@{ fieldAmount = '0.7'; fieldScale = '12'; fieldContrast = '2.5'; fieldOctaves = '3' }; at = $sizes },
            @{ label = 'field, one octave'; base = $fast; p = [ordered]@{ fieldAmount = '1'; fieldScale = '31.5' }; at = $sizes },
            @{ label = 'field on bulge';    base = $fast; p = [ordered]@{ fieldBulge = '0.6'; fieldScale = '10'; fieldOctaves = '2' }; at = $sizes },
            @{ label = 'pulses';            base = [ordered]@{ foodMax = '0.004' }; p = [ordered]@{ pulsePeriod = '137.5'; pulseDuty = '0.37'; pulseLow = '0.21' }; at = $pulseStops },
            @{ label = 'light';             base = $fast; p = [ordered]@{ lightAmount = '0.8'; lightAmbient = '0.1'; lightDirX = '0.42'; lightDirY = '0.57'; lightDirZ = '-0.7' }; at = $sizes },
            @{ label = 'light, coarse map'; base = $fast; p = [ordered]@{ lightAmount = '1'; lightAmbient = '0'; lightDirX = '0'; lightDirY = '0'; lightDirZ = '1'; lightPixel = '2.7'; lightBias = '0.3' }; at = $sizes },
            @{ label = 'diffusion, decay';  base = $fast; p = [ordered]@{ foodDiffusion = '0.3'; foodDecay = '0.002' }; at = $sizes },
            @{ label = 'crowding';          base = $fast; p = [ordered]@{ crowdLow = '0.8'; crowdHigh = '1.03' }; at = $sizes },
            @{ label = 'oriented division'; base = $fast; p = [ordered]@{ orientAmount = '0.7'; orientAxisX = '0.3'; orientAxisY = '1'; orientAxisZ = '0.2' }; at = $sizes },
            @{ label = 'morphogens';        base = $fast; p = [ordered]@{ morphogens = '1'; morphFood = '0.6'; morphBulge = '-0.5'; morphPlanar = '0.4'; morphRest = '0.2' }; at = $sizes },
            @{ label = 'morphogens, spots, 7 iterations'; base = $fast; p = [ordered]@{ morphogens = '1'; morphFeed = '0.035'; morphKill = '0.065'; morphIterations = '7'; morphSeedLevel = '0.6'; morphBulge = '0.7' }; at = $sizes }
        )
        # Combinations: the parameter sets (base parameters included) of forms in the gallery.
        foreach ($id in @(4199, 3707, 1397, 3433)) {
            $item = $galleryData.items | Where-Object { $_.id -eq $id }
            if (-not $item) { throw "gallery.json has no item $id" }
            $mechanisms += @{ label = ("gallery form $id (" + ($item.sources -join ' + ') + ')'); base = [ordered]@{}; p = (GalleryParams $item); at = $smaller }
        }
        # A form whose activator fades through the subnormal range on a part of the sheet (about
        # 2,800 of its 31,637 cells at step 4924). A GPU flushes subnormal numbers to zero; the
        # port computes them with integer arithmetic, and this pair is what checks that.
        $item = $galleryData.items | Where-Object { $_.id -eq 3719 }
        if (-not $item) { throw 'gallery.json has no item 3719' }
        $mechanisms += @{ label = ('gallery form 3719 (' + ($item.sources -join ' + ') + '), subnormal activator'); base = [ordered]@{}; p = (GalleryParams $item)
                          at = @(@{ name = 'step 4924'; stop = @('--until-step', '4924') }) }
        $m = 0
        $morphSmall = $null; $plainSmall = $null
        foreach ($c in $mechanisms) {
            foreach ($size in $c.at) {
                $a = "$out\m${m}a"; $b = "$out\m${m}b"
                Fresh $a; Fresh $b
                $baseArgs = ParamArgs $c.base
                $mechArgs = ParamArgs $c.p
                $stop = $size.stop
                & $gpu --new icosphere:2 --seed 1 --capacity 200000 @baseArgs @mechArgs @stop --max-seconds 600 --out $a @quiet | Out-Null
                Need "$a\latest.cfs"
                & $run --load "$a\latest.cfs" --steps 1 --out $b @quiet | Out-Null
                Need "$b\latest.cfs"
                $pairs += [ordered]@{ label = "$($c.label), $($size.name)"; before = "/runs/web_verify/m${m}a/latest.cfs"; after = "/runs/web_verify/m${m}b/latest.cfs" }
                if ($c.label -eq 'morphogens' -and -not $morphSmall) { $morphSmall = "m${m}a" }
                if ($c.label -eq 'field on bulge' -and -not $plainSmall) { $plainSmall = "m${m}a" }
                $m++
            }
        }

        # ---- parameters changed in a running form
        # Morphogens switched on at a step other than 0: the cells get their starting
        # concentrations from the positions and the step counter of that moment, then one step.
        $on = [ordered]@{ morphogens = '1'; morphFood = '0.6'; morphBulge = '-0.5'; morphSeedLevel = '0.7' }
        $b = "$out\s0b"; Fresh $b
        $onArgs = ParamArgs $on
        & $run --load "$out\$plainSmall\latest.cfs" @onArgs --steps 1 --out $b @quiet | Out-Null
        Need "$b\latest.cfs"
        $pairs += [ordered]@{ label = 'morphogens switched on in a running form'; before = "/runs/web_verify/$plainSmall/latest.cfs"; after = '/runs/web_verify/s0b/latest.cfs'; set = (Numbers $on) }
        # ... and switched off again: the concentrations stay as they are, daughters still inherit.
        $off = [ordered]@{ morphogens = '0' }
        $b = "$out\s1b"; Fresh $b
        & $run --load "$out\$morphSmall\latest.cfs" --param morphogens=0 --steps 1 --out $b @quiet | Out-Null
        Need "$b\latest.cfs"
        $pairs += [ordered]@{ label = 'morphogens switched off, concentrations kept'; before = "/runs/web_verify/$morphSmall/latest.cfs"; after = '/runs/web_verify/s1b/latest.cfs'; set = (Numbers $off) }

        # ---- start states (step 0), for makeStart
        $starts = @()
        $startCases = @(
            @{ label = 'icosphere:2, seed 1, morphogens';          shape = 'icosphere:2';       seed = 1; food = 'random'; p = [ordered]@{ morphogens = '1'; morphFood = '0.5' } },
            @{ label = 'icosphere:4, seed 5, morphogens, patches'; shape = 'icosphere:4';       seed = 5; food = 'random'; p = [ordered]@{ morphogens = '1'; morphSeedLevel = '0.5'; morphSeedScale = '1.7' } },
            @{ label = 'ellipsoid 2:1:1, seed 7, no food';         shape = 'ellipsoid:2:2:1:1'; seed = 7; food = 'zero';   p = [ordered]@{ morphogens = '1' } },
            @{ label = 'noisy sphere, seed 1';                     shape = 'noisy:2:0.3:2';     seed = 1; food = 'random'; p = [ordered]@{ foodThreshold = '2' } }
        )
        $k = 0
        foreach ($c in $startCases) {
            $d = "$out\start$k"; Fresh $d
            $pa = ParamArgs $c.p
            & $run --new $c.shape --seed $c.seed --init-food $c.food @pa --steps 0 --out $d @quiet | Out-Null
            Need "$d\latest.cfs"
            if (-not $startFiles.ContainsKey($c.shape)) { throw "web/app/starts has no file for $($c.shape)" }
            $starts += [ordered]@{ label = $c.label; mesh = "/web/app/starts/$($startFiles[$c.shape])"; seed = $c.seed; food = $c.food; params = (Numbers $c.p); state = "/runs/web_verify/start$k/latest.cfs" }
            $k++
        }

        [ordered]@{ pairs = $pairs; starts = $starts } | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$out\manifest.json"
        "wrote $($pairs.Count) pairs and $($starts.Count) start states to $out"
    }

    # ---- forms of the gallery, at the step the search stopped at (CUDA backend)
    if ($Gallery.Count -gt 0) {
        New-Item -ItemType Directory -Force "$out\gallery" | Out-Null
        $items = @()
        foreach ($id in $Gallery) {
            $item = $galleryData.items | Where-Object { $_.id -eq $id }
            if (-not $item) { throw "gallery.json has no item $id" }
            $d = "$out\gallery\$id"; Fresh $d
            $pa = ParamArgs (GalleryParams $item)
            $capacity = [int][math]::Floor($item.target * 1.5) + 4096     # as scripts/explore.py runs it
            if ($item.init -like 'icosphere:*') {
                $text = & $gpu --new $item.init --seed $item.seed --init-food $item.food --capacity $capacity @pa --until-step $item.step --out $d @quiet
            } else {
                # the CUDA runner only makes icospheres: the CPU runner writes the start state
                $d0 = "$out\gallery\${id}_start"; Fresh $d0
                & $run --new $item.init --seed $item.seed --init-food $item.food @pa --steps 0 --out $d0 @quiet | Out-Null
                $text = & $gpu --load "$d0\latest.cfs" --capacity $capacity --until-step $item.step --out $d @quiet
            }
            Need "$d\latest.cfs"
            $cells = [int](($text | Select-String -Pattern '^cells=(\d+)').Matches[0].Groups[1].Value)
            if ($cells -ne $item.cells) { Write-Warning "form ${id}: this run has $cells cells at step $($item.step), the gallery says $($item.cells)" }
            if (-not $startFiles.ContainsKey($item.init)) { throw "web/app/starts has no file for $($item.init)" }
            $items += [ordered]@{ id = $item.id; init = $item.init; mesh = "/web/app/starts/$($startFiles[$item.init])"; food = $item.food; seed = $item.seed
                                  sources = @($item.sources); params = $item.params; cells = $cells; step = $item.step; capacity = $capacity
                                  reference = "/runs/web_verify/gallery/$id/latest.cfs" }
            "form ${id}: $cells cells at step $($item.step)"
        }
        [ordered]@{ items = $items } | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$out\gallery.json"
        "wrote $($items.Count) gallery forms to $out\gallery.json"
    }

    # ---- a run whose parameters change twice on the way (?task=gallery&data=/runs/web_verify/relay.json)
    # The CUDA backend is resumed with new parameters; the page calls setParams at the same steps.
    # Leg 1: the base model. Leg 2: all seven mechanisms, with the parameter set of gallery form
    # 3433 (the morphogens get their starting concentrations at step 1500). Leg 3: everything off
    # again but oriented division (the concentrations stay as they were, daughters inherit them).
    if ($Relay) {
        $all = GalleryParams ($galleryData.items | Where-Object { $_.id -eq 3433 })
        $leg1 = [ordered]@{ foodMax = '0.004' }
        $leg3 = [ordered]@{ foodMax = '0.004'; orientAmount = '0.5' }
        # leg 3 on the command line: every parameter leg 2 set, back to its default or to leg 3's value
        $reset = [ordered]@{ springFactor = '0.15'; planarFactor = '0.1'; bulgeFactor = '0.1'; repulsionStrength = '0.1'; radiusOfInfluence = '2'; daughterOffset = '0.1'
                             lightAmount = '0'; lightAmbient = '0.05'; lightDirX = '0'; lightDirY = '1'; lightDirZ = '0'; crowdLow = '0'; crowdHigh = '0'
                             fieldAmount = '0'; fieldScale = '20'; fieldContrast = '1'; fieldOctaves = '1'; pulsePeriod = '0'; pulseDuty = '0.5'; pulseLow = '0'
                             foodDiffusion = '0'; morphogens = '0'; morphFood = '0'; morphBulge = '0'; morphPlanar = '0'; morphRest = '0' }
        foreach ($k in $leg3.Keys) { $reset[$k] = $leg3[$k] }
        foreach ($k in $all.Keys) { if (-not $reset.Contains($k)) { throw "the relay does not reset $k" } }
        $capacity = 150000
        $d = "$out\relay"; Fresh "${d}1"; Fresh "${d}2"; Fresh "${d}3"
        $a1 = ParamArgs $leg1; $a2 = ParamArgs $all; $a3 = ParamArgs $reset
        & $gpu --new icosphere:2 --seed 3 --capacity $capacity @a1 --until-step 1500 --out "${d}1" @quiet | Out-Null
        & $gpu --load "${d}1\latest.cfs" --capacity $capacity @a2 --until-step 3000 --out "${d}2" @quiet | Out-Null
        $text = & $gpu --load "${d}2\latest.cfs" --capacity $capacity @a3 --until-step 3600 --out "${d}3" @quiet
        Need "${d}3\latest.cfs"
        $cells = [int](($text | Select-String -Pattern '^cells=(\d+)').Matches[0].Groups[1].Value)
        $item = [ordered]@{ id = 0; init = 'icosphere:2'; mesh = "/web/app/starts/$($startFiles['icosphere:2'])"; food = 'random'; seed = 3
                            sources = @('the base model to step 1500', 'all seven mechanisms to step 3000', 'oriented division only to step 3600')
                            params = (Numbers $leg1); cells = $cells; step = 3600; capacity = $capacity; reference = '/runs/web_verify/relay3/latest.cfs'
                            legs = @([ordered]@{ step = 1500; params = (Numbers $leg1) }, [ordered]@{ step = 3000; params = (Numbers $all) }, [ordered]@{ step = 3600; params = (Numbers $leg3) }) }
        [ordered]@{ items = @($item) } | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$out\relay.json"
        "relay: $cells cells at step 3600; wrote $out\relay.json"
    }
}
finally { Pop-Location }
