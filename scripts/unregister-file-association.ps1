$ErrorActionPreference = "Stop"
$classesRoot = "HKCU:\Software\Classes"
$progId = "MdViewer.Markdown"

foreach ($extension in ".md", ".markdown") {
    $extensionPath = Join-Path $classesRoot $extension
    if (!(Test-Path -LiteralPath $extensionPath)) { continue }
    $currentDefault = (Get-Item -LiteralPath $extensionPath).GetValue("")
    if ($currentDefault -eq $progId) {
        Remove-ItemProperty -LiteralPath $extensionPath -Name "(default)" `
            -ErrorAction SilentlyContinue
    }
    $openWithPath = Join-Path $extensionPath "OpenWithProgids"
    if (Test-Path -LiteralPath $openWithPath) {
        Remove-ItemProperty -LiteralPath $openWithPath -Name $progId `
            -ErrorAction SilentlyContinue
    }
}

$progIdPath = Join-Path $classesRoot $progId
if (Test-Path -LiteralPath $progIdPath) {
    Remove-Item -LiteralPath $progIdPath -Recurse -Force
}

Write-Host "Removed the current-user MdViewer file association."
