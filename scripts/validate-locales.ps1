$ErrorActionPreference = "Stop"

$workspace = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$localeDirectory = Join-Path $workspace "web\locales"
$masterPath = Join-Path $localeDirectory "ko-KR.json"
$master = Get-Content -Raw -LiteralPath $masterPath | ConvertFrom-Json -AsHashtable
$failures = [System.Collections.Generic.List[string]]::new()

function Get-Parameters([string]$Value) {
    return @([regex]::Matches($Value, '\{([A-Za-z][A-Za-z0-9]*)\}') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
}

$catalogFiles = @(Get-ChildItem -LiteralPath $localeDirectory -Filter "*.json" |
    Sort-Object Name)
if ($catalogFiles.Count -ne 11) {
    $failures.Add("Expected 11 translation catalogs, found $($catalogFiles.Count).")
}

foreach ($file in $catalogFiles) {
    try {
        $catalog = Get-Content -Raw -LiteralPath $file.FullName |
            ConvertFrom-Json -AsHashtable
    } catch {
        $failures.Add("$($file.Name): invalid JSON: $($_.Exception.Message)")
        continue
    }

    foreach ($key in $master.Keys) {
        if (!$catalog.ContainsKey($key)) {
            $failures.Add("$($file.Name): missing key '$key'.")
            continue
        }
        $sourceParameters = @(Get-Parameters $key)
        $targetParameters = @(Get-Parameters ([string]$catalog[$key]))
        if ((Compare-Object $sourceParameters $targetParameters).Count -ne 0) {
            $failures.Add("$($file.Name): placeholder mismatch for '$key'.")
        }
    }
    foreach ($key in $catalog.Keys) {
        if (!$master.ContainsKey($key)) {
            $failures.Add("$($file.Name): unexpected key '$key'.")
        }
    }
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Error $_ }
    throw "Locale validation failed with $($failures.Count) error(s)."
}

Write-Host "Validated $($catalogFiles.Count) locale catalogs with $($master.Count) keys each."
