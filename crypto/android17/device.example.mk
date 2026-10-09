# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
# Copy into the device tree. This is an explicit opt-in, never a global default.
SOONG_CONFIG_NAMESPACES += recovery_crypto
SOONG_CONFIG_recovery_crypto += android17
SOONG_CONFIG_recovery_crypto_android17 := true

PRODUCT_PACKAGES += \
    librecovery_crypto_android17 \
    mydevice_recovery_crypto_profile \
    mydevice_recovery_crypto_fstab
