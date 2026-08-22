param(
    [Parameter(Mandatory = $true)]
    [string]$CredentialsPath
)

$ErrorActionPreference = 'Stop'

function Assert-IniValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or $Value.Contains("`r") -or
        $Value.Contains("`n")) {
        throw "The OAuth credential '$Name' is missing or invalid."
    }
}

$resolvedCredentials = (Resolve-Path -LiteralPath $CredentialsPath).Path
$json = [IO.File]::ReadAllText($resolvedCredentials) | ConvertFrom-Json

if ($null -ne $json.installed) {
    $credentials = $json.installed
    $clientType = 'installed'
} elseif ($null -ne $json.web) {
    $credentials = $json.web
    $clientType = 'web'
} else {
    throw 'The JSON file does not contain installed or web OAuth credentials.'
}

$clientId = [string]$credentials.client_id
$clientSecret = [string]$credentials.client_secret
Assert-IniValue -Name 'client_id' -Value $clientId
Assert-IniValue -Name 'client_secret' -Value $clientSecret

$configurationDirectory = Join-Path `
    ([Environment]::GetFolderPath('LocalApplicationData')) 'MdViewer'
$configurationPath = Join-Path $configurationDirectory 'google-drive.ini'
[IO.Directory]::CreateDirectory($configurationDirectory) | Out-Null

$content = "[google]`r`nclientId=$clientId`r`nclientSecret=$clientSecret`r`n"
$temporaryPath = Join-Path $configurationDirectory `
    ('.google-drive-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$backupPath = Join-Path $configurationDirectory `
    ('.google-drive-' + [Guid]::NewGuid().ToString('N') + '.bak')

try {
    [IO.File]::WriteAllText(
        $temporaryPath,
        $content,
        [Text.UTF8Encoding]::new($false)
    )
    if ([IO.File]::Exists($configurationPath)) {
        [IO.File]::Replace($temporaryPath, $configurationPath, $backupPath)
    } else {
        [IO.File]::Move($temporaryPath, $configurationPath)
    }
} finally {
    if ([IO.File]::Exists($temporaryPath)) {
        [IO.File]::Delete($temporaryPath)
    }
    if ([IO.File]::Exists($backupPath)) {
        [IO.File]::Delete($backupPath)
    }
}

[pscustomobject]@{
    ConfigurationPath = $configurationPath
    ClientType = $clientType
    ClientIdConfigured = $true
    ClientSecretConfigured = $true
}
