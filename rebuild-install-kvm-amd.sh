#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
LINUX_HOST_DIR="${SCRIPT_DIR}"
REPO_ROOT=$(cd "${LINUX_HOST_DIR}/../.." && pwd)
if [[ -z "${O_DIR:-}" ]]; then
  if [[ ${EUID} -eq 0 ]]; then
    O_DIR="/tmp/faasev-kvm-build-$(uname -r)"
  else
    O_DIR="${REPO_ROOT}/host/src"
  fi
fi
JOBS="${JOBS:-$(nproc)}"
NO_RELOAD=0
if [[ ${EUID} -eq 0 ]]; then
  SUDO_CMD=()
else
  read -r -a SUDO_CMD <<< "${SUDO:-sudo -n}"
fi

usage() {
  cat <<USAGE
Usage: $0 [--no-reload]

Environment:
  O_DIR   Kernel out dir (default: ${REPO_ROOT}/host/src)
  JOBS    Parallel build jobs (default: nproc)

What it does:
  1) Updates host kernel config for AMD KVM module build
  2) Prepares generated headers/scripts for the selected out dir
  3) Rebuilds arch/x86/kvm modules (kvm, kvm-amd)
  4) Installs modules into /lib/modules/\$(uname -r)/kernel/arch/x86/kvm
  5) Optionally reloads modules and verifies status
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-reload)
      NO_RELOAD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1
      ;;
  esac
done

RUNNING_RELEASE=$(uname -r)
RUNNING_VERMAGIC=$(modinfo -F vermagic kvm)
CONFIG_SCRIPT="${LINUX_HOST_DIR}/scripts/config"
LOCALVERSION_EXTRA=""

if [[ ! -x "${CONFIG_SCRIPT}" ]]; then
  echo "Missing config script: ${CONFIG_SCRIPT}"
  exit 1
fi

mkdir -p "${O_DIR}"

if [[ ${EUID} -ne 0 ]] && find "${O_DIR}" -xdev -uid 0 -print -quit | grep -q .; then
  echo "Build output dir has root-owned files: ${O_DIR}"
  echo "Run: sudo chown -R ${USER}:${USER} ${O_DIR}"
  exit 1
fi

if [[ ! -f "${O_DIR}/.config" ]]; then
  cp "/boot/config-${RUNNING_RELEASE}" "${O_DIR}/.config"
fi

"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --disable SYSTEM_TRUSTED_KEYS
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --disable SYSTEM_REVOCATION_KEYS
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --disable SECURITY_TOMOYO
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --disable IOMMU_SVA
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --disable INTEL_IOMMU_SVM
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --enable VIRTUALIZATION
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --module KVM
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --module KVM_AMD
"${CONFIG_SCRIPT}" --file "${O_DIR}/.config" --disable KVM_INTEL

make -C "${LINUX_HOST_DIR}" O="${O_DIR}" olddefconfig
make -C "${LINUX_HOST_DIR}" O="${O_DIR}" syncconfig

BASE_RELEASE=$(make -s -C "${LINUX_HOST_DIR}" O="${O_DIR}" kernelrelease LOCALVERSION=)
if [[ "${RUNNING_RELEASE}" == "${BASE_RELEASE}"* ]]; then
  LOCALVERSION_EXTRA="${RUNNING_RELEASE#${BASE_RELEASE}}"
else
  LOCALVERSION_EXTRA="${LOCALVERSION:-}"
fi
make -C "${LINUX_HOST_DIR}" O="${O_DIR}" syncconfig LOCALVERSION="${LOCALVERSION_EXTRA}"
make -C "${LINUX_HOST_DIR}" O="${O_DIR}" modules_prepare LOCALVERSION="${LOCALVERSION_EXTRA}"

RUNNING_SYMVERS="/lib/modules/${RUNNING_RELEASE}/build/Module.symvers"
if [[ ! -f "${RUNNING_SYMVERS}" ]]; then
  echo "Missing running-kernel symbol versions: ${RUNNING_SYMVERS}"
  exit 1
fi
cp "${RUNNING_SYMVERS}" "${O_DIR}/Module.symvers"

grep -q '^CONFIG_KVM=m$' "${O_DIR}/.config"
grep -q '^CONFIG_KVM_AMD=m$' "${O_DIR}/.config"
grep -q '^# CONFIG_KVM_INTEL is not set$' "${O_DIR}/.config"

BUILT_RELEASE=$(make -s -C "${LINUX_HOST_DIR}" O="${O_DIR}" kernelrelease LOCALVERSION="${LOCALVERSION_EXTRA}")
if [[ "${BUILT_RELEASE}" != "${RUNNING_RELEASE}" ]]; then
  echo "Running kernel (${RUNNING_RELEASE}) != built kernel (${BUILT_RELEASE}); refusing module installation."
  exit 1
fi

make -C "${LINUX_HOST_DIR}" O="${O_DIR}" M=arch/x86/kvm clean
make -C "${LINUX_HOST_DIR}" O="${O_DIR}" M=arch/x86/kvm modules -j"${JOBS}" LOCALVERSION="${LOCALVERSION_EXTRA}"

KVM_KO="${O_DIR}/arch/x86/kvm/kvm.ko"
KVM_AMD_KO="${O_DIR}/arch/x86/kvm/kvm-amd.ko"
DEST_DIR="/lib/modules/${RUNNING_RELEASE}/kernel/arch/x86/kvm"

[[ -f "${KVM_KO}" ]] || { echo "Missing ${KVM_KO}"; exit 1; }
[[ -f "${KVM_AMD_KO}" ]] || { echo "Missing ${KVM_AMD_KO}"; exit 1; }

for module in "${KVM_KO}" "${KVM_AMD_KO}"; do
  if [[ "$(modinfo -F vermagic "${module}")" != "${RUNNING_VERMAGIC}" ]]; then
    echo "Module vermagic differs from running KVM: ${module}"
    exit 1
  fi
done

if [[ ${NO_RELOAD} -eq 0 ]] && [[ "$(cat /sys/module/kvm_amd/refcnt)" != 0 ]]; then
  echo "KVM AMD is in use; refusing installation and reload. Leave the VMs running."
  exit 1
fi

"${SUDO_CMD[@]}" mkdir -p "${DEST_DIR}"
"${SUDO_CMD[@]}" install -m 0644 "${KVM_KO}" "${DEST_DIR}/kvm.ko"
"${SUDO_CMD[@]}" install -m 0644 "${KVM_AMD_KO}" "${DEST_DIR}/kvm-amd.ko"
"${SUDO_CMD[@]}" depmod -a "${RUNNING_RELEASE}"

if [[ ${NO_RELOAD} -eq 0 ]]; then
  "${SUDO_CMD[@]}" modprobe -r kvm_amd kvm || {
    echo "Failed to unload kvm modules (in use?). Stop VM processes and retry."
    exit 1
  }
  "${SUDO_CMD[@]}" modprobe kvm
  "${SUDO_CMD[@]}" modprobe kvm_amd
fi

echo "=== Verify ==="
echo "running kernel: ${RUNNING_RELEASE}"
grep -E '^CONFIG_KVM=|^CONFIG_KVM_AMD=|^CONFIG_KVM_INTEL=' "/boot/config-${RUNNING_RELEASE}" || true
lsmod | grep -E '^kvm|^kvm_amd|^kvm_intel' || true
modinfo kvm | sed -n '1,5p' || true
modinfo kvm_amd | sed -n '1,5p' || true

echo "Done."
