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

#include "controller.h"
#include "../utils/log.h"

#include <cmath>
#include <linux/input.h>

Controller::Controller(SendPacket sendPacket) :
    GipDevice(sendPacket), inputDevice(std::bind(
        &Controller::feedbackReceived,
        this,
        std::placeholders::_1,
        std::placeholders::_2
    )) {}

bool Controller::powerOff()
{
    return setPowerMode(POWER_OFF);
}

void Controller::deviceAnnounced(const AnnounceData *announce)
{
    Log::info("Product ID: %04x", announce->productId);
    Log::debug(
        "Firmware version: %d.%d.%d.%d",
        announce->firmwareVersion.build,
        announce->firmwareVersion.major,
        announce->firmwareVersion.minor,
        announce->firmwareVersion.revision
    );
    Log::debug(
        "Hardware version: %d.%d.%d.%d",
        announce->hardwareVersion.build,
        announce->hardwareVersion.major,
        announce->hardwareVersion.minor,
        announce->hardwareVersion.revision
    );

    InputDevice::AxisConfig stickConfig = {};

    // 16 bits (signed) for the sticks
    stickConfig.minimum = -32768;
    stickConfig.maximum = 32767;
    stickConfig.fuzz = 255;
    stickConfig.flat = 4095;

    InputDevice::AxisConfig triggerConfig = {};

    // 10 bits (unsigned) for the triggers
    triggerConfig.minimum = 0;
    triggerConfig.maximum = 1023;
    triggerConfig.fuzz = 3;
    triggerConfig.flat = 63;

    InputDevice::AxisConfig dpadConfig = {};

    // 1 bit for the DPAD buttons
    dpadConfig.minimum = -1;
    dpadConfig.maximum = 1;

    inputDevice.addKey(BTN_MODE);
    inputDevice.addKey(BTN_START);
    inputDevice.addKey(BTN_SELECT);
    inputDevice.addKey(BTN_A);
    inputDevice.addKey(BTN_B);
    inputDevice.addKey(BTN_X);
    inputDevice.addKey(BTN_Y);
    inputDevice.addKey(BTN_TL);
    inputDevice.addKey(BTN_TR);
    inputDevice.addKey(BTN_THUMBL);
    inputDevice.addKey(BTN_THUMBR);
    inputDevice.addAxis(ABS_X, stickConfig);
    inputDevice.addAxis(ABS_RX, stickConfig);
    inputDevice.addAxis(ABS_Y, stickConfig);
    inputDevice.addAxis(ABS_RY, stickConfig);
    inputDevice.addAxis(ABS_Z, triggerConfig);
    inputDevice.addAxis(ABS_RZ, triggerConfig);
    inputDevice.addAxis(ABS_HAT0X, dpadConfig);
    inputDevice.addAxis(ABS_HAT0Y, dpadConfig);
    inputDevice.addFeedback(FF_RUMBLE);
    inputDevice.create(
        announce->vendorId,
        announce->productId,
        "Xbox One Wireless Controller"
    );
}

void Controller::statusReceived(const StatusData *status)
{
    Log::debug(
        "Battery type: %d, level: %d",
        status->batteryType,
        status->batteryLevel
    );
}

void Controller::guideButtonPressed(const GuideButtonData *button)
{
    inputDevice.setKey(BTN_MODE, button->pressed);
    inputDevice.report();
}

void Controller::serialNumberReceived(const SerialData *serial)
{
    const std::string number(
        serial->serialNumber,
        sizeof(serial->serialNumber)
    );

    Log::info("Serial number: %s", number.c_str());
}

void Controller::inputReceived(const InputData *input)
{
    inputDevice.setKey(BTN_START, input->buttons.start);
    inputDevice.setKey(BTN_SELECT, input->buttons.select);
    inputDevice.setKey(BTN_A, input->buttons.a);
    inputDevice.setKey(BTN_B, input->buttons.b);
    inputDevice.setKey(BTN_X, input->buttons.x);
    inputDevice.setKey(BTN_Y, input->buttons.y);
    inputDevice.setKey(BTN_TL, input->buttons.bumperLeft);
    inputDevice.setKey(BTN_TR, input->buttons.bumperRight);
    inputDevice.setKey(BTN_THUMBL, input->buttons.stickLeft);
    inputDevice.setKey(BTN_THUMBR, input->buttons.stickRight);
    inputDevice.setAxis(ABS_X, input->stickLeftX);
    inputDevice.setAxis(ABS_RX, input->stickRightX);
    inputDevice.setAxis(ABS_Y, ~input->stickLeftY);
    inputDevice.setAxis(ABS_RY, ~input->stickRightY);
    inputDevice.setAxis(ABS_Z, input->triggerLeft);
    inputDevice.setAxis(ABS_RZ, input->triggerRight);
    inputDevice.setAxis(
        ABS_HAT0X,
        input->buttons.dpadRight - input->buttons.dpadLeft
    );
    inputDevice.setAxis(
        ABS_HAT0Y,
        input->buttons.dpadDown - input->buttons.dpadUp
    );
    inputDevice.report();
}

void Controller::feedbackReceived(ff_effect effect, uint16_t gain)
{
    if (effect.type != FF_RUMBLE)
    {
        return;
    }

    if (!rumbling && gain == 0)
    {
        return;
    }

    // Map Linux' magnitudes to rumble power
    uint8_t weak = static_cast<uint32_t>(
        effect.u.rumble.weak_magnitude
    ) * gain / 0xffffff;
    uint8_t strong = static_cast<uint32_t>(
        effect.u.rumble.strong_magnitude
    ) * gain / 0xffffff;

    Log::debug(
        "Feedback length: %d, delay: %d, direction: %d, weak: %d, strong: %d",
        effect.replay.length,
        effect.replay.delay,
        effect.direction,
        weak,
        strong
    );

    RumbleData rumble = {};

    rumble.motors = RUMBLE_ALL;
    rumble.left = strong;
    rumble.right = weak;
    rumble.duration = 0xff;

    // Upper half of the controller (from left to right)
    if (effect.direction >= 0x4000 && effect.direction <= 0xc000)
    {
        // Angle shifted by an eighth of a full circle
        float angle = static_cast<float>(effect.direction) / 0xffff - 0.125;
        float left = sin(2 * M_PI * angle);
        float right = cos(2 * M_PI * angle);
        uint8_t maxPower = strong > weak ? strong : weak;

        // Limit values to the left and right areas
        left = left > 0 ? left : 0;
        right = right < 0 ? -right : 0;

        // The trigger motors are very strong
        rumble.triggerLeft = left * maxPower / 4;
        rumble.triggerRight = right * maxPower / 4;
    }

    performRumble(rumble);

    rumbling = gain > 0;
}
