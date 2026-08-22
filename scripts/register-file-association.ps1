param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath
)

$ErrorActionPreference = "Stop"
$resolvedExecutable = [IO.Path]::GetFullPath($ExecutablePath)
if (!(Test-Path -LiteralPath $resolvedExecutable -PathType Leaf)) {
    throw "MdViewer executable was not found: $resolvedExecutable"
}
if ([IO.Path]::GetExtension($resolvedExecutable) -ne ".exe") {
    throw "ExecutablePath must point to an .exe file."
}

$classesRoot = "HKCU:\Software\Classes"
$progId = "MdViewer.Markdown"
$progIdPath = Join-Path $classesRoot $progId

New-Item -Path $progIdPath -Force | Out-Null
Set-Item -LiteralPath $progIdPath -Value "Markdown Document"
New-Item -Path (Join-Path $progIdPath "DefaultIcon") -Force | Out-Null
Set-Item -LiteralPath (Join-Path $progIdPath "DefaultIcon") `
    -Value ('"{0}",0' -f $resolvedExecutable)
New-Item -Path (Join-Path $progIdPath "shell\open\command") -Force | Out-Null
Set-Item -LiteralPath (Join-Path $progIdPath "shell\open\command") `
    -Value ('"{0}" "%1"' -f $resolvedExecutable)

foreach ($extension in ".md", ".markdown", ".mdz") {
    $extensionPath = Join-Path $classesRoot $extension
    New-Item -Path $extensionPath -Force | Out-Null
    Set-Item -LiteralPath $extensionPath -Value $progId
    $openWithPath = Join-Path $extensionPath "OpenWithProgids"
    New-Item -Path $openWithPath -Force | Out-Null
    New-ItemProperty -Path $openWithPath -Name $progId -PropertyType String `
        -Value "" -Force | Out-Null
}

Write-Host "Registered MdViewer for .md, .markdown, and .mdz files for the current user."
Write-Host "Windows may require confirmation under Settings > Default apps."
