[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CredentialsDirectory,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^ib-[0-9a-f]{32}$')]
    [string]$DeviceId,
    [string]$Destination = "include/interbridge_dev_ble_mqtt_secrets.h"
)

# Generates the single ignored local header the composed Phase 3C.4
# esp32-c3-dev-ble-mqtt environment depends on for AWS IoT/MQTT identity and
# the BLE DEV PoP - see docs/dev-ble-mqtt.md > "Local credentials and
# build". Deliberately never touches, generates, or accepts a Wi-Fi SSID or
# password: that environment gets Wi-Fi exclusively from the official BLE
# Unified Provisioning session and whatever the ESP-IDF Wi-Fi driver then
# persists in NVS. Never accepts certificate, private key, PoP, SSID, or
# password material as a command-line argument - all of it is read only
# from fixed filenames inside an explicitly selected directory outside this
# repository.

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$destinationPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $Destination))
$expectedDestination = [IO.Path]::GetFullPath((Join-Path $repoRoot "include/interbridge_dev_ble_mqtt_secrets.h"))
if ($destinationPath -ne $expectedDestination) { throw "Destination must be the ignored DEV BLE+MQTT header path." }

& git -C $repoRoot check-ignore --quiet -- $destinationPath
if ($LASTEXITCODE -ne 0) { throw "Destination is not ignored by Git; refusing to write." }

$credentialPath = (Resolve-Path -LiteralPath $CredentialsDirectory).Path
if ($credentialPath.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Credentials directory must be outside the repository."
}

# Exactly these five external files, nothing else - see docs/dev-ble-
# mqtt.md's runbook for how pop.txt is expected to be produced
# (openssl rand -hex 32).
$required = @(
    "endpoint.txt",
    "AmazonRootCA1.pem",
    "device-certificate.pem.crt",
    "private.pem.key",
    "pop.txt"
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $credentialPath $name) -PathType Leaf)) {
        throw "A required credential file is missing: $name"
    }
}

# Must match include/interbridge_dev_ble_mqtt_secrets.example.h exactly -
# never let a real generated header silently ship with the example
# placeholder still in place.
$popPlaceholder = "REPLACE_WITH_LOCAL_DEV_POP"

function ConvertTo-CppString([string]$Value) {
    # See generate_dev_secrets_header.ps1's identical helper for the full
    # rationale - preserves every byte of the original value exactly, only
    # ever inserting the escape characters a C++ string literal requires.
    return $Value.Replace("\", "\\").Replace('"', '\"').Replace("`r`n", '\n').Replace("`n", '\n').Replace("`r", '\n')
}

$rootCa = $null
$certificate = $null
$privateKey = $null
$pop = $null
try {
    $endpoint = (Get-Content -LiteralPath (Join-Path $credentialPath "endpoint.txt") -Raw).Trim()
    if ($endpoint -notmatch '^[a-z0-9][a-z0-9-]{8,}-ats\.iot\.[a-z0-9-]+\.amazonaws\.com$') {
        throw "Endpoint format is invalid."
    }

    $rootCa = Get-Content -LiteralPath (Join-Path $credentialPath "AmazonRootCA1.pem") -Raw
    if ([String]::IsNullOrWhiteSpace($rootCa)) { throw "Root CA file is empty." }

    $certificate = Get-Content -LiteralPath (Join-Path $credentialPath "device-certificate.pem.crt") -Raw
    if ([String]::IsNullOrWhiteSpace($certificate)) { throw "Device certificate file is empty." }

    $privateKey = Get-Content -LiteralPath (Join-Path $credentialPath "private.pem.key") -Raw
    if ([String]::IsNullOrWhiteSpace($privateKey)) { throw "Private key file is empty." }

    $pop = (Get-Content -LiteralPath (Join-Path $credentialPath "pop.txt") -Raw).Trim()
    if ($pop -eq $popPlaceholder) {
        throw "PoP must not be the example header's placeholder value."
    }
    # Exactly 64 lowercase hex characters - the format `openssl rand -hex
    # 32` produces. Rejects anything else (wrong length, uppercase,
    # non-hex, accidental whitespace/newlines beyond the single trim
    # above) rather than trying to normalize it.
    if ($pop -notmatch '^[0-9a-f]{64}$') {
        throw "PoP must be exactly 64 lowercase hexadecimal characters (e.g. from 'openssl rand -hex 32')."
    }

    $lines = @(
        '#pragma once',
        '',
        '// Generated local DEV configuration for esp32-c3-dev-ble-mqtt. This file',
        '// must remain ignored. Deliberately has no Wi-Fi SSID/password - Wi-Fi',
        '// comes exclusively from BLE Unified Provisioning/NVS.',
        ('#define INTERBRIDGE_DEV_AWS_ENDPOINT "{0}"' -f (ConvertTo-CppString $endpoint)),
        ('#define INTERBRIDGE_DEV_DEVICE_ID "{0}"' -f (ConvertTo-CppString $DeviceId)),
        ('#define INTERBRIDGE_DEV_ROOT_CA_PEM "{0}"' -f (ConvertTo-CppString $rootCa)),
        ('#define INTERBRIDGE_DEV_CERTIFICATE_PEM "{0}"' -f (ConvertTo-CppString $certificate)),
        ('#define INTERBRIDGE_DEV_PRIVATE_KEY_PEM "{0}"' -f (ConvertTo-CppString $privateKey)),
        ('#define INTERBRIDGE_DEV_BLE_POP "{0}"' -f (ConvertTo-CppString $pop))
    )

    # Every macro this header is supposed to define must appear exactly
    # once, and neither Wi-Fi macro must ever appear - guards against a
    # future edit to this script accidentally duplicating/dropping one, or
    # reintroducing a static Wi-Fi credential this environment must never
    # have.
    $expectedMacros = @(
        "INTERBRIDGE_DEV_AWS_ENDPOINT", "INTERBRIDGE_DEV_DEVICE_ID", "INTERBRIDGE_DEV_ROOT_CA_PEM",
        "INTERBRIDGE_DEV_CERTIFICATE_PEM", "INTERBRIDGE_DEV_PRIVATE_KEY_PEM", "INTERBRIDGE_DEV_BLE_POP"
    )
    foreach ($macro in $expectedMacros) {
        $pattern = "^#define $macro "
        $matchCount = ($lines | Select-String -Pattern $pattern).Count
        if ($matchCount -ne 1) {
            throw "Internal error: $macro must be defined exactly once (found $matchCount) - refusing to write."
        }
    }
    $forbiddenMacros = @("INTERBRIDGE_DEV_WIFI_SSID", "INTERBRIDGE_DEV_WIFI_PASSWORD")
    foreach ($macro in $forbiddenMacros) {
        if (($lines | Select-String -Pattern "^#define $macro ").Count -ne 0) {
            throw "Internal error: $macro must never appear in this header - refusing to write."
        }
    }

    [IO.File]::WriteAllLines($destinationPath, $lines, [Text.UTF8Encoding]::new($false))
} finally {
    $rootCa = $null
    $certificate = $null
    $privateKey = $null
    $pop = $null
    [GC]::Collect()
}

Write-Host "DEV BLE+MQTT secrets header generated safely at the ignored destination."
Write-Host "device_id: $DeviceId (certificate/private key/PoP values, sizes, and hashes are never shown)."
