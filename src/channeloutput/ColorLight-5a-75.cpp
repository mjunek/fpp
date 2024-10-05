/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the GPL v2 as described in the
 * included LICENSE.GPL file.
 */

/*
 *   Packet Format: (info based on mplayer ColorLight 5a-75 video output patch)
 *
 *   0x0101 Packet: (send first)
 *      This is the "display frame buffer" packet.
 *      - Data Length:     98
 *      - Destination MAC: 11:22:33:44:55:66
 *      - Source MAC:      22:22:33:44:55:66
 *      - Ether Type:      0x0101 (have also seen 0x0100, 0x0104, 0x0107.
 *      - Data[0-end]:     0x00
 *
 *      The following has been discovered Oct 2024 in later LEDVision Traces. These are all 0x0107 EtherType packets
 *      - Data[21]:        Display Brightness. eg:
 *              0x0D: 05% || 0x1A: 10% || 0x40: 25% || 0x80: 50% || 0xBF: 75% || 0xff: 100%
 *      - Data[22]:        0x05
 *      - Data[23]:        0x00
 *      - Data[24]:        Linear Brightness for Red (These three are used for Colour Temperature adjustment)
 *      - Data[25]:        Linear Brightness for Green
 *      - Data[26]:        Linear Brightness for Blue
 *              2000K at 10% brightness: 0x1a, 0x0c, 0x01
 *              6500K at 10% brightness: 0x1a, 0x1a, 0x1a
 *              2000K at 100% brightness: 0xff, 0x76, 0x06
 *              4500K at 100% brightness: 0xff, 0xdc, 0x8f
 *              6500K at 100% brightness: 0xff, 0xff, 0xff
 *              8000K at 100% brightness: 0xce, 0xd8, 0xff
 *
 *
 *   0x0AFF Packet: (send second, but not at all in some captures)
 *      This is the "Set Brightness" packet. Unsure how it differs from the above
 *      - Data Length:     63
 *      - Destination MAC: 11:22:33:44:55:66
 *      - Source MAC:      22:22:33:44:55:66
 *      - Ether Type:      0x0AFF  // The last two octets here also set brightness on some panels.See Data[21] in the above frame packet.
 *      - Data[0]:         0xFF    // Red Brightness
 *      - Data[1]:         0xFF    // Green Brightness
 *      - Data[2]:         0xFF    // Blue Brightness
 *      - Data[3-end]:     0x00
 *
 *   Row data packets: (send one packet for each row of display)
 *      - Data Length:     (Row_Width * 3) + 7
 *   	- Destination MAC: 11:22:33:44:55:66
 *   	- Source MAC:      22:22:33:44:55:66
 *   	- Ether Type:      0x5500 + MSB of Row Number
 *   	                     0x5500 for rows 0-255
 *   	                     0x5501 for rows 256-511
 *   	- Data[0]:         Row Number LSB
 *   	- Data[1]:         MSB of pixel offset for this packet
 *   	- Data[2]:         LSB of pixel offset for this packet
 *   	- Data[3]:         MSB of pixel count in packet
 *   	- Data[4]:         LSB of pixel count in packet
 *   	- Data[5]:         0x08 - ?? unsure what this is
 *   	- Data[6]:         0x80 - ?? unsure what this is
 *   	- Data[7-end]:     RGB order pixel data
 *
 *   Sample data packets seen in captures:
 *           0  1  2  3  4  5  6
 *     55 00 00 00 00 01 F1 00 00 (first 497 pixels on a 512 wide display)
 *     55 00 00 01 F1 00 0F 00 00 (last 15 pixels on a 512 wide display)
 *     55 00 00 00 00 01 20 08 88 (288 pixel wide display)
 *     55 00 00 00 00 00 80 08 88 (128 pixel wide display)
 *
 */
#include "fpp-pch.h"

#ifndef PLATFORM_OSX
#include <linux/if_packet.h>
#include <netinet/ether.h>
#else
#include <net/bpf.h>
#endif

#include "../SysSocket.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <cmath>
#include <errno.h>
#include <fstream>
#include <iostream>

#include "../Warnings.h"
#include "../common.h"
#include "../log.h"

#include "ColorLight-5a-75.h"
#include "overlays/PixelOverlay.h"

