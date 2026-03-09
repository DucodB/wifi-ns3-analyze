#!/usr/bin/env bash
set -euo pipefail

# Quick AX/AC sweep helper for WifiAxAcComparison
# Produces a CSV with total DL/UL/network throughput per scenario.

SIM="WifiAxAcComparison"
OUT_DIR="results"
OUT_CSV="${OUT_DIR}/compare_scenarios.csv"
OUT_LOG="${OUT_DIR}/compare_scenarios_full.log"

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

echo "standard,trafficMode,nUsers,minLoadMbps,maxLoadMbps,packetSize,totalDlMbps,totalUlMbps,totalNetworkMbps,jainFairness,avgDelayMs,spectralEfficiencyBpsPerHz,packetDropRatePercent" > "${OUT_CSV}"
echo "" > "${OUT_LOG}"

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
          fairness=$(echo "${output}" | awk -F': ' '/^Jain.*fairness index:/ {print $2}')
          delay=$(echo "${output}" | awk '/^Average packet delay:/ {print $(NF-1)}')
          spectral=$(echo "${output}" | awk '/^Spectral efficiency:/ {print $(NF-1)}')
          drop=$(echo "${output}" | awk -F': ' '/^Packet drop rate:/ {gsub(/%/, "", $2); print $2}')

          dl=${dl:-0}
          ul=${ul:-0}
          net=${net:-0}
          fairness=${fairness:-0}
          delay=${delay:-0}
          spectral=${spectral:-0}
          drop=${drop:-0}

          echo "${standard},${mode},${nUsers},${minLoad},${maxLoad},${PACKET_SIZE},${dl},${ul},${net},${fairness},${delay},${spectral},${drop}" >> "${OUT_CSV}"

          {
            echo "===== standard=${standard} mode=${mode} nUsers=${nUsers} min=${minLoad} max=${maxLoad} ====="
            echo "command: ./ns3 run \"${cmd}\""
            echo "${output}"
            echo
          } >> "${OUT_LOG}"

          echo "done: standard=${standard} mode=${mode} nUsers=${nUsers} min=${minLoad} max=${maxLoad}"
        done
      done
    done
  done
done

echo "Sweep complete. Summary CSV: ${OUT_CSV}"
echo "Sweep complete. Full logs: ${OUT_LOG}"
