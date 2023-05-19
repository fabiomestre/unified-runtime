// Copyright (C) 2022-2023 Intel Corporation
// SPDX-License-Identifier: MIT
#include <uur/fixtures.h>

using urDevicePartitionTest = uur::urAllDevicesTest;

template <class T>
struct urDevicePartitionTestWithParam : uur::urAllDevicesTest,
                                        ::testing::WithParamInterface<T> {};

void getNumberComputeUnits(ur_device_handle_t device,
                           uint32_t &n_compute_units) {
    ASSERT_SUCCESS(uur::GetDeviceMaxComputeUnits(device, n_compute_units));
    ASSERT_NE(n_compute_units, 0);
}

TEST_F(urDevicePartitionTest, PartitionEquallySuccess) {
    for (auto device : devices) {

        if (!uur::hasDevicePartitionSupport(device,
                                            UR_DEVICE_PARTITION_EQUALLY)) {
            ::testing::Message() << "Device: \'" << device
                                 << "\' does not support partitioning equally.";
            continue;
        }

        uint32_t n_compute_units = 0;
        ASSERT_NO_FATAL_FAILURE(getNumberComputeUnits(device, n_compute_units));

        for (uint32_t i = 1; i < n_compute_units; ++i) {
            ur_device_partition_property_t properties[] = {
                UR_DEVICE_PARTITION_EQUALLY, i, 0};

            // Get the number of devices that will be created
            uint32_t n_devices;
            ASSERT_SUCCESS(
                urDevicePartition(device, properties, 0, nullptr, &n_devices));
            ASSERT_NE(n_devices, 0);

            std::vector<ur_device_handle_t> sub_devices(n_devices);
            ASSERT_SUCCESS(urDevicePartition(
                device, properties, static_cast<uint32_t>(sub_devices.size()),
                sub_devices.data(), nullptr));
            for (auto sub_device : sub_devices) {
                ASSERT_NE(sub_device, nullptr);
                ASSERT_SUCCESS(urDeviceRelease(sub_device));
            }
        }
    }
}

TEST_F(urDevicePartitionTest, PartitionByCounts) {

    for (auto device : devices) {

        if (!uur::hasDevicePartitionSupport(device,
                                            UR_DEVICE_PARTITION_BY_COUNTS)) {
            ::testing::Message()
                << "Device: \'" << device
                << "\' does not support partitioning by counts.\n";
            continue;
        }

        uint32_t n_cu_in_device = 0;
        ASSERT_NO_FATAL_FAILURE(getNumberComputeUnits(device, n_cu_in_device));

        enum class Combination { ONE, HALF, ALL_MINUS_ONE, ALL };

        std::vector<Combination> combinations{Combination::ONE,
                                              Combination::ALL};

        if (n_cu_in_device >= 2) {
            combinations.push_back(Combination::HALF);
            combinations.push_back(Combination::ALL_MINUS_ONE);
        }

        uint32_t n_cu_across_sub_devices;
        for (const auto Combination : combinations) {

            std::vector<ur_device_partition_property_t> properties = {
                UR_DEVICE_PARTITION_BY_COUNTS};

            switch (Combination) {
            case Combination::ONE: {
                n_cu_across_sub_devices = 1;
                properties.insert(properties.end(), {1, 0});
                break;
            }
            case Combination::HALF: {
                n_cu_across_sub_devices = (n_cu_in_device / 2) * 2;
                properties.insert(properties.end(),
                                  {n_cu_in_device / 2, n_cu_in_device / 2, 0});
                break;
            }
            case Combination::ALL_MINUS_ONE: {
                n_cu_across_sub_devices = n_cu_in_device - 1;
                properties.insert(properties.end(), {n_cu_in_device - 1, 0});
                break;
            }
            case Combination::ALL: {
                n_cu_across_sub_devices = n_cu_in_device;
                properties.insert(properties.end(), {n_cu_in_device, 0});
                break;
            }
            }

            // Get the number of devices that will be created
            uint32_t n_devices;
            ASSERT_SUCCESS(urDevicePartition(device, properties.data(), 0,
                                             nullptr, &n_devices));
            ASSERT_EQ(n_devices, properties.size() - 2);

            std::vector<ur_device_handle_t> sub_devices(n_devices);
            ASSERT_SUCCESS(
                urDevicePartition(device, properties.data(),
                                  static_cast<uint32_t>(sub_devices.size()),
                                  sub_devices.data(), nullptr));

            uint32_t sum = 0;
            for (auto sub_device : sub_devices) {
                ASSERT_NE(sub_device, nullptr);
                uint32_t n_cu_in_sub_device;
                ASSERT_NO_FATAL_FAILURE(
                    getNumberComputeUnits(sub_device, n_cu_in_sub_device));
                sum += n_cu_in_sub_device;
                ASSERT_SUCCESS(urDeviceRelease(sub_device));
            }
            ASSERT_EQ(n_cu_across_sub_devices, sum);
        }
    }
}

using urDevicePartitionAffinityDomainTest =
    urDevicePartitionTestWithParam<ur_device_affinity_domain_flags_t>;
