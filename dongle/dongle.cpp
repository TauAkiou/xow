/*
 * Copyright (C) 2019 Medusalix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "dongle.h"
#include "../utils/log.h"

#include <functional>

bool Dongle::afterOpen()
{
    Log::info("Dongle plugged in");

    if (!Mt76::afterOpen())
    {
        return false;
    }

    Log::info("Dongle initialized");

    return true;
}

bool Dongle::beforeClose()
{
    // Prevent controller connect/disconnect race conditions
    std::lock_guard<std::mutex> lock(handlePacketMutex);

    Log::info("Dongle power-off");

    for (std::unique_ptr<Controller> &controller : controllers)
    {
        if (controller && !controller->powerOff())
        {
            Log::error("Failed to power off controller");
        }
    }

    if (!Mt76::beforeClose())
    {
        return false;
    }

    return true;
}

void Dongle::clientConnected(uint8_t wcid, Bytes address)
{
    auto sendPacket = std::bind(
        &Dongle::sendControllerPacket,
        this,
        wcid,
        address,
        std::placeholders::_1
    );

    controllers[wcid - 1].reset(new Controller(sendPacket));

    Log::info("Controller '%d' connected", wcid);
}

void Dongle::clientDisconnected(uint8_t wcid)
{
    if (!controllers[wcid - 1])
    {
        Log::error("Controller '%d' is not connected", wcid);

        return;
    }

    controllers[wcid - 1].reset();

    Log::info("Controller '%d' disconnected", wcid);
}

void Dongle::packetReceived(uint8_t wcid, const Bytes &packet)
{
    if (!controllers[wcid - 1])
    {
        Log::error("Packet for unconnected controller '%d'", wcid);

        return;
    }

    if (!controllers[wcid - 1]->handlePacket(packet))
    {
        Log::error("Error handling packet for controller '%d'", wcid);
    }
}

bool Dongle::sendControllerPacket(
    uint8_t wcid,
    Bytes address,
    const Bytes &packet
) {
    TxWi txWi = {};

    // OFDM transmission method
    // Wait for acknowledgement
    txWi.phyType = MT_PHY_TYPE_OFDM;
    txWi.ack = 1;
    txWi.mpduByteCount = sizeof(WlanFrame) + sizeof(QosFrame) + packet.size();

    WlanFrame wlanFrame = {};

    // Frame is sent from AP (DS)
    // Duration is the time required to transmit (μs)
    wlanFrame.frameControl.type = MT_WLAN_DATA;
    wlanFrame.frameControl.subtype = MT_WLAN_QOS_DATA;
    wlanFrame.frameControl.fromDs = 1;
    wlanFrame.duration = 144;

    address.copy(wlanFrame.destination);
    macAddress.copy(wlanFrame.source);
    macAddress.copy(wlanFrame.bssId);

    QosFrame qosFrame = {};

    // Frames and data must be 32-bit aligned
    uint32_t length = sizeof(txWi) + sizeof(wlanFrame) + sizeof(qosFrame);
    uint32_t wcidData = __builtin_bswap32(wcid - 1);
    uint8_t framePadding = Bytes::padding<uint32_t>(length);
    uint8_t dataPadding = Bytes::padding<uint32_t>(packet.size());

    Bytes out;

    out.append(wcidData);
    out.pad(sizeof(uint32_t));
    out.append(txWi);
    out.append(wlanFrame);
    out.append(qosFrame);
    out.pad(framePadding);
    out.append(packet);
    out.pad(dataPadding);

    if (!sendCommand(CMD_PACKET_TX, out))
    {
        Log::error("Failed to send controller packet");

        return false;
    }

    return true;
}
