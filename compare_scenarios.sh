#!/usr/bin/env bash
set -euo pipefail

# Quick AX/AC sweep helper for WifiAxAcComparison
# Produces a CSV with total DL/UL/network throughput per scenario.

SIM="WifiAxAcComparison"
OUT_DIR="results"
OUT_CSV="${OUT_DIR}/compare_scenarios.csv"

N_USERS_LIST=(4 8 16 20)
TRAFFIC_MODES=(both downlink uplink)
STANDARDS=(ax ac)
SIM_TIME=10
APP_START=1
MIN_LOAD_LIST=(0 2 5 10)
MAX_LOAD_LIST=(5 10 20 30)
PACKET_SIZE=1200
MCS=5
CHANNEL_WIDTH=80
STA_DISTANCE=5

mkdir -p "${OUT_DIR}"

echo "standard,trafficMode,nUsers,minLoadMbps,maxLoadMbps,packetSize,totalDlMbps,totalUlMbps,totalNetworkMbps" > "${OUT_CSV}"

echo "[1/2] Building ${SIM}..."
./ns3 build "${SIM}"

echo "[2/2] Running sweep..."
for standard in "${STANDARDS[@]}"; do
  for mode in "${TRAFFIC_MODES[@]}"; do
    for nUsers in "${N_USERS_LIST[@]}"; do
      for minLoad in "${MIN_LOAD_LIST[@]}"; do
        for maxLoad in "${MAX_LOAD_LIST[@]}"; do
          if (( minLoad > maxLoad )); then
            continue
          fi

          cmd="${SIM} --standard=${standard} --trafficMode=${mode} --nUsers=${nUsers} --simTime=${SIM_TIME} --appStart=${APP_START} --minSpeed=${minLoad} --maxSpeed=${maxLoad} --packetSize=${PACKET_SIZE} --mcs=${MCS} --channelWidth=${CHANNEL_WIDTH} --staDistance=${STA_DISTANCE}"

          output=$(./ns3 run "${cmd}")

          dl=$(echo "${output}" | awk '/^Total DL Throughput:/ {print $(NF-1)}')
          ul=$(echo "${output}" | awk '/^Total UL Throughput:/ {print $(NF-1)}')
          net=$(echo "${output}" | awk '/^Total Network Throughput:/ {print $(NF-1)}')

          dl=${dl:-0}
          ul=${ul:-0}
          net=${net:-0}

          echo "${standard},${mode},${nUsers},${minLoad},${maxLoad},${PACKET_SIZE},${dl},${ul},${net}" >> "${OUT_CSV}"
          echo "done: standard=${standard} mode=${mode} nUsers=${nUsers} min=${minLoad} max=${maxLoad}"
        done
      done
    done
  done
done

echo "Sweep complete. Results: ${OUT_CSV}"
