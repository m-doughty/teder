"""Run one Linux configuration of tools/ci.py in Docker, on the host's native architecture.

Requires Python 3 and Docker serving linux/amd64 or linux/arm64. A cached image from a pinned
Ubuntu 24.04 index adds the GitHub runner's toolchain packages. The build phase fetches sources
with the network; the test phase runs offline, with a hardened seccomp profile. Builds use a
temporary Docker volume, removed on exit. Logs and CTest results stay in build-ci/results.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build-ci"
CONFIGURATIONS = ("linux-shared", "linux-static", "linux-asan", "linux-tsan")
# A multi-arch OCI index; Docker selects the manifest for the daemon's architecture.
BASE = "ubuntu@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac"
# llvm-18 supplies the llvm-symbolizer that Clang's sanitizer runtimes use for stack traces.
PACKAGES = ("ca-certificates git build-essential cmake ninja-build python3 pkg-config util-linux "
            "clang-18 libclang-rt-18-dev llvm-18 libunistring-dev libdeflate-dev libncurses-dev "
            "ncurses-base")
PROFILE_URL = "https://raw.githubusercontent.com/moby/profiles/4b8214fc4971e06172938a1f1554ccff6647ee80/seccomp/default.json"
PROFILE_SHA256 = "6416b47770785a41ac59073cdc77d9fe98517df2799dc83ef207e622de3053f6"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def run_logged(log, *args):
    """Run a command, streaming its combined output to the terminal and to log."""
    print("+ " + shlex.join(args), flush=True)
    # Bytes pass through unchanged, whatever the console's encoding.
    with log.open("wb") as output, subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        for line in process.stdout:
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
            output.write(line)
    return process.returncode


def test_policy():
    original = OUTPUT / "seccomp-default.json"
    if original.exists():
        raw = original.read_bytes()
    else:
        with urllib.request.urlopen(PROFILE_URL, timeout=30) as response:
            raw = response.read()
    if hashlib.sha256(raw).hexdigest() != PROFILE_SHA256:
        raise SystemExit("Unexpected upstream seccomp profile hash; refusing to use it.")
    original.write_bytes(raw)
    profile = json.loads(raw)
    if profile["defaultAction"] != "SCMP_ACT_ERRNO":
        raise SystemExit("Expected a deny-by-default seccomp profile.")
    # setarch -R needs personality(ADDR_NO_RANDOMIZE), which the default profile denies.
    profile["syscalls"].append({
        "names": ["personality"], "action": "SCMP_ACT_ALLOW",
        "args": [{"index": 0, "value": 262144, "op": "SCMP_CMP_EQ"}],
        "comment": "Allow only Linux ADDR_NO_RANDOMIZE for this disposable test container.",
    })
    policy = OUTPUT / "seccomp-test.json"
    with policy.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps(profile, indent=2) + "\n")
    return policy


def builder_image(arch):
    dockerfile = f"""FROM {BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends {PACKAGES}
LABEL org.teder.purpose="ci-builder"
"""
    image = f"teder-ci-builder:{arch}-{hashlib.sha256(dockerfile.encode()).hexdigest()[:16]}"
    present = subprocess.run(["docker", "image", "inspect", image],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if present.returncode != 0:
        # Stdin supplies only the Dockerfile; the source tree is never a build context.
        run("docker", "build", "--platform", f"linux/{arch}", "--tag", image, "-",
            input=dockerfile, text=True)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("configuration", choices=CONFIGURATIONS)
    args = parser.parse_args()
    platform = run("docker", "version", "--format", "{{.Server.Os}}/{{.Server.Arch}}",
                   capture_output=True, text=True).stdout.strip()
    if platform not in ("linux/amd64", "linux/arm64"):
        raise SystemExit(f"This runner requires native Linux/amd64 or Linux/arm64 Docker, got {platform}.")
    arch = platform.split("/")[1]
    OUTPUT.mkdir(exist_ok=True)
    policy = test_policy()
    image = builder_image(arch)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    results = OUTPUT / "results" / f"{args.configuration}-{arch}-{stamp}"
    results.mkdir(parents=True)

    volume = "teder-ci-" + uuid.uuid4().hex
    run("docker", "volume", "create", volume, stdout=subprocess.DEVNULL)
    common = ["docker", "run", "--rm", "--platform", f"linux/{arch}", "--cpus", "4", "--memory", "4g",
              "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
              "--env", "PYTHONDONTWRITEBYTECODE=1",
              "--mount", f"type=bind,source={ROOT},target=/src,readonly",
              "--mount", f"type=volume,source={volume},target=/work"]
    ci = ["python3", "-B", "/src/tools/ci.py", args.configuration, "--build-root", "/work"]
    status = 0
    try:
        # Fetch pinned sources using the default policy. Test offline, with only the exact
        # personality argument added to the pinned Moby profile.
        status = run_logged(results / "build.log", *common, "--name", volume + "-build", image,
                            *ci, "--phase", "build")
        if status == 0:
            status = run_logged(results / "test.log", *common, "--name", volume + "-test",
                                "--network", "none", "--security-opt", f"seccomp={policy}",
                                image, *ci, "--phase", "test")
    finally:
        # Docker's attached client may be interrupted while its container lives.
        # These UUID names belong only to this run; --rm normally removed them.
        for name in (volume + "-build", volume + "-test"):
            subprocess.run(["docker", "rm", "--force", name],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Preserve detailed evidence even when a phase fails, before removing storage.
        try:
            copy = (f"cd /work/{args.configuration} 2>/dev/null || exit 0\n"
                    "for tree in build consumer; do\n"
                    "  if [ -d $tree/Testing ]; then\n"
                    "    mkdir -p /results/$tree && cp -R $tree/Testing /results/$tree/\n"
                    "  fi\n"
                    "done\n"
                    "if [ -d build ]; then find build -name 'control-*.log' -exec cp {} /results/ ';'; fi\n")
            # On a Linux host, copied files belong to the invoking user rather than root.
            owner = ["--user", f"{os.getuid()}:{os.getgid()}"] if hasattr(os, "getuid") else []
            run(*common, *owner, "--network", "none",
                "--mount", f"type=bind,source={results},target=/results", image, "bash", "-c", copy)
        finally:
            run("docker", "volume", "rm", volume, stdout=subprocess.DEVNULL)
    print(f"Logs and CTest results: {results}", flush=True)
    if status != 0:
        print(f"{args.configuration} failed with status {status}", file=sys.stderr, flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
