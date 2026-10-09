/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "recovery_ui/device.h"

// Interactive physical boot-partition flashing. Never changes the active slot.
void FlashPartitionImage(Device* device);
