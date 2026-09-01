#!/usr/bin/env python3
"""Reject tracked DEV secrets and obvious live AWS credential material."""

from pathlib import Path
import re
import subprocess
import sys

FORBIDDEN_TRACKED_PATH = "include/interbridge_dev_secrets.h"
PATTERNS = {
    "private key PEM": re.compile(
        rb"-----BEGIN ((?:RSA |EC )?PRIVATE KEY)-----\s+[A-Za-z0-9+/=\r\n]+-----END \1-----"
    ),
    "certificate PEM": re.compile(
        rb"-----BEGIN (CERTIFICATE)-----\s+[A-Za-z0-9+/=\r\n]+-----END \1-----"
    ),
    "AWS IoT ATS endpoint": re.compile(
        rb"\b[a-z0-9]{10,}-ats\.iot\.[a-z0-9-]+\.amazonaws\.com\b", re.IGNORECASE
    ),
    # Restrict numeric matching to AWS identifiers so ordinary 12-digit
    # setup-code test fixtures do not trigger false positives.
    "AWS Account ID": re.compile(
        rb"(?:arn:aws[^\r\n]{0,100}:\d{12}:|aws[_ -]?account[_ -]?id\D{0,8}\d{12}\b)",
        re.IGNORECASE,
    ),
}


def tracked_files() -> list[str]:
    output = subprocess.check_output(["git", "ls-files", "-z"])
    return [item.decode() for item in output.split(b"\0") if item]


def main() -> int:
    failures: list[str] = []
    for filename in tracked_files():
        # This checker necessarily names the forbidden markers in its own
        # source; those pattern definitions are not credential material.
        if filename == "scripts/check_repo_safety.py":
            continue
        if filename == FORBIDDEN_TRACKED_PATH:
            failures.append(f"tracked local secrets file: {filename}")
            continue
        path = Path(filename)
        if not path.is_file():
            continue
        data = path.read_bytes()
        for label, pattern in PATTERNS.items():
            if pattern.search(data):
                failures.append(f"{label} found in {filename}")

    example = Path("include/interbridge_dev_secrets.example.h").read_text()
    definitions = re.findall(r'^#define INTERBRIDGE_DEV_[A-Z_]+ "([^"]+)"$', example, re.MULTILINE)
    if len(definitions) != 7 or any(not value.startswith("REPLACE_WITH_") for value in definitions):
        failures.append("DEV secrets example must contain exactly seven obvious REPLACE_WITH_ placeholders")

    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", FORBIDDEN_TRACKED_PATH], check=False
    ).returncode == 0
    if not ignored:
        failures.append("local DEV secrets header must be ignored")

    ignore_text = Path(".gitignore").read_text()
    for extension in ("*.pem", "*.key", "*.crt"):
        if extension not in ignore_text:
            failures.append(f"credential extension is not ignored: {extension}")

    generator = Path("scripts/generate_dev_secrets_header.ps1").read_text()
    required_generator_fragments = (
        "Read-Host \"Wi-Fi password\" -AsSecureString",
        "git -C $repoRoot check-ignore",
        r'''.Replace("`r`n", '\n')''',
        'private.pem.key',
        'ZeroFreeBSTR',
    )
    if any(fragment not in generator for fragment in required_generator_fragments):
        failures.append("DEV header generator is missing a required safety/escaping control")
    if 'R\"' in generator or "@'" in generator or '@"' in generator:
        failures.append("DEV header generator must not emit multiline raw/here strings")

    # The two DEV bench entry points must share exactly one command-processing
    # composition (see src/dev/dev_command_environment.h) rather than each
    # hand-copying RemoteCommandProcessor/CommandHandler themselves - that
    # hand-copying is exactly how esp32-c3-dev-ring-simulator once ended up
    # silently missing command processing entirely (see
    # docs/dev-ring-simulator.md > "Command processing"). Check both entry
    # points, not just one, so that specific class of gap cannot recur
    # unnoticed in either of them.
    shared_command_environment = Path("src/dev/dev_command_environment.h").read_text()
    required_shared_composition = (
        "InMemoryDedupCache dedupCache_",
        "DisabledHardware hardware_",
        "Intercom intercom_",
        "DisabledSystemControl systemControl_",
        "CommandHandler handler_",
        "RemoteCommandProcessor processor_",
    )
    if any(fragment not in shared_command_environment for fragment in required_shared_composition):
        failures.append("Shared DEV command composition is missing a required class")

    required_dev_composition = (
        "Esp32AwsIotTransport transport",
        "DeviceCredentialStore credentials",
        "DevCommandEnvironment commandEnv",
        "transport.poll()",
        "commandEnv.subscribe()",
        "commandEnv.processPending()",
    )
    forbidden_dev_composition = (
        "MQTTClient",
        "WiFiClientSecure",
        "DevMqttSmokeHandler",
        "mqtt_smoke_handler",
    )
    for dev_entrypoint_path in ("src/dev/mqtt_smoke_main.cpp", "src/dev/dev_ring_simulator_main.cpp"):
        dev_entrypoint = Path(dev_entrypoint_path).read_text()
        if any(fragment not in dev_entrypoint for fragment in required_dev_composition):
            failures.append(f"{dev_entrypoint_path} is missing the shared DEV command composition")
        if any(fragment in dev_entrypoint for fragment in forbidden_dev_composition):
            failures.append(f"{dev_entrypoint_path} contains a forbidden parallel MQTT implementation")

    if failures:
        print("Repository credential safety check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("Repository credential safety check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
