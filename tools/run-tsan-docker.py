"""Run Linux x86_64 TSan with process-local ASLR control, without host sysctls.
Requires Python 3 and Docker. Keeps a cached builder image and ignored logs;
uses a temporary Linux volume for builds and removes it on exit.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build-tsan-docker"
IMAGE = "teder-tsan-builder:ubuntu24.04"
BASE = "ubuntu@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac"
PROFILE_URL = "https://raw.githubusercontent.com/moby/profiles/4b8214fc4971e06172938a1f1554ccff6647ee80/seccomp/default.json"
PROFILE_SHA256 = "6416b47770785a41ac59073cdc77d9fe98517df2799dc83ef207e622de3053f6"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def main():
    platform = run("docker", "version", "--format", "{{.Server.Os}}/{{.Server.Arch}}",
                   capture_output=True, text=True).stdout.strip()
    if platform != "linux/amd64":
        raise SystemExit(f"This TSan runner requires native Linux/amd64 Docker, got {platform}.")
    OUTPUT.mkdir(exist_ok=True)
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
    profile["syscalls"].append({
        "names": ["personality"], "action": "SCMP_ACT_ALLOW",
        "args": [{"index": 0, "value": 262144, "op": "SCMP_CMP_EQ"}],
        "comment": "Allow only Linux ADDR_NO_RANDOMIZE for this disposable TSan container.",
    })
    policy = OUTPUT / "seccomp-tsan.json"
    with policy.open("w", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps(profile, indent=2) + "\n")
    dockerfile = f"""FROM {BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates build-essential cmake ninja-build util-linux
LABEL org.teder.purpose="tsan-builder"
"""
    # Stdin supplies only the Dockerfile; the source tree is never a build context.
    run("docker", "build", "--tag", IMAGE, "-", input=dockerfile, text=True)
    volume = "teder-tsan-" + uuid.uuid4().hex
    run("docker", "volume", "create", volume, stdout=subprocess.DEVNULL)
    common = ["docker", "run", "--rm", "--cpus", "4", "--memory", "4g",
              "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
              "--mount", f"type=bind,source={ROOT},target=/src,readonly",
              "--mount", f"type=bind,source={OUTPUT},target=/results",
              "--mount", f"type=volume,source={volume},target=/work"]
    build = """set -euo pipefail
{ uname -a; gcc --version; cmake --version; dpkg-query -W libtsan2 libc6; cat /proc/sys/vm/mmap_rnd_bits; } > /results/environment.txt
cmake -S /src -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTEDER_WERROR=ON -DTEDER_SANITIZER=thread > /results/configure.log 2>&1
cmake --build /work/build --parallel 3 > /results/build.log 2>&1
"""
    test = """set -euo pipefail
ctest --test-dir /work/build --output-on-failure 2>&1 | tee /results/test.log
"""
    try:
        # Fetch pinned dependencies using the default policy. Test offline, with
        # only the exact personality argument added to the pinned Moby profile.
        run(*common, "--name", volume + "-build", IMAGE, "bash", "-c", build)
        run(*common, "--name", volume + "-test", "--network", "none", "--security-opt", f"seccomp={policy}",
            IMAGE, "setarch", "x86_64", "-R", "bash", "-c", test)
    finally:
        # Docker's attached client may be interrupted while its container lives.
        # These UUID names belong only to this run; --rm normally removed them.
        for name in (volume + "-build", volume + "-test"):
            subprocess.run(["docker", "rm", "--force", name],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Preserve detailed evidence even when CTest fails, before removing storage.
        try:
            run(*common, "--network", "none", IMAGE, "bash", "-c",
                "if [ -d /work/build ]; then\n"
                "  if [ -d /work/build/Testing ]; then cp -R /work/build/Testing /results/; fi\n"
                "  find /work/build -name 'control-*.log' -exec cp {} /results/ ';'\nfi")
        finally:
            run("docker", "volume", "rm", volume, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