TEST_P(urDevicePartitionAffinityDomainTest, PartitionByAffinityDomain) {

    for (auto device : devices) {

        if (!uur::hasDevicePartitionSupport(
                device, UR_DEVICE_PARTITION_BY_AFFINITY_DOMAIN)) {
            ::testing::Message() << "Device \'" << device
                                 << "\' does not support partitioning by "
                                    "affinity domain.\n";
            continue;
        }

        uint32_t n_compute_units = 0;
        ASSERT_NO_FATAL_FAILURE(getNumberComputeUnits(device, n_compute_units));

        // Skip if the affinity domain is not supported by device
        ur_device_affinity_domain_flags_t flag = GetParam();
        ur_device_affinity_domain_flags_t supported_flags{0};
        ASSERT_SUCCESS(uur::GetDevicePartitionAffinityDomainFlags(
            device, supported_flags));
        if (!(flag & supported_flags)) {
            ::testing::Message()
                << static_cast<ur_device_affinity_domain_flag_t>(flag)
                << " is not supported by the device: \'" << device << "\'.\n";
            continue;
        }

        std::vector<ur_device_partition_property_t> properties = {
            UR_DEVICE_PARTITION_BY_AFFINITY_DOMAIN, flag, 0};

        // Get the number of devices that will be created
        uint32_t n_devices = 0;
        ASSERT_SUCCESS(urDevicePartition(device, properties.data(), 0, nullptr,
                                         &n_devices));
        ASSERT_NE(n_devices, 0);

        std::vector<ur_device_handle_t> sub_devices(n_devices);
        ASSERT_SUCCESS(
            urDevicePartition(device, properties.data(),
                              static_cast<uint32_t>(sub_devices.size()),
                              sub_devices.data(), nullptr));

        for (auto sub_device : sub_devices) {
            ASSERT_NE(sub_device, nullptr);
            ASSERT_SUCCESS(urDeviceRelease(sub_device));
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    , urDevicePartitionAffinityDomainTest,
    ::testing::Values(UR_DEVICE_AFFINITY_DOMAIN_FLAG_NUMA,
                      UR_DEVICE_AFFINITY_DOMAIN_FLAG_L4_CACHE,
                      UR_DEVICE_AFFINITY_DOMAIN_FLAG_L3_CACHE,
                      UR_DEVICE_AFFINITY_DOMAIN_FLAG_L2_CACHE,
                      UR_DEVICE_AFFINITY_DOMAIN_FLAG_L1_CACHE,
                      UR_DEVICE_AFFINITY_DOMAIN_FLAG_NEXT_PARTITIONABLE),
    [](const ::testing::TestParamInfo<ur_device_affinity_domain_flags_t>
           &info) {
        std::stringstream ss;
        ss << static_cast<ur_device_affinity_domain_flag_t>(info.param);
        return ss.str();
    });

TEST_F(urDevicePartitionTest, InvalidNullHandleDevice) {
    ur_device_partition_property_t props[] = {UR_DEVICE_PARTITION_EQUALLY, 1,
                                              0};
    ur_device_handle_t sub_device = nullptr;
    ASSERT_EQ_RESULT(
        UR_RESULT_ERROR_INVALID_NULL_HANDLE,
        urDevicePartition(nullptr, props, 1, &sub_device, nullptr));
}

TEST_F(urDevicePartitionTest, InvalidNullPointerProperties) {
    for (auto device : devices) {
        ur_device_handle_t sub_device = nullptr;
        ASSERT_EQ_RESULT(
            UR_RESULT_ERROR_INVALID_NULL_POINTER,
            urDevicePartition(device, nullptr, 1, &sub_device, nullptr));
    }
}

TEST_F(urDevicePartitionTest, SuccessSubSet) {
    for (auto device : devices) {

        if (!uur::hasDevicePartitionSupport(device,
                                            UR_DEVICE_PARTITION_EQUALLY)) {
            ::testing::Message() << "Device: \'" << device
                                 << "\' does not support partitioning equally.";
            continue;
        }

        uint32_t n_compute_units = 0;
        ASSERT_NO_FATAL_FAILURE(getNumberComputeUnits(device, n_compute_units));

        // partition for 1 compute unit per sub-device
        ur_device_partition_property_t properties[] = {
            UR_DEVICE_PARTITION_EQUALLY, 1, 0};

        // Get the number of devices that will be created
        uint32_t n_devices;
        ASSERT_SUCCESS(
            urDevicePartition(device, properties, 0, nullptr, &n_devices));
        ASSERT_NE(n_devices, 0);

        // We can request only a subset of these devices from [0, n_devices]
        for (size_t subset = 0; subset <= n_devices; ++subset) {
            std::vector<ur_device_handle_t> sub_devices(subset);
            ASSERT_SUCCESS(urDevicePartition(
                device, properties, static_cast<uint32_t>(sub_devices.size()),
                sub_devices.data(), nullptr));
            for (auto sub_device : sub_devices) {
                ASSERT_NE(sub_device, nullptr);
                ASSERT_SUCCESS(urDeviceRelease(sub_device));
            }
        }
    }
}
