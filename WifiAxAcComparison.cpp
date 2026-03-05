#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/he-phy.h"
#include "ns3/spectrum-module.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <numbers>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

struct UserPerformance
{
    double dlThroughputMbps{0.0};
    double ulThroughputMbps{0.0};
    double totalThroughputMbps{0.0};
    double avgDelayMs{0.0};
    uint64_t txPackets{0};
    uint64_t rxPackets{0};
    double delaySumSeconds{0.0};
};

struct PerformanceMetrics
{
    std::vector<UserPerformance> perUser;
    double totalNetworkThroughputMbps{0.0};
    double jainFairness{0.0};
    double avgPacketDelayMs{0.0};
    double spectralEfficiencyBpsPerHz{0.0};
    double dropRatePercent{0.0};
    uint64_t totalTxPackets{0};
    uint64_t totalRxPackets{0};
    std::map<uint32_t, uint64_t> dropReasonsByCode;
};

double
ComputeJainFairness(const std::vector<double>& values)
{
    if (values.empty())
    {
        return 0.0;
    }

    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    const double sumSquares =
        std::accumulate(values.begin(), values.end(), 0.0, [](double acc, double v) {
            return acc + v * v;
        });

    if (sumSquares <= 0.0)
    {
        return 0.0;
    }

    return (sum * sum) / (static_cast<double>(values.size()) * sumSquares);
}

std::string
FlowDropReasonToString(uint32_t code)
{
    switch (code)
    {
    case 0:
        return "DROP_NO_ROUTE";
    case 1:
        return "DROP_TTL_EXPIRE";
    case 2:
        return "DROP_BAD_CHECKSUM";
    case 3:
        return "DROP_QUEUE";
    case 4:
        return "DROP_QUEUE_DISC";
    case 5:
        return "DROP_INTERFACE_DOWN";
    case 6:
        return "DROP_ROUTE_ERROR";
    case 7:
        return "DROP_FRAGMENT_TIMEOUT";
    default:
        return "UNKNOWN_REASON";
    }
}

PerformanceMetrics
CollectPerformanceMetrics(const std::map<FlowId, FlowMonitor::FlowStats>& stats,
                          Ptr<Ipv4FlowClassifier> classifier,
                          uint32_t nUsers,
                          uint16_t dlBasePort,
                          uint16_t ulBasePort,
                          double activeDurationSeconds,
                          uint32_t channelWidthMhz)
{
    PerformanceMetrics metrics;
    metrics.perUser.resize(nUsers);

    uint64_t totalRxBytes = 0;
    double totalDelaySeconds = 0.0;

    for (const auto& [flowId, st] : stats)
    {
        const auto tuple = classifier->FindFlow(flowId);
        if (tuple.protocol != 17) // UDP only
        {
            continue;
        }

        bool isDl = false;
        bool isUl = false;
        uint32_t userIndex = 0;

        if (tuple.destinationPort >= dlBasePort && tuple.destinationPort < dlBasePort + nUsers)
        {
            isDl = true;
            userIndex = tuple.destinationPort - dlBasePort;
        }
        else if (tuple.destinationPort >= ulBasePort && tuple.destinationPort < ulBasePort + nUsers)
        {
            isUl = true;
            userIndex = tuple.destinationPort - ulBasePort;
        }
        else
        {
            continue;
        }

        const double throughputMbps = (st.rxBytes * 8.0) / (activeDurationSeconds * 1e6);
        auto& user = metrics.perUser[userIndex];
        if (isDl)
        {
            user.dlThroughputMbps += throughputMbps;
        }
        if (isUl)
        {
            user.ulThroughputMbps += throughputMbps;
        }
        user.txPackets += st.txPackets;
        user.rxPackets += st.rxPackets;
        user.delaySumSeconds += st.delaySum.GetSeconds();

        metrics.totalTxPackets += st.txPackets;
        metrics.totalRxPackets += st.rxPackets;
        totalRxBytes += st.rxBytes;
        totalDelaySeconds += st.delaySum.GetSeconds();

        for (uint32_t reasonCode = 0; reasonCode < st.packetsDropped.size(); ++reasonCode)
        {
            if (st.packetsDropped[reasonCode] > 0)
            {
                metrics.dropReasonsByCode[reasonCode] += st.packetsDropped[reasonCode];
            }
        }
    }

    std::vector<double> userTotals;
    userTotals.reserve(nUsers);
    for (auto& user : metrics.perUser)
    {
        user.totalThroughputMbps = user.dlThroughputMbps + user.ulThroughputMbps;
        user.avgDelayMs = (user.rxPackets > 0) ? (user.delaySumSeconds * 1000.0 / user.rxPackets) : 0.0;
        userTotals.push_back(user.totalThroughputMbps);
    }

    metrics.totalNetworkThroughputMbps = (totalRxBytes * 8.0) / (activeDurationSeconds * 1e6);
    metrics.jainFairness = ComputeJainFairness(userTotals);
    metrics.avgPacketDelayMs =
        (metrics.totalRxPackets > 0) ? (totalDelaySeconds * 1000.0 / metrics.totalRxPackets) : 0.0;
    metrics.spectralEfficiencyBpsPerHz =
        (totalRxBytes * 8.0 / activeDurationSeconds) / (static_cast<double>(channelWidthMhz) * 1e6);
    metrics.dropRatePercent =
        (metrics.totalTxPackets > 0)
            ? (100.0 * static_cast<double>(metrics.totalTxPackets - metrics.totalRxPackets) /
               static_cast<double>(metrics.totalTxPackets))
            : 0.0;

    return metrics;
}

