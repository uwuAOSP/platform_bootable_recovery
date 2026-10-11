/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
namespace recovery_mtp {
// All waits are bounded. Absent device opt-in/locked storage is a no-op.
bool Start();
bool Stop();
}  // namespace recovery_mtp
