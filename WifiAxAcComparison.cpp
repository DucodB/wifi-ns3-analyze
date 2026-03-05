#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/he-phy.h"
#include "ns3/spectrum-module.h"

#include <cmath>
#include <numbers>
#include <sstream>

using namespace ns3;

int
main(int argc, char* argv[])
{
    std::string standard = "ax";
    std::string trafficMode = "mixed"; // mixed, downlink, uplink, both
    uint32_t nUsers = 8;
    double simTime = 10.0;
    double appStart = 1.0;
    double staDistance = 5.0;
    uint32_t channelWidthMhz = 80;
    uint8_t mcs = 5;
    uint32_t packetSize = 1200;
    double minOfferedLoadPerUserMbps = 5.0;
    double maxOfferedLoadPerUserMbps = 25.0;

    bool enableUlOfdma = false;
    bool enableDlOfdma = false;

    CommandLine cmd;
    cmd.AddValue("standard", "ac or ax", standard);
    cmd.AddValue("trafficMode", "Traffic mode: mixed, downlink, uplink, both", trafficMode);
    cmd.AddValue("nUsers", "Number of users", nUsers);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("appStart", "Application start time in seconds", appStart);
    cmd.AddValue("staDistance", "Distance from AP to each STA in meters", staDistance);
    cmd.AddValue("channelWidth", "Channel width in MHz", channelWidthMhz);
    cmd.AddValue("mcs", "MCS index (ax:0-11, ac:0-9)", mcs);
    cmd.AddValue("packetSize", "UDP packet size in bytes", packetSize);
    cmd.AddValue("minOfferedLoadPerUserMbps",
                 "Minimum random offered UDP load per user in Mbps",
                 minOfferedLoadPerUserMbps);
    cmd.AddValue("maxOfferedLoadPerUserMbps",
                 "Maximum random offered UDP load per user in Mbps",
                 maxOfferedLoadPerUserMbps);
    cmd.Parse(argc, argv);

    if (standard != "ax" && standard != "ac")
    {
        NS_ABORT_MSG("standard must be 'ax' or 'ac'");
    }
    if (nUsers == 0)
    {
        NS_ABORT_MSG("nUsers must be >= 1");
    }
    if (trafficMode != "mixed" && trafficMode != "downlink" && trafficMode != "uplink" &&
        trafficMode != "both")
    {
        NS_ABORT_MSG("trafficMode must be one of: mixed, downlink, uplink, both");
    }
    if (packetSize == 0)
    {
        NS_ABORT_MSG("packetSize must be >= 1");
    }
    if (simTime <= appStart)
    {
        NS_ABORT_MSG("simTime must be greater than appStart");
    }
    if (minOfferedLoadPerUserMbps <= 0.0 || maxOfferedLoadPerUserMbps <= 0.0)
    {
        NS_ABORT_MSG("Offered load bounds must be > 0");
    }
    if (maxOfferedLoadPerUserMbps < minOfferedLoadPerUserMbps)
    {
        NS_ABORT_MSG("maxOfferedLoadPerUserMbps must be >= minOfferedLoadPerUserMbps");
    }
    if (standard == "ac" && mcs > 9)
    {
        NS_ABORT_MSG("For 802.11ac, mcs must be in [0,9]");
    }
    if (standard == "ax" && mcs > 11)
    {
        NS_ABORT_MSG("For 802.11ax, mcs must be in [0,11]");
    }

    uint32_t nBothUsers = 0;
    uint32_t nDownlinkUsers = 0;
    uint32_t nUplinkUsers = 0;
    if (trafficMode == "mixed")
    {
        // Near-even 3-way split: both, DL-only, UL-only.
        nBothUsers = nUsers / 3;
        nDownlinkUsers = nUsers / 3;
        nUplinkUsers = nUsers / 3;
        uint32_t remainder = nUsers - (nBothUsers + nDownlinkUsers + nUplinkUsers);
        if (remainder > 0)
        {
            nBothUsers++;
            remainder--;
        }
        if (remainder > 0)
        {
            nDownlinkUsers++;
            remainder--;
        }
        if (remainder > 0)
        {
            nUplinkUsers++;
        }
    }

    // For this scenario, enabling 11ax always enables both UL and DL OFDMA.
    if (standard == "ax")
    {
        enableDlOfdma = true;
        enableUlOfdma = true;
    }

    // Reproducible RNG
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(1);

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
            mac.SetMultiUserScheduler("ns3::RrMultiUserScheduler",
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
    for (uint32_t i = 0; i < nUsers; ++i)
    {
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(nUsers);
        positionAlloc->Add(Vector(staDistance * std::cos(angle), staDistance * std::sin(angle), 0.0));
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
    rateRv->SetAttribute("Min", DoubleValue(minOfferedLoadPerUserMbps));
    rateRv->SetAttribute("Max", DoubleValue(maxOfferedLoadPerUserMbps));
    rateRv->SetStream(1);

    std::vector<double> userRatesMbps;
    userRatesMbps.reserve(nUsers);
    std::vector<bool> userHasDlFlow;
    std::vector<bool> userHasUlFlow;
    userHasDlFlow.reserve(nUsers);
    userHasUlFlow.reserve(nUsers);

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
        const uint64_t userRateBps = static_cast<uint64_t>(userRateMbps * 1e6);

        bool enableDlForUser = false;
        bool enableUlForUser = false;
        if (trafficMode == "downlink")
        {
            enableDlForUser = true;
        }
        else if (trafficMode == "uplink")
        {
            enableUlForUser = true;
        }
        else if (trafficMode == "both")
        {
            enableDlForUser = true;
            enableUlForUser = true;
        }
        else // mixed
        {
            if (i < nBothUsers)
            {
                enableDlForUser = true;
                enableUlForUser = true;
            }
            else if (i < (nBothUsers + nDownlinkUsers))
            {
                enableDlForUser = true;
                enableUlForUser = false;
            }
            else
            {
                enableDlForUser = false;
                enableUlForUser = true;
            }
        }

        userHasDlFlow.push_back(enableDlForUser);
        userHasUlFlow.push_back(enableUlForUser);

        if (enableDlForUser)
        {
            OnOffHelper dlSource("ns3::UdpSocketFactory",
                                 InetSocketAddress(staInterfaces.GetAddress(i), dlPort));
            dlSource.SetAttribute("DataRate", DataRateValue(DataRate(userRateBps)));
            dlSource.SetAttribute("PacketSize", UintegerValue(packetSize));
            dlSource.SetAttribute("StartTime", TimeValue(Seconds(appStart)));
            dlSource.SetAttribute("StopTime", TimeValue(Seconds(simTime)));
            dlSourceApps.Add(dlSource.Install(apNode.Get(0)));
        }

        if (enableUlForUser)
        {
            OnOffHelper ulSource("ns3::UdpSocketFactory",
                                 InetSocketAddress(apInterfaces.GetAddress(0), ulPort));
            ulSource.SetAttribute("DataRate", DataRateValue(DataRate(userRateBps)));
            ulSource.SetAttribute("PacketSize", UintegerValue(packetSize));
            ulSource.SetAttribute("StartTime", TimeValue(Seconds(appStart)));
            ulSource.SetAttribute("StopTime", TimeValue(Seconds(simTime)));
            ulSourceApps.Add(ulSource.Install(staNodes.Get(i)));
        }
    }

    std::cout << "Using standard: " << standard << "\n";
    std::cout << "Traffic mode: " << trafficMode << "\n";
    if (trafficMode == "mixed")
    {
        std::cout << "Mixed split: " << nBothUsers << " both, " << nDownlinkUsers
                  << " DL-only, " << nUplinkUsers << " UL-only users\n";
    }
    std::cout << "DL OFDMA enabled: " << (enableDlOfdma ? "true" : "false") << "\n";
    std::cout << "UL OFDMA enabled: " << (enableUlOfdma ? "true" : "false") << "\n";
    std::cout << "User offered loads (Mbps):\n";
    for (uint32_t i = 0; i < nUsers; ++i)
    {
        std::string role;
        if (userHasDlFlow[i] && userHasUlFlow[i])
        {
            role = "DL+UL";
        }
        else if (userHasDlFlow[i])
        {
            role = "DL";
        }
        else
        {
            role = "UL";
        }
        std::cout << "  User " << i << " (" << role << "): " << userRatesMbps[i] << "\n";
    }

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    std::cout << "\n--- Throughput Results ---\n";

    double totalDlThroughput = 0;
    double totalUlThroughput = 0;
    const double activeDuration = simTime - appStart;

    for (uint32_t i = 0; i < nUsers; ++i)
    {
        Ptr<PacketSink> dlSink = DynamicCast<PacketSink>(dlSinkApps.Get(i));
        Ptr<PacketSink> ulSink = DynamicCast<PacketSink>(ulSinkApps.Get(i));

        const double dlThroughput = userHasDlFlow[i] ? (dlSink->GetTotalRx() * 8.0) / (activeDuration * 1e6)
                                                     : 0.0;
        const double ulThroughput = userHasUlFlow[i] ? (ulSink->GetTotalRx() * 8.0) / (activeDuration * 1e6)
                                                     : 0.0;

        std::cout << "User " << i << ": DL=" << dlThroughput << " Mbps, UL=" << ulThroughput
                  << " Mbps\n";

        totalDlThroughput += dlThroughput;
        totalUlThroughput += ulThroughput;
    }

    std::cout << "\nTotal DL Throughput: " << totalDlThroughput << " Mbps\n";
    std::cout << "Total UL Throughput: " << totalUlThroughput << " Mbps\n";
    std::cout << "Total Network Throughput: " << (totalDlThroughput + totalUlThroughput)
              << " Mbps\n";

    Simulator::Destroy();
}