void
PrintPerformanceMetrics(const PerformanceMetrics& metrics)
{
    double totalDlThroughput = 0.0;
    double totalUlThroughput = 0.0;
    for (const auto& user : metrics.perUser)
    {
        totalDlThroughput += user.dlThroughputMbps;
        totalUlThroughput += user.ulThroughputMbps;
    }

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n--- Performance Metrics ---\n";
    // Keep these exact labels for compatibility with sweep scripts.
    std::cout << "Total DL Throughput: " << totalDlThroughput << " Mbps\n";
    std::cout << "Total UL Throughput: " << totalUlThroughput << " Mbps\n";
    std::cout << "Total Network Throughput: " << metrics.totalNetworkThroughputMbps << " Mbps\n";
    std::cout << "Jain's fairness index: " << metrics.jainFairness << "\n";
    std::cout << "Average packet delay: " << metrics.avgPacketDelayMs << " ms\n";
    std::cout << "Spectral efficiency: " << metrics.spectralEfficiencyBpsPerHz << " bps/Hz\n";
    std::cout << "Packet drop rate: " << metrics.dropRatePercent << "%\n";

    std::cout << "\nIndividual user throughput and delay:\n";
    for (uint32_t i = 0; i < metrics.perUser.size(); ++i)
    {
        const auto& user = metrics.perUser[i];
        std::cout << "User " << i << ": DL=" << user.dlThroughputMbps << " Mbps, UL="
                  << user.ulThroughputMbps << " Mbps, TOTAL=" << user.totalThroughputMbps
                  << " Mbps, delay=" << user.avgDelayMs << " ms\n";
    }

    std::cout << "\nPacket drop reasons:\n";
    if (metrics.dropReasonsByCode.empty())
    {
        std::cout << "  none observed\n";
    }
    else
    {
        for (const auto& [code, count] : metrics.dropReasonsByCode)
        {
            std::cout << "  code " << code << " (" << FlowDropReasonToString(code)
                      << "): " << count << " packets\n";
        }
    }
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string standard = "ax";
    std::string trafficMode = "both"; // downlink, uplink, both
    uint32_t nUsers = 8;
    double simTime = 10.0;
    double appStart = 1.0;
    double staDistance = 5.0;
    uint32_t channelWidthMhz = 80;
    uint8_t mcs = 5;
    double minimumUserLoadMbps = 0.0;
    double maximumUserLoadMbps = 30.0;
    uint32_t packetSize = 1200;
    const uint32_t predefinedSeed = 7;

    bool enableUlOfdma = false;
    bool enableDlOfdma = false;

    CommandLine cmd;
    cmd.AddValue("standard", "ac or ax", standard);
    cmd.AddValue("trafficMode", "Traffic mode: downlink, uplink, both", trafficMode);
    cmd.AddValue("nUsers", "Number of users", nUsers);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("appStart", "Application start time in seconds", appStart);
    cmd.AddValue("staDistance", "Deployment radius around AP in meters", staDistance);
    cmd.AddValue("channelWidth", "Channel width in MHz", channelWidthMhz);
    cmd.AddValue("mcs", "MCS index (ax:0-11, ac:0-9)", mcs);
    cmd.AddValue("minSpeed",
                 "Minimum per-user UDP offered load in Mbps",
                 minimumUserLoadMbps);
    cmd.AddValue("maxSpeed",
                 "Maximum per-user UDP offered load in Mbps",
                 maximumUserLoadMbps);
    cmd.AddValue("packetSize", "UDP payload size in bytes", packetSize);
    cmd.Parse(argc, argv);

    if (standard != "ax" && standard != "ac")
    {
        NS_ABORT_MSG("standard must be 'ax' or 'ac'");
    }
    if (nUsers == 0)
    {
        NS_ABORT_MSG("nUsers must be >= 1");
    }
    if (trafficMode != "downlink" && trafficMode != "uplink" && trafficMode != "both")
    {
        NS_ABORT_MSG("trafficMode must be one of: downlink, uplink, both");
    }
    if (simTime <= appStart)
    {
        NS_ABORT_MSG("simTime must be greater than appStart");
    }
    if (minimumUserLoadMbps < 0.0)
    {
        NS_ABORT_MSG("minSpeed must be >= 0");
    }
    if (maximumUserLoadMbps <= 0.0)
    {
        NS_ABORT_MSG("maxOfferedLoadPerUserMbps must be > 0");
    }
    if (minimumUserLoadMbps > maximumUserLoadMbps)
    {
        NS_ABORT_MSG("minSpeed must be <= maxSpeed");
    }
    if (packetSize == 0)
    {
        NS_ABORT_MSG("packetSize must be >= 1");
    }
    if (standard == "ac" && mcs > 9)
    {
        NS_ABORT_MSG("For 802.11ac, mcs must be in [0,9]");
    }
    if (standard == "ax" && mcs > 11)
    {
        NS_ABORT_MSG("For 802.11ax, mcs must be in [0,11]");
    }

    const bool hasDownlinkTraffic = (trafficMode == "downlink" || trafficMode == "both");
    const bool hasUplinkTraffic = (trafficMode == "uplink" || trafficMode == "both");

    // For AX, keep OFDMA enabled only in active traffic directions.
    if (standard == "ax")
    {
        enableDlOfdma = hasDownlinkTraffic;
        enableUlOfdma = hasUplinkTraffic;
    }

    // Reproducible RNG
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(predefinedSeed + 1);

    NodeContainer staNodes;
    staNodes.Create(nUsers);

    NodeContainer apNode;
    apNode.Create(1);

    WifiHelper wifi;
    WifiMacHelper mac;

    NetDeviceContainer staDevices;
    NetDeviceContainer apDevice;

    Ssid ssid("wifi-network");

    if (standard == "ax")
    {
        wifi.SetStandard(WIFI_STANDARD_80211ax);
    }
    else
    {
        wifi.SetStandard(WIFI_STANDARD_80211ac);
    }

    std::ostringstream channelSettings;
    channelSettings << "{0, " << channelWidthMhz << ", BAND_5GHZ, 0}";

    std::string dataMode;
    std::string controlMode;

    if (standard == "ax")
    {
        std::ostringstream data;
        data << "HeMcs" << static_cast<uint32_t>(mcs);
        dataMode = data.str();

        const auto nonHtRefRateMbps = HePhy::GetNonHtReferenceRate(mcs) / 1e6;
        std::ostringstream ctrl;
        ctrl << "OfdmRate" << nonHtRefRateMbps << "Mbps";
        controlMode = ctrl.str();
    }
    else
    {
        std::ostringstream data;
        data << "VhtMcs" << static_cast<uint32_t>(mcs);
        dataMode = data.str();
        controlMode = "OfdmRate24Mbps";
    }

    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue(dataMode),
                                 "ControlMode",
                                 StringValue(controlMode));

    auto spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());

    SpectrumWifiPhyHelper phy;
    phy.SetChannel(spectrumChannel);
    phy.Set("ChannelSettings", StringValue(channelSettings.str()));

    if (standard == "ax")
    {
        mac.SetType("ns3::StaWifiMac",
                    "Ssid", SsidValue(ssid),
                    "ActiveProbing", BooleanValue(false));

        staDevices = wifi.Install(phy, mac, staNodes);

        if (enableDlOfdma || enableUlOfdma)
        {
            const uint8_t maxStationsPerDlMu = std::min<uint8_t>(8, static_cast<uint8_t>(nUsers));
            mac.SetMultiUserScheduler("ns3::RrMultiUserScheduler",
                                      "NStations",
                                      UintegerValue(maxStationsPerDlMu),
                                      "EnableTxopSharing",
                                      BooleanValue(false),
                                      "EnableUlOfdma", BooleanValue(enableUlOfdma),
                                      "EnableBsrp", BooleanValue(false));
        }

        mac.SetType("ns3::ApWifiMac",
                    "Ssid", SsidValue(ssid));

        apDevice = wifi.Install(phy, mac, apNode);
    }
    else
    {
        mac.SetType("ns3::StaWifiMac",
                    "Ssid", SsidValue(ssid),
                    "ActiveProbing", BooleanValue(false));

        staDevices = wifi.Install(phy, mac, staNodes);

        mac.SetType("ns3::ApWifiMac",
                    "Ssid", SsidValue(ssid));

        apDevice = wifi.Install(phy, mac, apNode);
    }

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator>();
    positionAlloc->Add(Vector(0.0, 0.0, 0.0));

    Ptr<UniformRandomVariable> angleRv = CreateObject<UniformRandomVariable>();
    angleRv->SetAttribute("Min", DoubleValue(0.0));
    angleRv->SetAttribute("Max", DoubleValue(2.0 * std::numbers::pi));
    angleRv->SetStream(2);

    Ptr<UniformRandomVariable> radiusRv = CreateObject<UniformRandomVariable>();
    radiusRv->SetAttribute("Min", DoubleValue(0.0));
    radiusRv->SetAttribute("Max", DoubleValue(1.0));
    radiusRv->SetStream(3);

    for (uint32_t i = 0; i < nUsers; ++i)
    {
        const double angle = angleRv->GetValue();
        const double radius = std::sqrt(radiusRv->GetValue()) * staDistance;
        positionAlloc->Add(Vector(radius * std::cos(angle), radius * std::sin(angle), 0.0));
    }
    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);
    mobility.Install(staNodes);

    InternetStackHelper stack;
    stack.Install(staNodes);
    stack.Install(apNode);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");

    Ipv4InterfaceContainer staInterfaces = address.Assign(staDevices);
    Ipv4InterfaceContainer apInterfaces = address.Assign(apDevice);

    const uint16_t dlBasePort = 9000;
    const uint16_t ulBasePort = 19000;

    ApplicationContainer dlSinkApps;
    ApplicationContainer ulSinkApps;
    ApplicationContainer dlSourceApps;
    ApplicationContainer ulSourceApps;

    Ptr<UniformRandomVariable> rateRv = CreateObject<UniformRandomVariable>();
    rateRv->SetAttribute("Min", DoubleValue(minimumUserLoadMbps));
    rateRv->SetAttribute("Max", DoubleValue(maximumUserLoadMbps));
    rateRv->SetStream(1);

    std::vector<double> userRatesMbps;
    userRatesMbps.reserve(nUsers);
    std::vector<double> userDlRatesMbps;
    userDlRatesMbps.reserve(nUsers);
    std::vector<double> userUlRatesMbps;
    userUlRatesMbps.reserve(nUsers);

    for (uint32_t i = 0; i < nUsers; ++i)
    {
        const uint16_t dlPort = static_cast<uint16_t>(dlBasePort + i);
        const uint16_t ulPort = static_cast<uint16_t>(ulBasePort + i);

        PacketSinkHelper dlSink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), dlPort));
        PacketSinkHelper ulSink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), ulPort));

        dlSinkApps.Add(dlSink.Install(staNodes.Get(i)));
        ulSinkApps.Add(ulSink.Install(apNode.Get(0)));

        const double userRateMbps = rateRv->GetValue();
        userRatesMbps.push_back(userRateMbps);
        const bool bothDirections = hasDownlinkTraffic && hasUplinkTraffic;
        const double dlRateMbps = hasDownlinkTraffic ? (bothDirections ? userRateMbps * 0.5 : userRateMbps)
                                                     : 0.0;
        const double ulRateMbps = hasUplinkTraffic ? (bothDirections ? userRateMbps * 0.5 : userRateMbps)
                                                   : 0.0;
        userDlRatesMbps.push_back(dlRateMbps);
        userUlRatesMbps.push_back(ulRateMbps);

        const uint64_t dlRateBps = static_cast<uint64_t>(dlRateMbps * 1e6);
        const uint64_t ulRateBps = static_cast<uint64_t>(ulRateMbps * 1e6);

        if (hasDownlinkTraffic && dlRateBps > 0)
        {
            OnOffHelper dlSource("ns3::UdpSocketFactory",
                                 InetSocketAddress(staInterfaces.GetAddress(i), dlPort));
            dlSource.SetAttribute("DataRate", DataRateValue(DataRate(dlRateBps)));
            dlSource.SetAttribute("PacketSize", UintegerValue(packetSize));
            dlSource.SetAttribute("StartTime", TimeValue(Seconds(appStart)));
            dlSource.SetAttribute("StopTime", TimeValue(Seconds(simTime)));
            dlSourceApps.Add(dlSource.Install(apNode.Get(0)));
        }

        if (hasUplinkTraffic && ulRateBps > 0)
        {
            OnOffHelper ulSource("ns3::UdpSocketFactory",
                                 InetSocketAddress(apInterfaces.GetAddress(0), ulPort));
            ulSource.SetAttribute("DataRate", DataRateValue(DataRate(ulRateBps)));
            ulSource.SetAttribute("PacketSize", UintegerValue(packetSize));
            ulSource.SetAttribute("StartTime", TimeValue(Seconds(appStart)));
            ulSource.SetAttribute("StopTime", TimeValue(Seconds(simTime)));
            ulSourceApps.Add(ulSource.Install(staNodes.Get(i)));
        }
    }

    std::cout << "Using standard: " << standard << "\n";
    std::cout << "Traffic mode: " << trafficMode << "\n";
    std::cout << "Per-user offered load range: [" << minimumUserLoadMbps << ", "
              << maximumUserLoadMbps << "] Mbps\n";
    std::cout << "Packet size: " << packetSize << " B\n";
    std::cout << "Predefined seed: " << predefinedSeed << "\n";
    std::cout << "DL OFDMA enabled: " << (enableDlOfdma ? "true" : "false") << "\n";
    std::cout << "UL OFDMA enabled: " << (enableUlOfdma ? "true" : "false") << "\n";
    std::cout << "Sampled per-user offered loads (Mbps):\n";
    for (uint32_t i = 0; i < nUsers; ++i)
    {
        std::cout << "  User " << i << ": total=" << userRatesMbps[i] << ", dl="
                  << userDlRatesMbps[i] << ", ul=" << userUlRatesMbps[i] << "\n";
    }

    FlowMonitorHelper flowMonitorHelper;
    Ptr<FlowMonitor> flowMonitor = flowMonitorHelper.InstallAll();

    // Add a short cleanup period after generators stop so queued frames can drain.
    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    flowMonitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowMonitorHelper.GetClassifier());
    const auto stats = flowMonitor->GetFlowStats();

    const auto metrics = CollectPerformanceMetrics(stats,
                                                   classifier,
                                                   nUsers,
                                                   dlBasePort,
                                                   ulBasePort,
                                                   simTime - appStart,
                                                   channelWidthMhz);
    PrintPerformanceMetrics(metrics);

    Simulator::Destroy();
}