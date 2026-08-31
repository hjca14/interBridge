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

# Must match include/interbridge_dev_secrets.example.h exactly, and the
# same two literals src/dev/mqtt_smoke_main.cpp and
# src/dev/dev_ring_simulator_main.cpp compare the compiled macro against
# at runtime (diagnoseCredentialField()) - never let a real DEV header
# silently ship with the example placeholder still in place.
$wifiSsidPlaceholder = "REPLACE_WITH_WIFI_SSID"
$wifiPasswordPlaceholder = "REPLACE_WITH_WIFI_PASSWORD"

function ConvertTo-CppString([string]$Value) {
    # .Replace() operates on the full .NET (UTF-16) string, character by
    # character - it never splits or reinterprets a multi-byte code point,
    # so this preserves every byte of the original value exactly, only
    # ever inserting the two extra escape characters C++ string literals
    # require. WriteAllLines below then encodes the whole line (escapes
    # and all) as UTF-8, so the compiled macro holds precisely the UTF-8
    # bytes the user typed - never re-encoded, truncated, or substituted.
    return $Value.Replace("\", "\\").Replace('"', '\"').Replace("`r`n", '\n').Replace("`n", '\n').Replace("`r", '\n')
}

$securePassword = $null
$passwordPointer = [IntPtr]::Zero
$plainPassword = $null
$privateKey = $null
# Byte counts only - captured before $plainPassword is cleared in the
# finally block below, and the only fact about the SSID/password this
# script ever prints. Uses UTF8 byte count (not .Length, which counts
# UTF-16 code units) so this matches exactly what the compiled C++ macro
# holds and what the firmware's own diagnoseCredentialField() reports.
$ssidByteCount = 0
$passwordByteCount = 0
try {
    $ssid = Read-Host "Wi-Fi SSID"
    if ([String]::IsNullOrWhiteSpace($ssid)) { throw "SSID is required." }
    if ($ssid -eq $wifiSsidPlaceholder) {
        throw "SSID must not be the example header's placeholder value ($wifiSsidPlaceholder)."
    }
    $securePassword = Read-Host "Wi-Fi password" -AsSecureString
    $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePassword)
    $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
    if ([String]::IsNullOrEmpty($plainPassword)) { throw "Wi-Fi password is required." }
    if ($plainPassword -eq $wifiPasswordPlaceholder) {
        throw "Wi-Fi password must not be the example header's placeholder value."
    }
    $ssidByteCount = [Text.Encoding]::UTF8.GetByteCount($ssid)
    $passwordByteCount = [Text.Encoding]::UTF8.GetByteCount($plainPassword)

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

    # Every macro this header is supposed to define must appear exactly
    # once - guards against a future edit to this script accidentally
    # duplicating or dropping one (e.g. a bad merge/copy-paste), which
    # would otherwise only surface later as a confusing C++ redefinition
    # error or a silently-missing macro at compile time.
    $expectedMacros = @(
        "INTERBRIDGE_DEV_WIFI_SSID", "INTERBRIDGE_DEV_WIFI_PASSWORD", "INTERBRIDGE_DEV_AWS_ENDPOINT",
        "INTERBRIDGE_DEV_DEVICE_ID", "INTERBRIDGE_DEV_ROOT_CA_PEM", "INTERBRIDGE_DEV_CERTIFICATE_PEM",
        "INTERBRIDGE_DEV_PRIVATE_KEY_PEM"
    )
    foreach ($macro in $expectedMacros) {
        $pattern = "^#define $macro "
        $matchCount = ($lines | Select-String -Pattern $pattern).Count
        if ($matchCount -ne 1) {
            throw "Internal error: $macro must be defined exactly once (found $matchCount) - refusing to write."
        }
    }

    [IO.File]::WriteAllLines($destinationPath, $lines, [Text.UTF8Encoding]::new($false))
} finally {
    if ($passwordPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer) }
    $plainPassword = $null
    $privateKey = $null
    $securePassword = $null
    [GC]::Collect()
}

Write-Host "DEV secrets header generated safely at the ignored destination."
Write-Host "SSID length: $ssidByteCount bytes; Wi-Fi password length: $passwordByteCount bytes (values not shown)."