#include "Plugin.h"
class ColorLight5a75Plugin : public FPPPlugins::Plugin, public FPPPlugins::ChannelOutputPlugin {
public:
    ColorLight5a75Plugin() :
        FPPPlugins::Plugin("ColorLight5a75") {
    }
    virtual ChannelOutput* createChannelOutput(unsigned int startChannel, unsigned int channelCount) override {
        return new ColorLight5a75Output(startChannel, channelCount);
    }
};

extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new ColorLight5a75Plugin();
}
}

/*
 *
 */
ColorLight5a75Output::ColorLight5a75Output(unsigned int startChannel, unsigned int channelCount) :
    ChannelOutput(startChannel, channelCount),
    m_width(0),
    m_height(0),
    m_colorOrder(FPPColorOrder::kColorOrderRGB),
    m_fd(-1),
    m_rowSize(0),
    m_panelWidth(0),
    m_panelHeight(0),
    m_panels(0),
    m_rows(0),
    m_outputs(0),
    m_longestChain(0),
    m_invertedData(0),
    m_matrix(NULL),
    m_panelMatrix(NULL),
    m_slowCount(0),
    m_flippedLayout(0) {
    LogDebug(VB_CHANNELOUT, "ColorLight5a75Output::ColorLight5a75Output(%u, %u)\n",
             startChannel, channelCount);
}

/*
 *
 */
ColorLight5a75Output::~ColorLight5a75Output() {
    LogDebug(VB_CHANNELOUT, "ColorLight5a75Output::~ColorLight5a75Output()\n");

    for (int i = 0; i < m_iovecs.size(); i++) {
        free(m_iovecs[i].iov_base);

        if (i >= 4)
            i++; // first 4 are header+data, only headers for the rest
    }

    if (m_fd >= 0)
        close(m_fd);

    if (m_outputFrame)
        delete[] m_outputFrame;
}

/*
 *
 */
