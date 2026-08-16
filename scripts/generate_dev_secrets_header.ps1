[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CredentialsDirectory,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^ib-[0-9a-f]{32}$')]
    [string]$DeviceId,
    [string]$Destination = "include/interbridge_dev_secrets.h"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$destinationPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $Destination))
$expectedDestination = [IO.Path]::GetFullPath((Join-Path $repoRoot "include/interbridge_dev_secrets.h"))
if ($destinationPath -ne $expectedDestination) { throw "Destination must be the ignored DEV header path." }

& git -C $repoRoot check-ignore --quiet -- $destinationPath
if ($LASTEXITCODE -ne 0) { throw "Destination is not ignored by Git; refusing to write." }

$credentialPath = (Resolve-Path -LiteralPath $CredentialsDirectory).Path
if ($credentialPath.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Credentials directory must be outside the repository."
}

$required = @(
    "endpoint.txt",
    "AmazonRootCA1.pem",
    "device-certificate.pem.crt",
    "private.pem.key"
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $credentialPath $name) -PathType Leaf)) {
        throw "A required credential file is missing."
    }
}

function ConvertTo-CppString([string]$Value) {
    return $Value.Replace("\", "\\").Replace('"', '\"').Replace("`r`n", '\n').Replace("`n", '\n').Replace("`r", '\n')
}

$securePassword = $null
$passwordPointer = [IntPtr]::Zero
$plainPassword = $null
$privateKey = $null
try {
    $ssid = Read-Host "Wi-Fi SSID"
    if ([String]::IsNullOrWhiteSpace($ssid)) { throw "SSID is required." }
    $securePassword = Read-Host "Wi-Fi password" -AsSecureString
    $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePassword)
    $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
    if ([String]::IsNullOrEmpty($plainPassword)) { throw "Wi-Fi password is required." }

    $endpoint = (Get-Content -LiteralPath (Join-Path $credentialPath "endpoint.txt") -Raw).Trim()
    if ($endpoint -notmatch '^[a-z0-9][a-z0-9-]{8,}-ats\.iot\.[a-z0-9-]+\.amazonaws\.com$') {
        throw "Endpoint format is invalid."
    }
    $rootCa = Get-Content -LiteralPath (Join-Path $credentialPath "AmazonRootCA1.pem") -Raw
    $certificate = Get-Content -LiteralPath (Join-Path $credentialPath "device-certificate.pem.crt") -Raw
    $privateKey = Get-Content -LiteralPath (Join-Path $credentialPath "private.pem.key") -Raw

    $lines = @(
        '#pragma once',
        '',
        '// Generated local DEV configuration. This file must remain ignored.',
        ('#define INTERBRIDGE_DEV_WIFI_SSID "{0}"' -f (ConvertTo-CppString $ssid)),
        ('#define INTERBRIDGE_DEV_WIFI_PASSWORD "{0}"' -f (ConvertTo-CppString $plainPassword)),
        ('#define INTERBRIDGE_DEV_AWS_ENDPOINT "{0}"' -f (ConvertTo-CppString $endpoint)),
        ('#define INTERBRIDGE_DEV_DEVICE_ID "{0}"' -f (ConvertTo-CppString $DeviceId)),
        ('#define INTERBRIDGE_DEV_ROOT_CA_PEM "{0}"' -f (ConvertTo-CppString $rootCa)),
        ('#define INTERBRIDGE_DEV_CERTIFICATE_PEM "{0}"' -f (ConvertTo-CppString $certificate)),
        ('#define INTERBRIDGE_DEV_PRIVATE_KEY_PEM "{0}"' -f (ConvertTo-CppString $privateKey))
    )
    [IO.File]::WriteAllLines($destinationPath, $lines, [Text.UTF8Encoding]::new($false))
} finally {
    if ($passwordPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer) }
    $plainPassword = $null
    $privateKey = $null
    $securePassword = $null
    [GC]::Collect()
}

Write-Host "DEV secrets header generated safely at the ignored destination."