int ColorLight5a75Output::Init(Json::Value config) {
    LogDebug(VB_CHANNELOUT, "ColorLight5a75Output::Init(JSON)\n");

    m_panelWidth = config["panelWidth"].asInt();
    m_panelHeight = config["panelHeight"].asInt();

    if (!m_panelWidth)
        m_panelWidth = 32;

    if (!m_panelHeight)
        m_panelHeight = 16;

    m_invertedData = config["invertedData"].asInt();

    m_colorOrder = ColorOrderFromString(config["colorOrder"].asString());

    m_panelMatrix =
        new PanelMatrix(m_panelWidth, m_panelHeight, m_invertedData);

    if (!m_panelMatrix) {
        LogErr(VB_CHANNELOUT, "Unable to create PanelMatrix\n");
        return 0;
    }

    if (config.isMember("cfgVersion")) {
        m_flippedLayout = config["cfgVersion"].asInt() >= 2 ? 0 : 1;
    } else {
        m_flippedLayout = 1;
    }

    for (int i = 0; i < config["panels"].size(); i++) {
        Json::Value p = config["panels"][i];
        char orientation = 'N';
        std::string o = p["orientation"].asString();
        if (o != "") {
            orientation = o[0];
        }

        if (m_flippedLayout) {
            switch (orientation) {
            case 'N':
                orientation = 'U';
                break;
            case 'U':
                orientation = 'N';
                break;
            case 'R':
                orientation = 'L';
                break;
            case 'L':
                orientation = 'R';
                break;
            }
        }

        if (p["colorOrder"].asString() == "")
            p["colorOrder"] = ColorOrderToString(m_colorOrder);

        m_panelMatrix->AddPanel(p["outputNumber"].asInt(),
                                p["panelNumber"].asInt(), orientation,
                                p["xOffset"].asInt(), p["yOffset"].asInt(),
                                ColorOrderFromString(p["colorOrder"].asString()));

        if (p["outputNumber"].asInt() > m_outputs)
            m_outputs = p["outputNumber"].asInt();

        if (p["panelNumber"].asInt() > m_longestChain)
            m_longestChain = p["panelNumber"].asInt();
    }

    // Both of these are 0-based, so bump them up by 1 for comparisons
    m_outputs++;
    m_longestChain++;

    m_panels = m_panelMatrix->PanelCount();

    m_rows = m_outputs * m_panelHeight;

    m_width = m_panelMatrix->Width();
    m_height = m_panelMatrix->Height();

    m_channelCount = m_width * m_height * 3;

    m_outputFrame = new char[m_outputs * m_longestChain * m_panelHeight * m_panelWidth * 3];

    m_matrix = new Matrix(m_startChannel, m_width, m_height);

    if (config.isMember("subMatrices")) {
        for (int i = 0; i < config["subMatrices"].size(); i++) {
            Json::Value sm = config["subMatrices"][i];

            m_matrix->AddSubMatrix(
                sm["enabled"].asInt(),
                sm["startChannel"].asInt() - 1,
                sm["width"].asInt(),
                sm["height"].asInt(),
                sm["xOffset"].asInt(),
                sm["yOffset"].asInt());
        }
    }

    float gamma = 1.0;
    if (config.isMember("gamma")) {
        gamma = atof(config["gamma"].asString().c_str());
    }
    if (gamma < 0.01 || gamma > 50.0) {
        gamma = 1.0;
    }
    for (int x = 0; x < 256; x++) {
        float f = x;
        f = 255.0 * pow(f / 255.0f, gamma);
        if (f > 255.0) {
            f = 255.0;
        }
        if (f < 0.0) {
            f = 0.0;
        }
        m_gammaCurve[x] = round(f);
    }

    if (config.isMember("interface"))
        m_ifName = config["interface"].asString();
    else
        m_ifName = "eth1";

    m_rowSize = m_longestChain * m_panelWidth * 3;

#ifndef PLATFORM_OSX

    // Check if interface is up
    std::ifstream ifstate_src("/sys/class/net/" + m_ifName + "/operstate");
    std::string ifstate;

    if (ifstate_src.is_open()) {
        ifstate_src >> ifstate; // pipe file's content into stream
        ifstate_src.close();
    }

    if (ifstate != "up") {
        LogErr(VB_CHANNELOUT, "Error ColorLight: Configured interface %s does not have link %s\n", m_ifName.c_str(), strerror(errno));
        WarningHolder::AddWarning("ColorLight: Configured interface " + m_ifName + " does not have link");
        return 0;
    }

    // Check interface is 1000Mbps capable and display error if not
    std::ifstream ifspeed_src("/sys/class/net/" + m_ifName + "/speed");

    if (ifspeed_src.is_open()) { // always check whether the file is open
        ifspeed_src >> ifspeed;  // pipe file's content into stream
        ifspeed_src.close();
    }

    if (ifspeed < 1000) {
        LogErr(VB_CHANNELOUT, "Error ColorLight: Configured interface %s is not 1000Mbps Capable: %s\n", m_ifName.c_str(), strerror(errno));
        WarningHolder::AddWarning("ColorLight: Configured interface " + m_ifName + " is not 1000Mbps Capable");
        return 0;
    }

    // Open our raw socket
    if ((m_fd = socket(AF_PACKET, SOCK_RAW, IPPROTO_RAW)) == -1) {
        LogErr(VB_CHANNELOUT, "Error creating raw socket: %s\n", strerror(errno));
        WarningHolder::AddWarning("ColorLight: Error creating raw socket");
        return 0;
    }

    // Get the output interface ID
    memset(&m_if_idx, 0, sizeof(struct ifreq));
    strcpy(m_if_idx.ifr_name, m_ifName.c_str());
    if (ioctl(m_fd, SIOCGIFINDEX, &m_if_idx) < 0) {
        LogErr(VB_CHANNELOUT, "Error getting index of %s interface: %s\n",
               m_ifName.c_str(), strerror(errno));
        WarningHolder::AddWarning("ColorLight: Error getting index of interface " + m_ifName);
        return 0;
    }

    m_sock_addr.sll_family = AF_PACKET;
    m_sock_addr.sll_ifindex = m_if_idx.ifr_ifindex;
    m_sock_addr.sll_halen = ETH_ALEN;

    unsigned char dhost[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    memcpy(m_sock_addr.sll_addr, dhost, 6);

    // Force packets out the desired interface
    if ((bind(m_fd, (struct sockaddr*)&m_sock_addr, sizeof(m_sock_addr))) == -1) {
        LogErr(VB_CHANNELOUT, "bind() failed\n");
        WarningHolder::AddWarning("ColorLight: Could not bind to interface " + m_ifName);
        return 0;
    }
#else
    char buf[11] = { 0 };
    int i = 0;
    for (int i = 0; i < 255; i++) {
        snprintf(buf, sizeof(buf), "/dev/bpf%i", i);
        m_fd = open(buf, O_RDWR);
        if (m_fd != -1) {
            break;
        }
    }
    if (m_fd == -1) {
        LogErr(VB_CHANNELOUT, "Error opening bpf file: %s\n", strerror(errno));
        return 0;
    }

    struct ifreq bound_if;
    memset(&bound_if, 0, sizeof(bound_if));
    strcpy(bound_if.ifr_name, m_ifName.c_str());
    if (ioctl(m_fd, BIOCSETIF, &bound_if) > 0) {
        LogErr(VB_CHANNELOUT, "Cannot bind bpf device to physical device %s, exiting\n", m_ifName.c_str());
    }
    int yes = 1;
    ioctl(m_fd, BIOCSHDRCMPLT, &yes);
#endif

    int packetCount = 2 + (m_rows * (((int)(m_rowSize - 1) / CL5A75_MAX_CHANNELS_PER_PACKET) + 1));
    m_msgs.resize(packetCount);
    m_iovecs.resize(packetCount * 2);

    unsigned int p = 0;
    unsigned char* header = nullptr;
    unsigned char* data = nullptr;

    // First Init packet
    header = (unsigned char*)malloc(sizeof(struct ether_header));
    memset(header, 0, sizeof(struct ether_header));
    m_eh = (struct ether_header*)header;
    m_eh->ether_type = htons(0x0101);
    SetHostMACs(header);
    m_iovecs[p * 2].iov_base = header;
    m_iovecs[p * 2].iov_len = sizeof(struct ether_header);

    data = (unsigned char*)malloc(CL5A75_0101_PACKET_DATA_LEN);
    memset(data, 0, CL5A75_0101_PACKET_DATA_LEN);
    m_iovecs[p * 2 + 1].iov_base = data;
    m_iovecs[p * 2 + 1].iov_len = CL5A75_0101_PACKET_DATA_LEN;
    p++;

    // Second Init packet
    header = (unsigned char*)malloc(sizeof(struct ether_header));
    memset(header, 0, sizeof(struct ether_header));
    m_eh = (struct ether_header*)header;
    m_eh->ether_type = htons(0x0AFF);
    SetHostMACs(header);
    m_iovecs[p * 2].iov_base = header;
    m_iovecs[p * 2].iov_len = sizeof(struct ether_header);

    data = (unsigned char*)malloc(CL5A75_0AFF_PACKET_DATA_LEN);
    memset(data, 0, CL5A75_0AFF_PACKET_DATA_LEN);
    data[0] = 0xff;
    data[1] = 0xff;
    data[2] = 0xff;
    m_iovecs[p * 2 + 1].iov_base = data;
    m_iovecs[p * 2 + 1].iov_len = CL5A75_0AFF_PACKET_DATA_LEN;
    p++;

    char* rowPtr = (char*)m_outputFrame;
    unsigned int dSize = 0;
    unsigned int part = 0;
    unsigned int hSize = sizeof(struct ether_header) + CL5A75_HEADER_LEN;
    unsigned int offset = 0;
    unsigned int bytesInPacket = 0;
    unsigned int pixelOffset = 0;
    unsigned int pixelsInPacket = 0;

    for (int r = 0; r < m_rows; r++) {
        part = 0;
        offset = 0;

        while (offset < m_rowSize) {
            header = (unsigned char*)malloc(hSize);
            memset(header, 0, hSize);
            m_eh = (struct ether_header*)header;

            m_eh->ether_type = htons(0x5500 + (int)(r / 256));
            SetHostMACs(header);

            data = header + sizeof(struct ether_header);
            data[0] = r % 256;

            if ((offset + CL5A75_MAX_CHANNELS_PER_PACKET) > m_rowSize)
                bytesInPacket = m_rowSize - offset;
            else
                bytesInPacket = CL5A75_MAX_CHANNELS_PER_PACKET;

            pixelOffset = offset / 3;
            pixelsInPacket = bytesInPacket / 3;

            data[1] = pixelOffset >> 8;      // Pixel Offset MSB
            data[2] = pixelOffset & 0xFF;    // Pixel Offset LSB
            data[3] = pixelsInPacket >> 8;   // Pixels In Packet MSB
            data[4] = pixelsInPacket & 0xFF; // Pixels In Packet LSB

            data[5] = 0x08; // ?? still not sure what this value is
            data[6] = 0x80; // ?? still not sure what this value is

            m_iovecs[p * 2].iov_base = header;
            m_iovecs[p * 2].iov_len = hSize;
            m_iovecs[p * 2 + 1].iov_base = rowPtr + offset;
            m_iovecs[p * 2 + 1].iov_len = bytesInPacket;

            offset += bytesInPacket;
            part++;
            p++;
        }

        rowPtr += m_rowSize;
    }

    for (int m = 0; m < packetCount; m++) {
        struct mmsghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_hdr.msg_iov = &m_iovecs[m * 2];
        msg.msg_hdr.msg_iovlen = 2;
        m_msgs[m] = msg;
    }
    if (PixelOverlayManager::INSTANCE.isAutoCreatePixelOverlayModels()) {
        std::string dd = "LED Panels";
        if (config.isMember("description")) {
            dd = config["description"].asString();
        }
        std::string desc = dd;
        int count = 0;
        while (PixelOverlayManager::INSTANCE.getModel(desc) != nullptr) {
            count++;
            desc = dd + "-" + std::to_string(count);
        }
        PixelOverlayManager::INSTANCE.addAutoOverlayModel(desc,
                                                          m_startChannel, m_channelCount, 3,
                                                          "H", m_invertedData ? "BL" : "TL",
                                                          m_height, 1);
    }
    return ChannelOutput::Init(config);
}

/*
 *
 */
int ColorLight5a75Output::Close(void) {
    LogDebug(VB_CHANNELOUT, "ColorLight5a75Output::Close()\n");

    return ChannelOutput::Close();
}

void ColorLight5a75Output::GetRequiredChannelRanges(const std::function<void(int, int)>& addRange) {
    addRange(m_startChannel, m_startChannel + m_channelCount - 1);
}

void ColorLight5a75Output::OverlayTestData(unsigned char* channelData, int cycleNum, float percentOfCycle, int testType, const Json::Value& config) {
    for (int output = 0; output < m_outputs; output++) {
        int panelsOnOutput = m_panelMatrix->m_outputPanels[output].size();
        for (int i = 0; i < panelsOnOutput; i++) {
            int panel = m_panelMatrix->m_outputPanels[output][i];
            int chain = m_panelMatrix->m_panels[panel].chain;

            if (m_flippedLayout)
                chain = (m_longestChain - 1) - m_panelMatrix->m_panels[panel].chain - 1;

            m_panelMatrix->m_panels[panel].drawTestPattern(channelData + m_startChannel, cycleNum, testType);
            m_panelMatrix->m_panels[panel].drawNumber(output + 1, m_panelWidth / 2 + 1, m_panelHeight > 16 ? 2 : 1, channelData + m_startChannel);
            m_panelMatrix->m_panels[panel].drawNumber(chain + 1, m_panelWidth / 2 + 8, m_panelHeight > 16 ? 2 : 1, channelData + m_startChannel);
        }
    }
}

/*
 *
 */
void ColorLight5a75Output::PrepData(unsigned char* channelData) {
    m_matrix->OverlaySubMatrices(channelData);

    unsigned char* r = NULL;
    unsigned char* g = NULL;
    unsigned char* b = NULL;
    unsigned char* s = NULL;
    unsigned char* dst = NULL;
    int pw3 = m_panelWidth * 3;

    channelData += m_startChannel; // FIXME, this function gets offset 0

    for (int output = 0; output < m_outputs; output++) {
        int panelsOnOutput = m_panelMatrix->m_outputPanels[output].size();

        for (int i = 0; i < panelsOnOutput; i++) {
            int panel = m_panelMatrix->m_outputPanels[output][i];
            int chain = (m_longestChain - 1) - m_panelMatrix->m_panels[panel].chain;

            if (m_flippedLayout)
                chain = m_panelMatrix->m_panels[panel].chain;

            for (int y = 0; y < m_panelHeight; y++) {
                int px = chain * m_panelWidth;
                int yw = y * m_panelWidth * 3;

                dst = (unsigned char*)(m_outputFrame + (((((output * m_panelHeight) + y) * m_panelWidth * m_longestChain) + px) * 3));

                for (int x = 0; x < pw3; x += 3) {
                    *(dst++) = m_gammaCurve[channelData[m_panelMatrix->m_panels[panel].pixelMap[yw + x]]];
                    *(dst++) = m_gammaCurve[channelData[m_panelMatrix->m_panels[panel].pixelMap[yw + x + 1]]];
                    *(dst++) = m_gammaCurve[channelData[m_panelMatrix->m_panels[panel].pixelMap[yw + x + 2]]];

                    px++;
                }
            }
        }
    }
}

int ColorLight5a75Output::sendMessages(struct mmsghdr* msgs, int msgCount) {
#ifdef PLATFORM_OSX
    char buf[1500];
    for (int m = 0; m < msgCount; m++) {
        int cur = 0;
        for (int io = 0; io < msgs[m].msg_hdr.msg_iovlen; io++) {
            memcpy(&buf[cur], msgs[m].msg_hdr.msg_iov[io].iov_base, msgs[m].msg_hdr.msg_iov[io].iov_len);
            cur += msgs[m].msg_hdr.msg_iov[io].iov_len;
        }
        int bytes_sent = write(m_fd, buf, cur);
        if (bytes_sent != cur) {
            return m;
        }
    }
    return msgCount;
#else
    return sendmmsg(m_fd, msgs, msgCount, MSG_DONTWAIT);
#endif
}

/*
 *
 */
int ColorLight5a75Output::SendData(unsigned char* channelData) {
    LogExcess(VB_CHANNELOUT, "ColorLight5a75Output::SendData(%p)\n", channelData);

    long long startTime = GetTimeMS();
    struct mmsghdr* msgs = &m_msgs[0];
    int msgCount = m_msgs.size();
    if (msgCount == 0)
        return 0;

    errno = 0;
    int oc = sendMessages(msgs, msgCount);
    int outputCount = 0;
    if (oc > 0) {
        outputCount = oc;
    }

    int errCount = 0;
    bool done = false;
    while ((outputCount != msgCount) && !done) {
        errCount++;
        errno = 0;
        oc = sendMessages(&msgs[outputCount], msgCount - outputCount);
        if (oc > 0) {
            outputCount += oc;
        }
        if (outputCount != msgCount) {
            long long tm = GetTimeMS();
            long long totalTime = tm - startTime;
            if (totalTime < 22) {
                // we'll keep trying for up to 22ms, but give the network stack some time to flush some buffers
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            } else {
                done = true;
            }
        }
    }
    long long endTime = GetTimeMS();
    long long totalTime = endTime - startTime;
    if (outputCount != msgCount) {
        int tti = (int)totalTime;
        LogWarn(VB_CHANNELOUT, "sendmmsg() failed for ColorLight output (Socket: %d   output count: %d/%d   time: %dms) with error: %d   %s, errorcount: %d\n",
                m_fd, outputCount, msgCount, tti, errno, strerror(errno), errCount);
        m_slowCount++;
        if (m_slowCount > 3) {
            LogWarn(VB_CHANNELOUT, "Repeated frames taking more than 20ms to send to ColorLight");
            WarningHolder::AddWarningTimeout("Repeated frames taking more than 20ms to send to ColorLight", 30);
        }
    } else {
        m_slowCount = 0;
    }
    return m_channelCount;
}

/*
 *
 */
void ColorLight5a75Output::DumpConfig(void) {
    LogDebug(VB_CHANNELOUT, "ColorLight5a75Output::DumpConfig()\n");

    LogDebug(VB_CHANNELOUT, "    Width          : %d\n", m_width);
    LogDebug(VB_CHANNELOUT, "    Height         : %d\n", m_height);
    LogDebug(VB_CHANNELOUT, "    Rows           : %d\n", m_rows);
    LogDebug(VB_CHANNELOUT, "    Row Size       : %d\n", m_rowSize);
    LogDebug(VB_CHANNELOUT, "    m_fd           : %d\n", m_fd);
    LogDebug(VB_CHANNELOUT, "    Outputs        : %d\n", m_outputs);
    LogDebug(VB_CHANNELOUT, "    Longest Chain  : %d\n", m_longestChain);
    LogDebug(VB_CHANNELOUT, "    Inverted Data  : %d\n", m_invertedData);
    LogDebug(VB_CHANNELOUT, "    Interface      : %s\n", m_ifName.c_str());

    ChannelOutput::DumpConfig();
}

/*
 *
 */
void ColorLight5a75Output::SetHostMACs(void* ptr) {
    struct ether_header* eh = (struct ether_header*)ptr;

    // Set the source MAC address
    eh->ether_shost[0] = 0x22;
    eh->ether_shost[1] = 0x22;
    eh->ether_shost[2] = 0x33;
    eh->ether_shost[3] = 0x44;
    eh->ether_shost[4] = 0x55;
    eh->ether_shost[5] = 0x66;

    // Set the dest MAC address
    eh->ether_dhost[0] = 0x11;
    eh->ether_dhost[1] = 0x22;
    eh->ether_dhost[2] = 0x33;
    eh->ether_dhost[3] = 0x44;
    eh->ether_dhost[4] = 0x55;
    eh->ether_dhost[5] = 0x66;
}
